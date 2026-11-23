#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_system.h>
#include <esp_freertos_hooks.h>
#include <time.h>
#include "protocol.h"
#include "shared_config.h"
#include "firmware_version.h"
#include "languages/en.h"
#include "languages/sk.h"
#include "web_ui.h"

using namespace bridge;
static volatile uint32_t idleTicks = 0;
static uint32_t maxIdleTicks = 0;
static float cpuLoad = 0.0f;
static bool idleHook(void) { idleTicks++; return true; }
HardwareSerial nanoLink(1);
WebServer server(80);
DNSServer dns;
Preferences prefs;

struct Sample { uint32_t sec; int16_t temp10; int16_t set10; uint8_t heater; uint8_t lid; };
struct Activity { uint32_t sec; uint32_t epoch; int32_t value; char type[20]; };
struct HeaterEvent { uint32_t sec; uint8_t on; uint8_t baseline; };
static Sample history[1080];
static Activity activities[100];
static HeaterEvent heaterEvents[200];
static uint16_t histHead=0, histCount=0, activityHead=0, activityCount=0;
static uint16_t heaterHead=0, heaterCount=0;
static uint32_t bootMs=0, configWaitStartedMs=0, lastSampleMs=0, lastControlMs=0, lastNanoFrameMs=0, lastHelloMs=0;
static uint32_t seq=0, lastNanoSeq=0, lastAckSeq=0, settingsCommandSeq=0, completed=0, openingEvents=0;
static float tempC=NAN;
static bool lidClosed=false, heaterOn=false, sensorFault=true, nanoUp=false, nanoHeartbeatReady=false, runEnabled=false;
static int targetC=WAFFLE_DEFAULT_TEMPERATURE_C, durationS=WAFFLE_DEFAULT_BAKE_TIME_S;
static uint8_t bakeState=0; // 0 idle, 1 countdown, 2 overtime
static bool initialPreheatDone=false;
static uint32_t bakeStartedMs=0, bakeElapsedS=0, overtimeS=0;
static uint32_t lidOpenStartMs=0;
static uint32_t settingsChangedMs=0;
static bool encoderTimeMode=false, settingsDirty=false;
static bool haveHeaterState=false;
static bool previousNanoUp=false;
static bool haveNanoSeq=false, nanoSessionStarted=false, lastAckOk=false, settingsSyncPending=false;
static uint64_t bakeDurationSum=0, lidOpenDurationSum=0;
static uint32_t bakeDurationN=0, lidOpenDurationN=0;
static uint32_t completedTimes[11]; static uint8_t completedTimesN=0, completedTimesHead=0;
static char rxLine[kMaxLineLength]; static uint8_t rxLen=0;
static bool apMode=false, updatePasswordSet=false, arduinoOtaStarted=false;
static bool stationConnecting=false;
static uint32_t stationStartedMs=0;
static bool stationWasConnected=false, wifiLedState=false, nanoLinkLogInitialized=false, lastLoggedNanoUp=false;
static uint32_t lastWifiLedMs=0;
static bool importTargetPending=false, importDurationPending=false;
static String uiPassword;
static String setupApSsid;
static String activeLanguage="en";
static String deviceHostname="oblatkovac";
static bool uiAuth=false, otaAuth=true, ntpEnabled=true, staticIpEnabled=false;
static String ntpServerName="pool.ntp.org", timezoneName="Europe/Bratislava", timeFormat="24";
static String staticIpText="", subnetText="255.255.255.0", gatewayText="", dnsText="";
static bool timeConfigStarted=false;

static void sendNanoHello();
static void sendNanoState();
static void sendNanoSettings();

static uint8_t crc8(const char *s,size_t n){uint8_t c=0;while(n--){c^=(uint8_t)*s++;for(uint8_t b=0;b<8;b++)c=(c&0x80)?(uint8_t)((c<<1)^0x07):(uint8_t)(c<<1);}return c;}
static void sendFrame(const char *payload){nanoLink.print(payload);nanoLink.print('*');char h[3];snprintf(h,sizeof(h),"%02X",crc8(payload,strlen(payload)));nanoLink.println(h);
#if WAFFLE_DEBUG
  Serial.printf("[ESP TX] %s*%s\n",payload,h);
#endif
}
static bool validFrame(char *s){char *p=strrchr(s,'*');if(!p||strlen(p)!=3)return false;if(crc8(s,p-s)!=(uint8_t)strtoul(p+1,nullptr,16))return false;*p='\0';return true;}
static bool authorized(){if(!uiAuth)return true;if(!server.authenticate("admin",uiPassword.c_str())){server.requestAuthentication();return false;}return true;}
static bool otaAuthorized(){if(!otaAuth)return true;if(!updatePasswordSet){server.send(403,"text/plain","OTA login is enabled but no password is configured");return false;}if(!server.authenticate("admin",uiPassword.c_str())){server.requestAuthentication();return false;}return true;}
static uint32_t uptimeS(){return (millis()-bootMs)/1000;}
static String jsonEscape(const String &value);
static void setWifiLed(bool on){digitalWrite(8,on?LOW:HIGH);} // Super Mini blue LED is active-low on GPIO8.

static void addActivity(const char *type,int32_t value=0){Activity &a=activities[activityHead];a.sec=uptimeS();time_t now=time(nullptr);a.epoch=now>1600000000?(uint32_t)now:0;a.value=value;strncpy(a.type,type,sizeof(a.type)-1);a.type[sizeof(a.type)-1]='\0';activityHead=(activityHead+1)%100;if(activityCount<100)activityCount++;}
static void addHeaterEvent(uint32_t sec,bool on,bool baseline=false){heaterEvents[heaterHead]={sec,(uint8_t)(on?1:0),(uint8_t)(baseline?1:0)};heaterHead=(heaterHead+1)%200;if(heaterCount<200)heaterCount++;}
static void addSample(){Sample &s=history[histHead];s.sec=uptimeS();s.temp10=isnan(tempC)?-32768:(int16_t)(tempC*10);s.set10=(int16_t)(targetC*10);s.heater=heaterOn;s.lid=lidClosed?0:1;histHead=(histHead+1)%1080;if(histCount<1080)histCount++;}
static float avgBake(){return bakeDurationN?(float)bakeDurationSum/bakeDurationN:NAN;}
static float avgOpen(){return lidOpenDurationN?(float)lidOpenDurationSum/lidOpenDurationN:NAN;}
static float ratePerHour(){if(completedTimesN<2)return NAN;uint8_t intervals=completedTimesN-1;uint64_t sum=0;for(uint8_t i=0;i<intervals;i++){uint8_t a=(completedTimesHead+11-completedTimesN+i)%11;uint8_t b=(a+1)%11;sum+=(uint32_t)(completedTimes[b]-completedTimes[a]);}float mean=(float)sum/intervals;return mean>0?3600.0f/mean:NAN;}
static void completeBake(){uint32_t now=uptimeS();uint32_t total=(millis()-bakeStartedMs)/1000;completed++;bakeDurationSum+=total;bakeDurationN++;completedTimes[completedTimesHead]=now;completedTimesHead=(completedTimesHead+1)%11;if(completedTimesN<11)completedTimesN++;addActivity("bake_complete",total);Serial.printf("[ESP] Bake completed: count=%lu, duration=%lu s\n",(unsigned long)completed,(unsigned long)total);bakeState=0;bakeElapsedS=0;overtimeS=0;}

static void parseNano(char *line){if(!validFrame(line)){Serial.printf("[ESP CRC ERR] %s\n",line);return;}char *save=nullptr;char *kind=strtok_r(line,",",&save);char *seqStr=strtok_r(nullptr,",",&save);if(!kind||!seqStr)return;
  if(!strcmp(kind,"ACK")){lastAckSeq=strtoul(seqStr,nullptr,10);char *result=strtok_r(nullptr,",",&save);lastAckOk=result&&!strcmp(result,"OK");if(!lastAckOk)Serial.printf("[ESP] Nano rejected command sequence %lu\n",(unsigned long)lastAckSeq);else{lastNanoFrameMs=millis();if(settingsSyncPending&&lastAckSeq==settingsCommandSeq)settingsSyncPending=false;}return;}
  if(strcmp(kind,"IO")&&strcmp(kind,"EV"))return;uint32_t incomingSeq=strtoul(seqStr,nullptr,10);if(haveNanoSeq&&(int32_t)(incomingSeq-lastNanoSeq)<=0){if((int32_t)(lastNanoSeq-incomingSeq)>50){lastNanoSeq=incomingSeq;}else{return;}}lastNanoSeq=incomingSeq;haveNanoSeq=true;lastNanoFrameMs=millis();
  if(!strcmp(kind,"IO")){char *t=strtok_r(nullptr,",",&save);char *closed=strtok_r(nullptr,",",&save);char *heat=strtok_r(nullptr,",",&save);char *fault=strtok_r(nullptr,",",&save);char *editMode=strtok_r(nullptr,",",&save);char *openS=strtok_r(nullptr,",",&save);char *nanoLinkFlag=strtok_r(nullptr,",",&save);char *legacyT=strtok_r(nullptr,",",&save);char *legacyD=strtok_r(nullptr,",",&save);
    bool wasClosed=lidClosed,wasHeater=heaterOn;if(t){float v=atof(t);tempC=(v<0||v>250)?NAN:v;}if(closed)lidClosed=atoi(closed)!=0;if(heat)heaterOn=atoi(heat)!=0;if(fault)sensorFault=atoi(fault)!=0;if(!haveHeaterState){addHeaterEvent(uptimeS(),heaterOn,true);haveHeaterState=true;}else if(wasHeater!=heaterOn){addHeaterEvent(uptimeS(),heaterOn);addActivity(heaterOn?"heater_on":"heater_off");}
    if(wasClosed&&!lidClosed){lidOpenStartMs=millis();openingEvents++;addActivity("lid_open");if(bakeState==2)completeBake();else {bakeState=0;bakeElapsedS=0;overtimeS=0;}}
    if(!wasClosed&&lidClosed&&lidOpenStartMs){uint32_t d=(millis()-lidOpenStartMs)/1000;lidOpenDurationSum+=d;lidOpenDurationN++;addActivity("lid_closed",d);lidOpenStartMs=0;}
    if(legacyT&&legacyD){int lt=atoi(legacyT),ld=atoi(legacyD);if(lt>=WAFFLE_MIN_TEMPERATURE_C&&lt<=WAFFLE_MAX_TEMPERATURE_C&&ld>=WAFFLE_MIN_BAKE_TIME_S&&ld<=WAFFLE_MAX_BAKE_TIME_S){if(importTargetPending||targetC<WAFFLE_MIN_TEMPERATURE_C||targetC>WAFFLE_MAX_TEMPERATURE_C){targetC=lt;prefs.putInt("target",lt);importTargetPending=false;}if(importDurationPending||durationS<WAFFLE_MIN_BAKE_TIME_S||durationS>WAFFLE_MAX_BAKE_TIME_S){durationS=ld;prefs.putInt("duration",ld);importDurationPending=false;}}}
    if(editMode)encoderTimeMode=atoi(editMode)==2;
    nanoHeartbeatReady=nanoLinkFlag&&atoi(nanoLinkFlag)!=0;
    if(openS&&atoi(openS)>0){} // duration is measured locally between stable transitions
  } else if(!strcmp(kind,"EV")){char *ev=strtok_r(nullptr,",",&save);if(ev&&!strcmp(ev,"START")){if(nanoUp&&!sensorFault){runEnabled=true;initialPreheatDone=(!isnan(tempC)&&tempC>=targetC);addActivity("run_started");Serial.println(F("[ESP] Local START accepted"));}else Serial.println(F("[ESP] Local START rejected: Nano offline or sensor fault"));}else if(ev&&!strcmp(ev,"STOP")){runEnabled=false;bakeState=0;initialPreheatDone=false;addActivity("run_stopped");Serial.println(F("[ESP] Local STOP received"));}else if(ev&&!strcmp(ev,"SET")){char *t=strtok_r(nullptr,",",&save);char *d=strtok_r(nullptr,",",&save);int newTarget=t?atoi(t):-1,newDuration=d?atoi(d):-1;if(!importTargetPending&&!importDurationPending&&newTarget>=WAFFLE_MIN_TEMPERATURE_C&&newTarget<=WAFFLE_MAX_TEMPERATURE_C&&newDuration>=WAFFLE_MIN_BAKE_TIME_S&&newDuration<=WAFFLE_MAX_BAKE_TIME_S){if(targetC!=newTarget){addActivity("target_changed",newTarget);if(newTarget>targetC&&tempC<newTarget)initialPreheatDone=false;}if(durationS!=newDuration)addActivity("time_changed",newDuration);targetC=newTarget;durationS=newDuration;settingsDirty=true;settingsChangedMs=millis();Serial.printf("[ESP] Nano settings changed: target=%d C, duration=%d s\n",targetC,durationS);}}else if(ev&&!strcmp(ev,"ENC")){char *deltaText=strtok_r(nullptr,",",&save);int delta=deltaText?atoi(deltaText):0;if(!importTargetPending&&!importDurationPending&&delta){if(encoderTimeMode){durationS=constrain(durationS+delta,WAFFLE_MIN_BAKE_TIME_S,WAFFLE_MAX_BAKE_TIME_S);addActivity("time_changed",durationS);}else{int nextTarget=constrain(targetC+delta,WAFFLE_MIN_TEMPERATURE_C,WAFFLE_MAX_TEMPERATURE_C);if(nextTarget>targetC&&tempC<nextTarget)initialPreheatDone=false;targetC=nextTarget;addActivity("target_changed",targetC);}settingsDirty=true;settingsChangedMs=millis();Serial.printf("[ESP] Encoder changed setting: target=%d C, duration=%d s\n",targetC,durationS);}}}
}
static void pollNano(){while(nanoLink.available()){char c=(char)nanoLink.read();if(c=='\n'||c=='\r'){if(rxLen){rxLine[rxLen]=0;
#if WAFFLE_DEBUG
Serial.printf("[ESP RX] %s\n",rxLine);
#endif
parseNano(rxLine);rxLen=0;}}else if(rxLen<sizeof(rxLine)-1)rxLine[rxLen++]=c;else rxLen=0;}}

static void updateBake(){uint32_t now=millis();if(!nanoUp||sensorFault||!runEnabled){bakeState=0;initialPreheatDone=false;return;}if(!initialPreheatDone){if(!isnan(tempC)&&tempC>=targetC){initialPreheatDone=true;addActivity("temp_reached",(int32_t)lroundf(tempC));}}if(!lidClosed){if(bakeState==1){bakeState=0;}return;}if(bakeState==0){if(!initialPreheatDone)return;bakeState=1;bakeStartedMs=now;bakeElapsedS=0;overtimeS=0;}if(bakeState==1){bakeElapsedS=(now-bakeStartedMs)/1000;if(bakeElapsedS>=durationS){bakeState=2;addActivity("timer_done",durationS);}}if(bakeState==2)overtimeS=(now-bakeStartedMs)/1000-durationS;}

static void sendNanoHello(){char out[32];snprintf(out,sizeof(out),"CMD,%lu,HELLO",(unsigned long)++seq);sendFrame(out);lastHelloMs=millis();nanoSessionStarted=true;haveNanoSeq=false;}
static void sendNanoState(){uint32_t rem=bakeState==1&&bakeElapsedS<durationS?durationS-bakeElapsedS:0;int sT=constrain(targetC,WAFFLE_MIN_TEMPERATURE_C,WAFFLE_MAX_TEMPERATURE_C),sD=constrain(durationS,WAFFLE_MIN_BAKE_TIME_S,WAFFLE_MAX_BAKE_TIME_S);char out[kMaxLineLength];snprintf(out,sizeof(out),"CMD,%lu,STATE,%d,%d,%u,%lu,%lu,%lu,%u",(unsigned long)++seq,sT,sD,runEnabled?1:0,(unsigned long)completed,(unsigned long)rem,(unsigned long)overtimeS,(unsigned)bakeState);sendFrame(out);}
static void sendNanoSettings(){settingsCommandSeq=++seq;int sT=constrain(targetC,WAFFLE_MIN_TEMPERATURE_C,WAFFLE_MAX_TEMPERATURE_C),sD=constrain(durationS,WAFFLE_MIN_BAKE_TIME_S,WAFFLE_MAX_BAKE_TIME_S);char out[48];snprintf(out,sizeof(out),"CMD,%lu,SET,%d,%d",(unsigned long)settingsCommandSeq,sT,sD);sendFrame(out);}

static const char *resetReasonName(){switch(esp_reset_reason()){case ESP_RST_POWERON:return "Power-on";case ESP_RST_EXT:return "External";case ESP_RST_SW:return "Software";case ESP_RST_PANIC:return "Panic";case ESP_RST_INT_WDT:return "Interrupt watchdog";case ESP_RST_TASK_WDT:return "Task watchdog";case ESP_RST_WDT:return "Watchdog";case ESP_RST_DEEPSLEEP:return "Deep sleep";case ESP_RST_BROWNOUT:return "Brownout";case ESP_RST_SDIO:return "SDIO";default:return "Unknown";}}
static String statusJson(){
  String ip=(apMode?WiFi.softAPIP():WiFi.localIP()).toString();
  String ssid=WiFi.status()==WL_CONNECTED?WiFi.SSID():String("");
  String subnet=staticIpEnabled?subnetText:WiFi.subnetMask().toString();
  String gateway=staticIpEnabled?gatewayText:WiFi.gatewayIP().toString();
  String dnsServer=staticIpEnabled?dnsText:WiFi.dnsIP(0).toString();
  struct tm tmNow;bool timeSynced=getLocalTime(&tmNow,0);time_t epoch=timeSynced?mktime(&tmNow):0;
  String j;j.reserve(1500);
  j="{\"temp\":"+(isnan(tempC)?String("null"):String(tempC,1))+",\"target\":"+String(targetC)+",\"duration\":"+String(durationS)+",\"minTemp\":"+String(WAFFLE_MIN_TEMPERATURE_C)+",\"maxTemp\":"+String(WAFFLE_MAX_TEMPERATURE_C)+",\"minTime\":"+String(WAFFLE_MIN_BAKE_TIME_S)+",\"maxTime\":"+String(WAFFLE_MAX_BAKE_TIME_S);
  j+=",\"uiAuth\":"+String(uiAuth?"true":"false")+",\"otaAuth\":"+String(otaAuth?"true":"false")+",\"ntpEnabled\":"+String(ntpEnabled?"true":"false")+",\"ntpServer\":\""+jsonEscape(ntpServerName)+"\",\"timezone\":\""+jsonEscape(timezoneName)+"\",\"timeFormat\":\""+timeFormat+"\",\"timeSynced\":"+String(timeSynced?"true":"false")+",\"epoch\":"+String((uint32_t)epoch);
  j+=",\"staticIpEnabled\":"+String(staticIpEnabled?"true":"false")+",\"staticIp\":\""+jsonEscape(staticIpText.length()?staticIpText:ip)+"\",\"subnet\":\""+jsonEscape(staticIpEnabled?subnetText:subnet)+"\",\"gateway\":\""+jsonEscape(staticIpEnabled?gatewayText:gateway)+"\",\"dns\":\""+jsonEscape(staticIpEnabled?dnsText:dnsServer)+"\",\"currentSubnet\":\""+subnet+"\",\"currentGateway\":\""+gateway+"\",\"currentDns\":\""+dnsServer+"\"";
  j+=",\"encoderMode\":"+String(encoderTimeMode?"\"time\"":"\"temperature\"")+",\"nanoAckSeq\":"+String(lastAckSeq)+",\"nanoAckOk\":"+String(lastAckOk?"true":"false")+",\"heater\":"+String(heaterOn?"true":"false")+",\"lidClosed\":"+String(lidClosed?"true":"false")+",\"nanoOnline\":"+String(nanoUp?"true":"false")+",\"fault\":"+String(sensorFault?"true":"false")+",\"run\":"+String(runEnabled?"true":"false")+",\"count\":"+String(completed)+",\"remaining\":"+String(bakeState==1&&bakeElapsedS<durationS?durationS-bakeElapsedS:0)+",\"overtime\":"+String(overtimeS);
  j+=",\"bakeAvg\":"+(isnan(avgBake())?String("null"):String(avgBake(),1))+",\"openAvg\":"+(isnan(avgOpen())?String("null"):String(avgOpen(),1))+",\"rate\":"+(isnan(ratePerHour())?String("null"):String(ratePerHour(),1))+",\"uptime\":"+String(uptimeS())+",\"wifiConnected\":"+String(WiFi.status()==WL_CONNECTED?"true":"false")+",\"wifiSsid\":\""+jsonEscape(ssid)+"\",\"wifiRssi\":"+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0)+",\"ip\":\""+ip+"\",\"hostname\":\""+jsonEscape(deviceHostname)+"\"";
  float chipT=temperatureRead();
  j+=",\"freeHeap\":"+String(ESP.getFreeHeap())+",\"minFreeHeap\":"+String(ESP.getMinFreeHeap())+",\"maxAllocHeap\":"+String(ESP.getMaxAllocHeap())+",\"cpuMhz\":"+String(ESP.getCpuFreqMHz())+",\"cpuLoad\":"+String(cpuLoad,1)+",\"flashSize\":"+String(ESP.getFlashChipSize())+",\"sketchFree\":"+String(ESP.getFreeSketchSpace())+",\"chip\":\""+String(ESP.getChipModel())+"\",\"chipRevision\":"+String(ESP.getChipRevision())+",\"chipTemp\":"+(isnan(chipT)?String("null"):String(chipT,1))+",\"resetReason\":\""+String(resetReasonName())+"\"}";
  return j;
}
static void sendStatus(){if(!authorized())return;server.send(200,"application/json",statusJson());}
static void sendHistory(){if(!authorized())return;uint32_t from=server.hasArg("from")?(uint32_t)server.arg("from").toInt():0;String j="{\"samples\":[";bool first=true;uint16_t start=(histHead+1080-histCount)%1080;for(uint16_t i=0;i<histCount;i++){Sample &s=history[(start+i)%1080];if(from&&s.sec<=from)continue;if(!first)j+=',';first=false;j+="["+String(s.sec)+","+(s.temp10==-32768?String("null"):String(s.temp10/10.0f,1))+","+String(s.set10/10.0f,1)+","+String(s.heater)+","+String(s.lid)+"]";}j+="]}";server.send(200,"application/json",j);}
static void sendEvents(){if(!authorized())return;String j="{\"activities\":[";uint16_t start=(activityHead+100-activityCount)%100;for(uint16_t i=0;i<activityCount;i++){Activity &a=activities[(start+i)%100];if(i)j+=',';j+="{\"sec\":"+String(a.sec)+",\"epoch\":"+String(a.epoch)+",\"type\":\""+String(a.type)+"\",\"value\":"+String(a.value)+"}";}j+="],\"heater\":[";bool f=true;start=(heaterHead+200-heaterCount)%200;for(uint16_t i=0;i<heaterCount;i++){HeaterEvent &e=heaterEvents[(start+i)%200];if(!f)j+=',';f=false;j+="["+String(e.sec)+","+String(e.on)+","+String(e.baseline)+"]";}j+="]}";server.send(200,"application/json",j);}

static const char FAVICON[] PROGMEM=R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48"><rect width="48" height="48" rx="12" fill="#20a99d"/><g fill="none" stroke="#fff" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="24" cy="24" r="12"/><path d="M24 7v34M9.3 15.5l29.4 17M38.7 15.5l-29.4 17"/></g></svg>)SVG";


static void handleWifi();
static void handleRoot(){String html=FPSTR(PAGE);String tempAttrs="min=\"";tempAttrs+=WAFFLE_MIN_TEMPERATURE_C;tempAttrs+="\" max=\"";tempAttrs+=WAFFLE_MAX_TEMPERATURE_C;tempAttrs+="\" value=\"";tempAttrs+=WAFFLE_DEFAULT_TEMPERATURE_C;tempAttrs+='\"';String timeAttrs="min=\"";timeAttrs+=WAFFLE_MIN_BAKE_TIME_S;timeAttrs+="\" max=\"";timeAttrs+=WAFFLE_MAX_BAKE_TIME_S;timeAttrs+="\" value=\"";timeAttrs+=WAFFLE_DEFAULT_BAKE_TIME_S;timeAttrs+='\"';html.replace("min=\"100\" max=\"180\" value=\"134\"",tempAttrs);html.replace("min=\"10\" max=\"90\" value=\"30\"",timeAttrs);server.send(200,"text/html; charset=utf-8",html);}
static void handleWifi(){String html=R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#101c35"><link rel="icon" type="image/svg+xml" href="/favicon.svg"><title>Wi-Fi setup · Oblátkovač</title><style>
:root{--navy:#101c35;--navy2:#172844;--teal:#27b5a6;--green:#80b918;--ink:#eaf1fb;--muted:#a8b6cb;--line:#2d405e;--card:#17243c;--input:#0e1930}*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(ellipse at 10% 0%,#1b3a57 0,transparent 48%),linear-gradient(145deg,#0c162b,#111f39 65%,#102b3b);color:var(--ink);font:15px system-ui,Segoe UI,sans-serif;display:flex;flex-direction:column}.top{height:68px;display:flex;align-items:center;justify-content:space-between;padding:0 clamp(18px,5vw,70px);border-bottom:1px solid #ffffff18;background:#0b1528aa}.brand{display:flex;gap:11px;align-items:center;font-weight:800;letter-spacing:.2px}.mark{width:36px;height:36px;border-radius:12px;background:linear-gradient(135deg,var(--teal),#16847e);display:grid;place-items:center;box-shadow:0 5px 18px #27b5a644}.mark svg{width:20px;height:20px}.pill{font-size:12px;color:#c5d4e8;padding:7px 10px;border:1px solid var(--line);border-radius:99px}.layout{width:min(1040px,92vw);margin:auto;display:grid;grid-template-columns:1fr 1fr;gap:34px;align-items:center;padding:42px 0}.intro h1{font-size:clamp(30px,5vw,48px);line-height:1.08;margin:16px 0}.intro p{color:var(--muted);line-height:1.65;max-width:480px}.eyebrow{color:#53d3c4;text-transform:uppercase;letter-spacing:1.5px;font-size:12px;font-weight:750}.art{height:150px;max-width:360px;display:flex;align-items:center;gap:16px;margin:20px 0}.device{width:110px;height:132px;border:1px solid #52718d;border-radius:18px;background:linear-gradient(150deg,#213956,#10203b);display:grid;place-items:center;box-shadow:0 18px 40px #0004}.device svg{width:54px;height:54px;color:#5bdfcf}.wave{display:flex;align-items:center;gap:8px}.wave i{display:block;width:8px;border-radius:8px;background:var(--teal);opacity:.8}.wave i:nth-child(1),.wave i:nth-child(5){height:22px;opacity:.35}.wave i:nth-child(2),.wave i:nth-child(4){height:42px;opacity:.6}.wave i:nth-child(3){height:65px}.card{background:linear-gradient(160deg,#1b2b46,#14223a);border:1px solid #415774;border-radius:18px;padding:25px;box-shadow:0 24px 70px #0005}.card h2{margin:0;font-size:21px}.sub{color:var(--muted);font-size:13px;margin:7px 0 20px}.network{display:flex;align-items:center;gap:12px;background:#0e1b31;border:1px solid var(--line);border-radius:12px;padding:12px 14px;margin-bottom:20px}.wifiIcon{width:40px;height:40px;border-radius:12px;background:#1e4355;color:#66e0d2;display:grid;place-items:center}.wifiIcon svg{width:23px;height:23px}.netText small{display:block;color:var(--muted);font-size:11px;margin-bottom:3px}.netText strong{font-size:14px}.field{margin:15px 0}.field label{display:block;color:#d8e2f0;font-size:13px;font-weight:650;margin-bottom:7px}.field input{width:100%;padding:12px 13px;border:1px solid #3c506d;border-radius:9px;background:var(--input);color:var(--ink);font-size:15px;outline:none}.field input:focus{border-color:var(--teal);box-shadow:0 0 0 3px #27b5a622}.pass{position:relative}.pass input{padding-right:48px}.show{position:absolute;right:7px;top:6px;border:0;background:transparent;color:var(--muted);padding:7px;cursor:pointer}.submit{width:100%;border:0;border-radius:10px;padding:13px;background:linear-gradient(90deg,#18a898,#168e90);color:#fff;font-size:15px;font-weight:750;cursor:pointer;box-shadow:0 8px 22px #18a89833}.submit:hover{filter:brightness(1.1)}.hint{font-size:12px;color:var(--muted);line-height:1.5;margin:15px 0 0}.footer{text-align:center;color:#8ea0b8;font-size:12px;padding:0 16px 24px}.steps{display:flex;gap:10px;margin-top:24px;color:#b5c6d9;font-size:12px}.step{padding:9px 11px;border:1px solid #ffffff19;border-radius:9px;background:#ffffff08}.step b{color:#62d7c8;margin-right:5px}@media(max-width:760px){.layout{grid-template-columns:1fr;gap:10px;padding:30px 0}.intro p{max-width:none}.art{height:90px;margin:12px 0}.device{height:80px;width:76px}.device svg{width:40px}.card{padding:21px}.top{height:60px}}
</style></head><body><header class="top"><div class="brand"><span class="mark"><svg viewBox="0 0 24 24" fill="none" stroke="white" stroke-width="2.3" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v18M5 7l14 10M19 7 5 17"/><circle cx="12" cy="12" r="9"/></svg></span><span>OBLÁTKOVAČ</span></div><span class="pill" data-i18n="setupTitle">Device setup</span><label style="color:#c5d4e8;font-size:12px"><span data-i18n="language">Language</span> <select id="portalLanguage" onchange="changePortalLanguage()"><option value="en">English</option><option value="sk">Slovenčina</option></select></label></header><main class="layout"><section class="intro"><div class="eyebrow" data-i18n="firstConnection">First connection</div><h1 data-i18n="connectWifi">Connect to your Wi-Fi.</h1><p data-i18n="wifiIntro">Enter your home network name and password. After saving, the ESP32 will restart and the dashboard will be available on your local network.</p><div class="art"><div class="device"><svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2"><rect x="11" y="5" width="26" height="38" rx="5"/><path d="M19 12h10M19 36h10M17 23h14M20 19l-3 4 3 4M28 19l3 4-3 4"/></svg></div><div class="wave"><i></i><i></i><i></i><i></i><i></i></div><div style="font-size:28px">⌂</div></div><div class="steps"><span class="step"><b>1</b> <span data-i18n="stepChoose">Choose a network</span></span><span class="step"><b>2</b> <span data-i18n="stepPassword">Enter password</span></span><span class="step"><b>3</b> <span data-i18n="stepSave">Save</span></span></div></section><section class="card"><h2 data-i18n="wifiSettings">Wi-Fi connection</h2><p class="sub" data-i18n="savedLocally">Your credentials are stored on the ESP32.</p><div class="network"><div class="wifiIcon"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M2 8.8a15 15 0 0 1 20 0M5 12a10.5 10.5 0 0 1 14 0m-11 3.4a5.5 5.5 0 0 1 8 0M12 19h.01"/></svg></div><div class="netText"><small data-i18n="currentAp">Current access point</small><strong>@@SSID@@</strong></div></div><div style="display:flex;gap:10px;align-items:center;margin:0 0 10px"><button id="scanButton" type="button" onclick="scanWifi()" style="border:1px solid #3c6178;border-radius:9px;background:#17384b;color:#78e5d8;padding:10px 12px;font-weight:700;cursor:pointer" data-i18n="scan">Scan for Wi-Fi networks</button><span id="scanState" style="font-size:12px;color:#a8b6cb"></span></div><div id="wifiNetworks" style="display:grid;gap:7px;margin-bottom:14px"></div><form method="post" action="/savewifi"><div class="field"><label for="ssid" data-i18n="ssid">Wi-Fi network name (SSID)</label><input id="ssid" name="ssid" autocomplete="off" required placeholder="e.g. HomeWiFi" data-i18n-placeholder="ssidPlaceholder"></div><div class="field"><label for="pass" data-i18n="password">Wi-Fi password</label><div class="pass"><input id="pass" name="pass" type="password" autocomplete="new-password" placeholder="Enter password" data-i18n-placeholder="passwordPlaceholder"><button class="show" type="button" onclick="let p=document.getElementById('pass');p.type=p.type==='password'?'text':'password';this.textContent=portalTr(p.type==='password'?'show':'hide')" data-i18n="show">Show</button></div></div><button class="submit" type="submit" data-i18n="saveConnect">Save and connect&nbsp; →</button></form><p class="hint" data-i18n="reconnectHint">After restart, reconnect to your Wi-Fi. If the connection fails, the device will start this setup access point again.</p></section></main><footer class="footer" data-i18n="wifiSetupFooter">Oblátkovač · ESP32-C3 setup</footer><script>let portalLang="en",portalWords={};function portalTr(k){return portalWords[k]||k}function applyPortalLanguage(){document.documentElement.lang=portalLang;document.querySelectorAll("[data-i18n]").forEach(e=>{if(portalWords[e.dataset.i18n])e.textContent=portalWords[e.dataset.i18n]});document.querySelectorAll("[data-i18n-placeholder]").forEach(e=>{if(portalWords[e.dataset.i18nPlaceholder])e.placeholder=portalWords[e.dataset.i18nPlaceholder]});portalLanguage.value=portalLang}async function changePortalLanguage(){try{portalLang=portalLanguage.value==="sk"?"sk":"en";let p=await fetch("/api/language",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:new URLSearchParams({language:portalLang})});if(!p.ok)throw Error();let t=await fetch("/api/translations?lang="+portalLang,{cache:"no-store"});portalWords=await t.json();applyPortalLanguage()}catch(e){}}async function initPortalLanguage(){try{let p=await fetch("/api/language",{cache:"no-store"}),d=await p.json();portalLang=d.language==="sk"?"sk":"en";let t=await fetch("/api/translations?lang="+portalLang,{cache:"no-store"});portalWords=await t.json();applyPortalLanguage()}catch(e){}}initPortalLanguage();async function scanWifi(){const b=document.getElementById("scanButton"),s=document.getElementById("scanState"),list=document.getElementById("wifiNetworks");b.disabled=true;b.textContent=portalTr("scanning");s.textContent=portalTr("scanning");list.replaceChildren();try{let r=await fetch("/api/wifi-scan?start=1",{cache:"no-store"}),d=await r.json(),tries=0;while(d.state==="scanning"&&tries++<30){await new Promise(ok=>setTimeout(ok,500));r=await fetch("/api/wifi-scan",{cache:"no-store"});d=await r.json()}if(d.state!=="done"){s.textContent=portalTr("scanFailed");return}if(!d.networks.length){s.textContent=portalTr("noNetworks");return}s.textContent=portalTr("networksFound")+" "+d.networks.length;for(const n of d.networks){const item=document.createElement("button");item.type="button";item.style.cssText="text-align:left;border:1px solid #344a67;border-radius:9px;background:#0e1b31;color:#eaf1fb;padding:10px 12px;cursor:pointer";item.textContent=(n.secure?"🔒 ":"")+"📶 "+n.ssid+" · "+n.rssi+" dBm";item.onclick=()=>{document.getElementById("ssid").value=n.ssid;document.getElementById("pass").focus()};list.appendChild(item)}}catch(e){s.textContent=portalTr("scanError")}finally{b.disabled=false;b.textContent=portalTr("scan")}}</script></body></html>)HTML";html.replace("@@SSID@@",setupApSsid.length()?setupApSsid:String("Oblatkovac-Setup"));server.send(200,"text/html; charset=utf-8",html);}
static void handleSaveWifi(){if(!apMode){server.send(403,"text/plain","Wi-Fi settings are available only in setup mode");return;}prefs.putString("ssid",server.arg("ssid"));prefs.putString("wifiPass",server.arg("pass"));Serial.printf("[ESP] Saved Wi-Fi network '%s'; rebooting\n",server.arg("ssid").c_str());server.send(200,"text/plain","Saved. Restarting…");delay(500);ESP.restart();}
static void handleLanguage(){
  if(server.method()==HTTP_POST){String requested=server.arg("language");if(requested!="sk"&&requested!="en"){server.send(400,"application/json","{\"error\":\"Unsupported language\"}");return;}activeLanguage=requested;prefs.putString("language",activeLanguage);Serial.printf("[ESP] UI language set to %s\n",activeLanguage.c_str());}
  server.send(200,"application/json",String("{\"language\":\"")+activeLanguage+"\"}");
}
static void handleTranslations(){String requested=server.arg("lang");if(requested=="sk")server.send_P(200,"application/json; charset=utf-8",UI_LANGUAGE_SK);else server.send_P(200,"application/json; charset=utf-8",UI_LANGUAGE_EN);}
static String jsonEscape(const String &value){String out;out.reserve(value.length()+8);for(size_t i=0;i<value.length();i++){char c=value[i];if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if((uint8_t)c>=0x20)out+=c;}return out;}
static bool wifiScanRunning=false;
static void handleWifiScan(){
  if(!apMode){server.send(403,"application/json","{\"state\":\"unavailable\",\"networks\":[]}");return;}
  if(server.hasArg("start")&&!wifiScanRunning){
    Serial.println(F("[ESP] Scanning nearby Wi-Fi networks"));
    int result=WiFi.scanNetworks(true);
    wifiScanRunning=(result==WIFI_SCAN_RUNNING);
    if(result>=0)wifiScanRunning=false;
  }
  int count=WiFi.scanComplete();
  if(count==WIFI_SCAN_RUNNING){server.send(200,"application/json","{\"state\":\"scanning\",\"networks\":[]}");return;}
  if(count<0){
    Serial.printf("[ESP] Wi-Fi scan failed (%d)\n",count);
    wifiScanRunning=false;WiFi.scanDelete();server.send(200,"application/json","{\"state\":\"failed\",\"networks\":[]}");return;
  }
  String json="{\"state\":\"done\",\"networks\":[";bool first=true;
  for(int i=0;i<count;i++){
    String ssid=WiFi.SSID(i);if(!ssid.length())continue;if(!first)json+=',';first=false;
    json+="{\"ssid\":\""+jsonEscape(ssid)+"\",\"rssi\":"+String(WiFi.RSSI(i))+",\"secure\":"+String(WiFi.encryptionType(i)!=WIFI_AUTH_OPEN?"true":"false")+"}";
  }
  json+="]}";Serial.printf("[ESP] Wi-Fi scan complete: %d networks\n",count);
  WiFi.scanDelete();wifiScanRunning=false;server.send(200,"application/json",json);
}
static void handleSet(){if(!authorized())return;int t=server.arg("target").toInt(),d=server.arg("duration").toInt();if(t<WAFFLE_MIN_TEMPERATURE_C||t>WAFFLE_MAX_TEMPERATURE_C||d<WAFFLE_MIN_BAKE_TIME_S||d>WAFFLE_MAX_BAKE_TIME_S){String error="Temperature "+String(WAFFLE_MIN_TEMPERATURE_C)+"-"+String(WAFFLE_MAX_TEMPERATURE_C)+" C, time "+String(WAFFLE_MIN_BAKE_TIME_S)+"-"+String(WAFFLE_MAX_BAKE_TIME_S)+" s";server.send(400,"text/plain",error);return;}if(targetC!=t)addActivity("target_changed",t);if(durationS!=d)addActivity("time_changed",d);targetC=t;durationS=d;prefs.putInt("target",t);prefs.putInt("duration",d);settingsDirty=false;settingsSyncPending=true;Serial.printf("[ESP] Settings saved: target=%d C, duration=%d s\n",targetC,durationS);lastControlMs=0;server.send(200,"application/json","{\"ok\":true}");}
static void handleControl(){if(!authorized())return;String c=server.arg("cmd");if(c=="STOP"){runEnabled=false;bakeState=0;initialPreheatDone=false;addActivity("run_stopped");Serial.println(F("[ESP] Web STOP accepted"));if(nanoUp)lastControlMs=0;server.send(200,"application/json","{\"ok\":true}");}else if(c=="RUN"){if(!nanoUp||sensorFault){Serial.println(F("[ESP] Web START rejected: Nano offline or sensor fault"));server.send(409,"application/json","{\"ok\":false,\"error\":\"Nano offline or sensor fault\"}");return;}runEnabled=true;initialPreheatDone=(!isnan(tempC)&&tempC>=targetC);addActivity("run_started");Serial.println(F("[ESP] Web START accepted"));if(nanoUp)lastControlMs=0;server.send(200,"application/json","{\"ok\":true}");}else server.send(400,"text/plain","Unknown command");}
static String timezoneRule(const String &zone){if(zone=="Europe/Bratislava"||zone=="Europe/Prague"||zone=="Europe/Berlin")return "CET-1CEST,M3.5.0,M10.5.0/3";if(zone=="Europe/London")return "GMT0BST,M3.5.0/1,M10.5.0";if(zone=="America/New_York")return "EST5EDT,M3.2.0,M11.1.0";if(zone=="America/Los_Angeles")return "PST8PDT,M3.2.0,M11.1.0";if(zone=="Asia/Tokyo")return "JST-9";return "UTC0";}
static void configureTime(){String rule=timezoneRule(timezoneName);setenv("TZ",rule.c_str(),1);tzset();timeConfigStarted=false;if(ntpEnabled&&WiFi.status()==WL_CONNECTED){configTzTime(rule.c_str(),ntpServerName.c_str());timeConfigStarted=true;}}
static bool validHostname(String host){host.trim();if(!host.length()||host.length()>32||host[0]=='-'||host[host.length()-1]=='-')return false;for(size_t i=0;i<host.length();i++){char c=host[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return false;}return true;}
static String defaultHostname(){char host[24];snprintf(host,sizeof(host),"oblatkovac-%06x",(uint32_t)(ESP.getEfuseMac()&0xFFFFFF));return String(host);}
static void handleNetworkSettings(){if(!authorized())return;String host=server.arg("hostname");host.trim();bool useStatic=server.arg("staticIpEnabled")=="1";IPAddress ip,mask,gateway,dnsServer;if(!validHostname(host)){server.send(400,"application/json","{\"error\":\"invalid_hostname\"}");return;}if(useStatic&&(!ip.fromString(server.arg("staticIp"))||!mask.fromString(server.arg("subnet"))||!gateway.fromString(server.arg("gateway"))||!dnsServer.fromString(server.arg("dns")))){server.send(400,"application/json","{\"error\":\"invalid_ip_configuration\"}");return;}deviceHostname=host;staticIpEnabled=useStatic;staticIpText=server.arg("staticIp");subnetText=server.arg("subnet");gatewayText=server.arg("gateway");dnsText=server.arg("dns");prefs.putString("hostname",deviceHostname);prefs.putBool("staticIpEnabled",staticIpEnabled);prefs.putString("staticIp",staticIpText);prefs.putString("subnet",subnetText);prefs.putString("gateway",gatewayText);prefs.putString("dns",dnsText);addActivity("network_changed");server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void handleTimeSettings(){if(!authorized())return;String ntp=server.arg("ntpServer"),zone=server.arg("timezone"),format=server.arg("timeFormat");ntp.trim();if(!ntp.length()||ntp.length()>128||(format!="12"&&format!="24")||(zone!="Europe/Bratislava"&&zone!="Europe/Prague"&&zone!="Europe/Berlin"&&zone!="Europe/London"&&zone!="America/New_York"&&zone!="America/Los_Angeles"&&zone!="Asia/Tokyo"&&zone!="UTC")){server.send(400,"application/json","{\"error\":\"invalid_time_settings\"}");return;}ntpEnabled=server.arg("ntpEnabled")=="1";ntpServerName=ntp;timezoneName=zone;timeFormat=format;prefs.putBool("ntpEnabled",ntpEnabled);prefs.putString("ntpServer",ntpServerName);prefs.putString("timezone",timezoneName);prefs.putString("timeFormat",timeFormat);configureTime();addActivity("time_configured");server.send(200,"application/json","{\"ok\":true}");}
static void handleAuth(){if(!authorized())return;String p=server.arg("password");bool nextUi=server.arg("uiEnabled")=="1",nextOta=server.arg("otaEnabled")=="1";if((nextUi||nextOta)&&!updatePasswordSet&&!p.length()){server.send(400,"application/json","{\"error\":\"set_password_first\"}");return;}if(p.length()&&p.length()<8){server.send(400,"application/json","{\"error\":\"password_too_short\"}");return;}if(p.length()){uiPassword=p;prefs.putString("uiPass",p);updatePasswordSet=true;}uiAuth=nextUi;otaAuth=nextOta;prefs.putBool("uiAuth",uiAuth);prefs.putBool("otaAuth",otaAuth);addActivity("security_changed");server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void handleMetrics(){String m="# HELP esp32_uptime_seconds Uptime since boot\n# TYPE esp32_uptime_seconds counter\nesp32_uptime_seconds "+String(uptimeS())+"\n";
  m+="# HELP esp32_heap_free_bytes Free heap bytes\n# TYPE esp32_heap_free_bytes gauge\nesp32_heap_free_bytes "+String(ESP.getFreeHeap())+"\n";
  m+="# HELP esp32_heap_total_bytes Total heap bytes\n# TYPE esp32_heap_total_bytes gauge\nesp32_heap_total_bytes "+String(ESP.getHeapSize())+"\n";
  m+="# HELP esp32_heap_min_free_bytes Minimum free heap since boot\n# TYPE esp32_heap_min_free_bytes gauge\nesp32_heap_min_free_bytes "+String(ESP.getMinFreeHeap())+"\n";
  m+="# HELP esp32_heap_max_alloc_bytes Largest heap block\n# TYPE esp32_heap_max_alloc_bytes gauge\nesp32_heap_max_alloc_bytes "+String(ESP.getMaxAllocHeap())+"\n";
  m+="# HELP esp32_cpu_frequency_mhz CPU frequency\n# TYPE esp32_cpu_frequency_mhz gauge\nesp32_cpu_frequency_mhz "+String(ESP.getCpuFreqMHz())+"\n";
  m+="# HELP esp32_reset_reason Reset reason enum value\n# TYPE esp32_reset_reason gauge\nesp32_reset_reason "+String((uint8_t)esp_reset_reason())+"\n";
  m+="# HELP esp32_flash_size_bytes Flash size\n# TYPE esp32_flash_size_bytes gauge\nesp32_flash_size_bytes "+String(ESP.getFlashChipSize())+"\n";
  m+="# HELP esp32_sketch_size_bytes Firmware image size\n# TYPE esp32_sketch_size_bytes gauge\nesp32_sketch_size_bytes "+String(ESP.getSketchSize())+"\n";
  m+="# HELP esp32_sketch_free_bytes OTA partition free size\n# TYPE esp32_sketch_free_bytes gauge\nesp32_sketch_free_bytes "+String(ESP.getFreeSketchSpace())+"\n";
  m+="# HELP esp32_wifi_connected WiFi station connected\n# TYPE esp32_wifi_connected gauge\nesp32_wifi_connected "+String(WiFi.status()==WL_CONNECTED?1:0)+"\n";
  m+="# HELP esp32_wifi_rssi_dbm WiFi RSSI\n# TYPE esp32_wifi_rssi_dbm gauge\nesp32_wifi_rssi_dbm "+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0)+"\n";
  m+="# HELP oblatkovac_temperature_celsius Actual temperature\n# TYPE oblatkovac_temperature_celsius gauge\noblatkovac_temperature_celsius "+String(isnan(tempC)?0:tempC,1)+"\n";
  m+="# HELP oblatkovac_target_temperature_celsius Set temperature\n# TYPE oblatkovac_target_temperature_celsius gauge\noblatkovac_target_temperature_celsius "+String(targetC)+"\n";
  m+="# HELP oblatkovac_heater_on Heater state\n# TYPE oblatkovac_heater_on gauge\noblatkovac_heater_on "+String(heaterOn?1:0)+"\n";
  m+="# HELP oblatkovac_lid_open Lid open state\n# TYPE oblatkovac_lid_open gauge\noblatkovac_lid_open "+String(lidClosed?0:1)+"\n";
  m+="# HELP oblatkovac_nano_link_up Nano UART link state\n# TYPE oblatkovac_nano_link_up gauge\noblatkovac_nano_link_up "+String(nanoUp?1:0)+"\n";
  m+="# HELP oblatkovac_completed_bakes_total Completed bakes since boot\n# TYPE oblatkovac_completed_bakes_total counter\noblatkovac_completed_bakes_total "+String(completed)+"\n";
  m+="# HELP oblatkovac_open_events_total Lid opening events since boot\n# TYPE oblatkovac_open_events_total counter\noblatkovac_open_events_total "+String(openingEvents)+"\n";
  m+="# HELP oblatkovac_average_bake_duration_seconds Average completed bake duration incl overtime\n# TYPE oblatkovac_average_bake_duration_seconds gauge\noblatkovac_average_bake_duration_seconds "+String(isnan(avgBake())?0:avgBake(),1)+"\n";
  m+="# HELP oblatkovac_average_lid_open_seconds Average lid-open interval\n# TYPE oblatkovac_average_lid_open_seconds gauge\noblatkovac_average_lid_open_seconds "+String(isnan(avgOpen())?0:avgOpen(),1)+"\n";
  m+="# HELP oblatkovac_wafers_per_hour Estimated production rate\n# TYPE oblatkovac_wafers_per_hour gauge\noblatkovac_wafers_per_hour "+String(isnan(ratePerHour())?0:ratePerHour(),1)+"\n";
  server.send(200,"text/plain; version=0.0.4; charset=utf-8",m);}

static void handleUpdateUpload(){HTTPUpload &u=server.upload();if(!otaAuthorized())return;if(u.status==UPLOAD_FILE_START){if(!Update.begin(UPDATE_SIZE_UNKNOWN))Update.printError(Serial);}else if(u.status==UPLOAD_FILE_WRITE){if(Update.write(u.buf,u.currentSize)!=u.currentSize)Update.printError(Serial);}else if(u.status==UPLOAD_FILE_END){if(Update.end(true))Serial.println("OTA web update complete");else Update.printError(Serial);}}
static void startPortal(){stationConnecting=false;stationWasConnected=false;apMode=true;WiFi.mode(WIFI_AP_STA);String mac=WiFi.softAPmacAddress();mac.replace(":","");setupApSsid="Oblatkovac-Setup-"+mac.substring(mac.length()-4);WiFi.softAP(setupApSsid.c_str());dns.start(53,"*",WiFi.softAPIP());Serial.printf("[ESP] Setup AP '%s', address %s\n",setupApSsid.c_str(),WiFi.softAPIP().toString().c_str());}
static void handleReconfigureWifi(){if(!authorized())return;startPortal();String j="{\"ssid\":\""+jsonEscape(setupApSsid)+"\",\"ip\":\""+WiFi.softAPIP().toString()+"\"}";server.send(200,"application/json",j);}
static void handleTimeSync(){if(!authorized())return;if(!ntpEnabled){server.send(409,"application/json","{\"error\":\"ntp_disabled\"}");return;}configureTime();addActivity("time_synced");server.send(200,"application/json","{\"ok\":true}");}
static void handleRestart(){if(!authorized())return;addActivity("device_restart");server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void handleForgetWifi(){if(!authorized())return;prefs.remove("ssid");prefs.remove("wifiPass");server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void handleFactoryReset(){if(!authorized())return;prefs.clear();server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void startNetwork(){String s=prefs.getString("ssid","");String p=prefs.getString("wifiPass","");WiFi.setHostname(deviceHostname.c_str());if(!s.length()){Serial.println(F("[ESP] No saved Wi-Fi; starting setup AP"));startPortal();return;}Serial.printf("[ESP] Connecting to Wi-Fi '%s'\n",s.c_str());apMode=false;stationConnecting=true;stationStartedMs=millis();WiFi.mode(WIFI_STA);if(staticIpEnabled){IPAddress ip,mask,gateway,dnsServer;if(ip.fromString(staticIpText)&&mask.fromString(subnetText)&&gateway.fromString(gatewayText)&&dnsServer.fromString(dnsText))WiFi.config(ip,gateway,mask,dnsServer);}WiFi.begin(s.c_str(),p.c_str());}
static void startArduinoOTAIfReady(){if((!otaAuth||updatePasswordSet)&&!apMode&&!arduinoOtaStarted&&WiFi.status()==WL_CONNECTED){if(otaAuth)ArduinoOTA.setPassword(uiPassword.c_str());ArduinoOTA.setHostname(deviceHostname.c_str());ArduinoOTA.begin();arduinoOtaStarted=true;Serial.println(F("[ESP] ArduinoOTA ready"));}}

void setup(){
  bootMs=millis();Serial.begin(115200);pinMode(8,OUTPUT);setWifiLed(false);lastWifiLedMs=millis();Serial.println(F("[ESP] Oblatkovac controller boot; USB log at 115200 baud"));Serial.printf("[ESP] Firmware v%s %s %s\\n",FIRMWARE_VERSION,FIRMWARE_BUILD_DATE,FIRMWARE_BUILD_TIME);
  esp_register_freertos_idle_hook(idleHook);
  nanoLink.begin(kBaudRate,SERIAL_8N1,kEspRxPin,kEspTxPin);prefs.begin("waffle",false);activeLanguage=prefs.getString("language","en");if(activeLanguage!="sk")activeLanguage="en";deviceHostname=prefs.getString("hostname","");if(!deviceHostname.length()||deviceHostname=="oblatkovac")deviceHostname=defaultHostname();staticIpEnabled=prefs.getBool("staticIpEnabled",false);staticIpText=prefs.getString("staticIp","");subnetText=prefs.getString("subnet","255.255.255.0");gatewayText=prefs.getString("gateway","");dnsText=prefs.getString("dns","");ntpEnabled=prefs.getBool("ntpEnabled",true);ntpServerName=prefs.getString("ntpServer","pool.ntp.org");timezoneName=prefs.getString("timezone","Europe/Bratislava");timeFormat=prefs.getString("timeFormat","24");if(timeFormat!="12")timeFormat="24";configureTime();importTargetPending=!prefs.isKey("target");importDurationPending=!prefs.isKey("duration");targetC=prefs.getInt("target",WAFFLE_DEFAULT_TEMPERATURE_C);if(targetC<WAFFLE_MIN_TEMPERATURE_C||targetC>WAFFLE_MAX_TEMPERATURE_C)targetC=WAFFLE_DEFAULT_TEMPERATURE_C;durationS=prefs.getInt("duration",WAFFLE_DEFAULT_BAKE_TIME_S);if(durationS<WAFFLE_MIN_BAKE_TIME_S||durationS>WAFFLE_MAX_BAKE_TIME_S)durationS=WAFFLE_DEFAULT_BAKE_TIME_S;uiAuth=prefs.getBool("uiAuth",false);otaAuth=prefs.getBool("otaAuth",true);uiPassword=prefs.getString("uiPass","");updatePasswordSet=uiPassword.length()>0;startNetwork();configWaitStartedMs=millis();
  server.on("/",HTTP_GET,handleRoot);server.on("/settings",HTTP_GET,handleRoot);server.on("/wifi",HTTP_GET,handleWifi);server.on("/savewifi",HTTP_POST,handleSaveWifi);
  server.on("/favicon.ico",HTTP_ANY,[]{server.sendHeader("Cache-Control","no-store");server.send_P(200,"image/svg+xml",FAVICON);});server.on("/favicon.svg",HTTP_GET,[]{server.sendHeader("Cache-Control","no-store");server.send_P(200,"image/svg+xml",FAVICON);});server.on("/api/wifi-scan",HTTP_GET,handleWifiScan);
  server.on("/api/version",HTTP_GET,[]{String j="{\"version\":\"" FIRMWARE_VERSION "\",\"buildDate\":\"" FIRMWARE_BUILD_DATE "\",\"buildTime\":\"" FIRMWARE_BUILD_TIME "\"}";server.send(200,"application/json",j);});server.on("/api/status",HTTP_GET,sendStatus);server.on("/api/history",HTTP_GET,sendHistory);server.on("/api/events",HTTP_GET,sendEvents);server.on("/api/language",HTTP_ANY,handleLanguage);server.on("/api/translations",HTTP_GET,handleTranslations);
  server.on("/api/settings",HTTP_POST,handleSet);server.on("/api/network-settings",HTTP_POST,handleNetworkSettings);server.on("/api/time-settings",HTTP_POST,handleTimeSettings);server.on("/api/time-sync",HTTP_POST,handleTimeSync);server.on("/api/control",HTTP_GET,[]{server.send(405,"application/json","{\"error\":\"method_not_allowed\",\"expected\":\"POST\"}");});server.on("/api/control",HTTP_POST,handleControl);server.on("/api/auth",HTTP_POST,handleAuth);server.on("/api/reconfigure-wifi",HTTP_POST,handleReconfigureWifi);server.on("/api/restart",HTTP_POST,handleRestart);server.on("/api/wifi/forget",HTTP_POST,handleForgetWifi);server.on("/api/factory-reset",HTTP_POST,handleFactoryReset);server.on("/metrics",HTTP_GET,handleMetrics);
  server.on("/update",HTTP_GET,[]{if(!otaAuthorized()){server.send(403,"text/plain","OTA requires configured password");return;}server.send(200,"text/html","<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='firmware'><button>Upload firmware</button></form>");});
  server.on("/update",HTTP_POST,[]{if(!otaAuthorized())return;server.send(200,"text/plain","Update complete; rebooting");delay(300);ESP.restart();},handleUpdateUpload);
  server.onNotFound([](){if(server.uri().startsWith("/api/")){server.send(404,"application/json","{\"error\":\"not_found\"}");return;}handleWifi();});
  server.begin();lastSampleMs=millis();lastControlMs=millis();Serial.println(F("[ESP] Web server ready; waiting for Nano UART and Wi-Fi"));}

void loop(){
  uint32_t now=millis();
  static uint32_t lastCpuCheckMs=0;if(now-lastCpuCheckMs>=1000){uint32_t ticks=idleTicks;idleTicks=0;if(ticks>maxIdleTicks)maxIdleTicks=ticks;if(maxIdleTicks>0){float idlePercent=(float)ticks*100.0f/(float)maxIdleTicks;cpuLoad=100.0f-idlePercent;if(cpuLoad<0.0f)cpuLoad=0.0f;if(cpuLoad>100.0f)cpuLoad=100.0f;}lastCpuCheckMs=now;}
  if(stationConnecting){if(WiFi.status()==WL_CONNECTED){stationConnecting=false;stationWasConnected=true;configureTime();Serial.printf("[ESP] Wi-Fi connected, IP %s, RSSI %d dBm\n",WiFi.localIP().toString().c_str(),WiFi.RSSI());}else if(now-stationStartedMs>=15000){Serial.println(F("[ESP] Wi-Fi connection timed out; starting setup AP"));startPortal();}}
  else if(!apMode&&WiFi.status()!=WL_CONNECTED){if(stationWasConnected)Serial.println(F("[ESP] Wi-Fi lost; retrying for 15 seconds"));stationWasConnected=false;stationConnecting=true;stationStartedMs=now;WiFi.reconnect();}
  if(apMode)dns.processNextRequest();pollNano();server.handleClient();pollNano();startArduinoOTAIfReady();if(arduinoOtaStarted)ArduinoOTA.handle();pollNano();
  now=millis();
  nanoUp=(lastNanoFrameMs>0)&&(now-lastNanoFrameMs<2000);
  if(!nanoLinkLogInitialized||nanoUp!=lastLoggedNanoUp){Serial.println(nanoUp?F("[ESP] Nano UART online"):F("[ESP] Nano UART offline; run disabled"));lastLoggedNanoUp=nanoUp;nanoLinkLogInitialized=true;}
  if(!nanoUp&&previousNanoUp){runEnabled=false;bakeState=0;bakeElapsedS=0;overtimeS=0;nanoSessionStarted=false;haveNanoSeq=false;}previousNanoUp=nanoUp;
  if(!apMode&&WiFi.status()==WL_CONNECTED){if(!wifiLedState){wifiLedState=true;setWifiLed(true);}}else{uint32_t blinkMs=apMode?500:250;if(now-lastWifiLedMs>=blinkMs){lastWifiLedMs=now;wifiLedState=!wifiLedState;setWifiLed(wifiLedState);}}
  if((importTargetPending||importDurationPending)&&now-configWaitStartedMs>=1500){if(importTargetPending)prefs.putInt("target",targetC);if(importDurationPending)prefs.putInt("duration",durationS);importTargetPending=false;importDurationPending=false;}
  if(settingsDirty&&now-settingsChangedMs>=2000){prefs.putInt("target",targetC);prefs.putInt("duration",durationS);settingsDirty=false;}
  updateBake();if(now-lastSampleMs>=10000){lastSampleMs=now;addSample();}
  if(now-lastControlMs>=500&&(!importTargetPending&&!importDurationPending)){
    lastControlMs=now;
    if(!nanoUp){
      heaterOn=false;
      if(!nanoSessionStarted||now-lastHelloMs>=500)sendNanoHello();
    }else{
      nanoSessionStarted=true;
      if(settingsSyncPending)sendNanoSettings();else sendNanoState();
    }
  }
}
