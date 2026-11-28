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
#include "login_ui.h"

using namespace bridge;
static volatile uint32_t idleTicks = 0;
static uint32_t maxIdleTicks = 0;
static float cpuLoad = 0.0f;
static bool idleHook(void) { idleTicks++; return true; }
HardwareSerial nanoLink(1);
WebServer server(80);
DNSServer dns;
Preferences prefs;

struct Sample { uint32_t sec; int16_t temp10; int16_t set10; uint8_t heater; uint8_t lid; uint8_t run; };
struct Activity { uint32_t sec; uint32_t epoch; int32_t value; char type[20]; };
struct HeaterEvent { uint32_t sec; uint8_t state; uint8_t baseline; };
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
static bool authEnabled=false, ntpEnabled=true, staticIpEnabled=false;
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
// One switch protects the web UI, the API and OTA. The user name is always "admin".
// Browsers use a session cookie; scripts may send an HTTP Basic header instead.
struct Session{char token[33];uint32_t createdMs;};
static Session sessions[4];
static const uint32_t kSessionTtlMs=12UL*60UL*60UL*1000UL;
static uint8_t loginFailures=0;
static uint32_t lastLoginFailureMs=0;
static bool constantTimeEquals(const String &a,const String &b){const size_t n=a.length(),m=b.length();uint8_t diff=(n!=m)?1:0;for(size_t i=0;i<n;i++)diff|=(uint8_t)(a[i]^(m?b[i%m]:0));return diff==0;}
// Non-blocking brute-force brake: 1 s after a failure, 30 s from the fifth one on.
static bool loginBlocked(){return loginFailures&&millis()-lastLoginFailureMs<(loginFailures<5?1000UL:30000UL);}
static void noteLoginFailure(){if(loginFailures<255)loginFailures++;lastLoginFailureMs=millis();}
static String cookieToken(){if(!server.hasHeader("Cookie"))return String();const String c=server.header("Cookie");int at=c.indexOf("wf_session=");if(at<0)return String();at+=11;const int end=c.indexOf(';',at);return end<0?c.substring(at):c.substring(at,end);}
static Session *findSession(){const String t=cookieToken();if(t.length()!=32)return nullptr;for(Session &s:sessions){if(!s.token[0])continue;if(millis()-s.createdMs>=kSessionTtlMs){s.token[0]=0;continue;}if(constantTimeEquals(t,String(s.token)))return &s;}return nullptr;}
static String createSession(){Session *slot=&sessions[0];for(Session &s:sessions){if(!s.token[0]){slot=&s;break;}if(millis()-s.createdMs>millis()-slot->createdMs)slot=&s;}snprintf(slot->token,sizeof(slot->token),"%08lx%08lx%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random(),(unsigned long)esp_random());slot->createdMs=millis();return String(slot->token);}
static bool basicAuthOk(){if(!server.hasHeader("Authorization")||loginBlocked())return false;if(server.authenticate("admin",uiPassword.c_str())){loginFailures=0;return true;}noteLoginFailure();return false;}
static bool isAuthenticated(){return !authEnabled||findSession()!=nullptr||basicAuthOk();}
// For API handlers: 401 without a WWW-Authenticate header, so browsers show no popup.
static bool authorized(){if(isAuthenticated())return true;server.send(401,"application/json","{\"error\":\"login_required\"}");return false;}
static void redirectTo(const String &location){server.sendHeader("Location",location);server.send(302,"text/plain","");}
// For HTML pages: send the visitor to the login page.
static bool pageAuthorized(const char *page){if(isAuthenticated())return true;redirectTo(String("/login?redir=")+page);return false;}
// getString() logs an error for a key that was never saved, so ask first.
static String prefString(const char *key,const String &def){return prefs.isKey(key)?prefs.getString(key,def):def;}
static uint32_t uptimeS(){return (millis()-bootMs)/1000;}
static String jsonEscape(const String &value);
static void setWifiLed(bool on){digitalWrite(8,on?LOW:HIGH);} // Super Mini blue LED is active-low on GPIO8.

static void addActivity(const char *type,int32_t value=0){Activity &a=activities[activityHead];a.sec=uptimeS();time_t now=time(nullptr);a.epoch=now>1600000000?(uint32_t)now:0;a.value=value;strncpy(a.type,type,sizeof(a.type)-1);a.type[sizeof(a.type)-1]='\0';activityHead=(activityHead+1)%100;if(activityCount<100)activityCount++;}
static uint8_t currentIndicatorState(){if(!runEnabled)return 0;return heaterOn?1:(initialPreheatDone?2:0);} // 0=idle (blue), 1=heating (red), 2=ready/target reached (green)
static void addHeaterEvent(uint32_t sec,uint8_t state,bool baseline=false){heaterEvents[heaterHead]={sec,state,(uint8_t)(baseline?1:0)};heaterHead=(heaterHead+1)%200;if(heaterCount<200)heaterCount++;}
static void addSample(){Sample &s=history[histHead];s.sec=uptimeS();s.temp10=isnan(tempC)?-32768:(int16_t)(tempC*10);s.set10=(int16_t)(targetC*10);s.heater=currentIndicatorState();s.lid=lidClosed?0:1;s.run=runEnabled?1:0;histHead=(histHead+1)%1080;if(histCount<1080)histCount++;}
static float avgBake(){return bakeDurationN?(float)bakeDurationSum/bakeDurationN:NAN;}
static float avgOpen(){return lidOpenDurationN?(float)lidOpenDurationSum/lidOpenDurationN:NAN;}
static float ratePerHour(){if(completedTimesN<2)return NAN;uint8_t intervals=completedTimesN-1;uint64_t sum=0;for(uint8_t i=0;i<intervals;i++){uint8_t a=(completedTimesHead+11-completedTimesN+i)%11;uint8_t b=(a+1)%11;sum+=(uint32_t)(completedTimes[b]-completedTimes[a]);}float mean=(float)sum/intervals;return mean>0?3600.0f/mean:NAN;}
static void completeBake(){uint32_t now=uptimeS();uint32_t total=(millis()-bakeStartedMs)/1000;completed++;bakeDurationSum+=total;bakeDurationN++;completedTimes[completedTimesHead]=now;completedTimesHead=(completedTimesHead+1)%11;if(completedTimesN<11)completedTimesN++;addActivity("bake_complete",total);Serial.printf("[ESP] Bake completed: count=%lu, duration=%lu s\n",(unsigned long)completed,(unsigned long)total);bakeState=0;bakeElapsedS=0;overtimeS=0;}

static void parseNano(char *line){if(!validFrame(line)){Serial.printf("[ESP CRC ERR] %s\n",line);return;}char *save=nullptr;char *kind=strtok_r(line,",",&save);char *seqStr=strtok_r(nullptr,",",&save);if(!kind||!seqStr)return;
  if(!strcmp(kind,"ACK")){lastAckSeq=strtoul(seqStr,nullptr,10);char *result=strtok_r(nullptr,",",&save);lastAckOk=result&&!strcmp(result,"OK");if(!lastAckOk)Serial.printf("[ESP] Nano rejected command sequence %lu\n",(unsigned long)lastAckSeq);else{lastNanoFrameMs=millis();if(settingsSyncPending&&lastAckSeq==settingsCommandSeq)settingsSyncPending=false;}return;}
  if(strcmp(kind,"IO")&&strcmp(kind,"EV"))return;uint32_t incomingSeq=strtoul(seqStr,nullptr,10);if(haveNanoSeq&&(int32_t)(incomingSeq-lastNanoSeq)<=0){if((int32_t)(lastNanoSeq-incomingSeq)>50){lastNanoSeq=incomingSeq;}else{return;}}lastNanoSeq=incomingSeq;haveNanoSeq=true;lastNanoFrameMs=millis();
  if(!strcmp(kind,"IO")){char *t=strtok_r(nullptr,",",&save);char *closed=strtok_r(nullptr,",",&save);char *heat=strtok_r(nullptr,",",&save);char *fault=strtok_r(nullptr,",",&save);char *editMode=strtok_r(nullptr,",",&save);char *openS=strtok_r(nullptr,",",&save);char *nanoLinkFlag=strtok_r(nullptr,",",&save);char *legacyT=strtok_r(nullptr,",",&save);char *legacyD=strtok_r(nullptr,",",&save);
    bool wasClosed=lidClosed,wasHeater=heaterOn;if(t){float v=atof(t);tempC=(v<0||v>250)?NAN:v;}if(closed)lidClosed=atoi(closed)!=0;if(heat)heaterOn=atoi(heat)!=0;if(fault)sensorFault=atoi(fault)!=0;if(!haveHeaterState){addHeaterEvent(uptimeS(),currentIndicatorState(),true);haveHeaterState=true;}else if(wasHeater!=heaterOn){addHeaterEvent(uptimeS(),currentIndicatorState());addActivity(heaterOn?"heater_on":"heater_off");}
    if(runEnabled&&!initialPreheatDone&&!isnan(tempC)&&tempC>=targetC){initialPreheatDone=true;addHeaterEvent(uptimeS(),currentIndicatorState());addActivity("temp_reached",(int32_t)lroundf(tempC));}
    if(wasClosed&&!lidClosed){lidOpenStartMs=millis();openingEvents++;addActivity("lid_open");if(bakeState==2)completeBake();else {bakeState=0;bakeElapsedS=0;overtimeS=0;}}
    if(!wasClosed&&lidClosed&&lidOpenStartMs){uint32_t d=(millis()-lidOpenStartMs)/1000;lidOpenDurationSum+=d;lidOpenDurationN++;addActivity("lid_closed",d);lidOpenStartMs=0;}
    if(legacyT&&legacyD){int lt=atoi(legacyT),ld=atoi(legacyD);if(lt>=WAFFLE_MIN_TEMPERATURE_C&&lt<=WAFFLE_MAX_TEMPERATURE_C&&ld>=WAFFLE_MIN_BAKE_TIME_S&&ld<=WAFFLE_MAX_BAKE_TIME_S){if(importTargetPending||targetC<WAFFLE_MIN_TEMPERATURE_C||targetC>WAFFLE_MAX_TEMPERATURE_C){targetC=lt;prefs.putInt("target",lt);importTargetPending=false;}if(importDurationPending||durationS<WAFFLE_MIN_BAKE_TIME_S||durationS>WAFFLE_MAX_BAKE_TIME_S){durationS=ld;prefs.putInt("duration",ld);importDurationPending=false;}}}
    if(editMode)encoderTimeMode=atoi(editMode)==2;
    nanoHeartbeatReady=nanoLinkFlag&&atoi(nanoLinkFlag)!=0;
    if(openS&&atoi(openS)>0){} // duration is measured locally between stable transitions
  } else if(!strcmp(kind,"EV")){char *ev=strtok_r(nullptr,",",&save);if(ev&&!strcmp(ev,"START")){if(nanoUp&&!sensorFault){runEnabled=true;initialPreheatDone=(!isnan(tempC)&&tempC>=targetC);addHeaterEvent(uptimeS(),currentIndicatorState());addActivity("run_started");Serial.println(F("[ESP] Local START accepted"));}else Serial.println(F("[ESP] Local START rejected: Nano offline or sensor fault"));}else if(ev&&!strcmp(ev,"STOP")){runEnabled=false;bakeState=0;initialPreheatDone=false;addHeaterEvent(uptimeS(),currentIndicatorState());addActivity("run_stopped");Serial.println(F("[ESP] Local STOP received"));}else if(ev&&!strcmp(ev,"SET")){char *t=strtok_r(nullptr,",",&save);char *d=strtok_r(nullptr,",",&save);int newTarget=t?atoi(t):-1,newDuration=d?atoi(d):-1;if(!importTargetPending&&!importDurationPending&&newTarget>=WAFFLE_MIN_TEMPERATURE_C&&newTarget<=WAFFLE_MAX_TEMPERATURE_C&&newDuration>=WAFFLE_MIN_BAKE_TIME_S&&newDuration<=WAFFLE_MAX_BAKE_TIME_S){if(targetC!=newTarget){addActivity("target_changed",newTarget);if(newTarget>targetC&&tempC<newTarget)initialPreheatDone=false;}if(durationS!=newDuration)addActivity("time_changed",newDuration);targetC=newTarget;durationS=newDuration;settingsDirty=true;settingsChangedMs=millis();Serial.printf("[ESP] Nano settings changed: target=%d C, duration=%d s\n",targetC,durationS);}}else if(ev&&!strcmp(ev,"ENC")){char *deltaText=strtok_r(nullptr,",",&save);int delta=deltaText?atoi(deltaText):0;if(!importTargetPending&&!importDurationPending&&delta){if(encoderTimeMode){durationS=constrain(durationS+delta,WAFFLE_MIN_BAKE_TIME_S,WAFFLE_MAX_BAKE_TIME_S);addActivity("time_changed",durationS);}else{int nextTarget=constrain(targetC+delta,WAFFLE_MIN_TEMPERATURE_C,WAFFLE_MAX_TEMPERATURE_C);if(nextTarget>targetC&&tempC<nextTarget)initialPreheatDone=false;targetC=nextTarget;addActivity("target_changed",targetC);}settingsDirty=true;settingsChangedMs=millis();Serial.printf("[ESP] Encoder changed setting: target=%d C, duration=%d s\n",targetC,durationS);}}}
}
static void pollNano(){while(nanoLink.available()){char c=(char)nanoLink.read();if(c=='\n'||c=='\r'){if(rxLen){rxLine[rxLen]=0;
#if WAFFLE_DEBUG
Serial.printf("[ESP RX] %s\n",rxLine);
#endif
parseNano(rxLine);rxLen=0;}}else if(rxLen<sizeof(rxLine)-1)rxLine[rxLen++]=c;else rxLen=0;}}

static void updateBake(){uint32_t now=millis();if(!nanoUp||sensorFault||!runEnabled){bakeState=0;initialPreheatDone=false;return;}if(!initialPreheatDone){if(!isnan(tempC)&&tempC>=targetC){initialPreheatDone=true;addHeaterEvent(uptimeS(),currentIndicatorState());addActivity("temp_reached",(int32_t)lroundf(tempC));}}if(!lidClosed){if(bakeState==1){bakeState=0;}return;}if(bakeState==0){if(!initialPreheatDone)return;bakeState=1;bakeStartedMs=now;bakeElapsedS=0;overtimeS=0;}if(bakeState==1){bakeElapsedS=(now-bakeStartedMs)/1000;if(bakeElapsedS>=durationS){bakeState=2;addActivity("timer_done",durationS);}}if(bakeState==2)overtimeS=(now-bakeStartedMs)/1000-durationS;}

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
  j+=",\"auth\":"+String(authEnabled?"true":"false")+",\"ntpEnabled\":"+String(ntpEnabled?"true":"false")+",\"ntpServer\":\""+jsonEscape(ntpServerName)+"\",\"timezone\":\""+jsonEscape(timezoneName)+"\",\"timeFormat\":\""+timeFormat+"\",\"timeSynced\":"+String(timeSynced?"true":"false")+",\"epoch\":"+String((uint32_t)epoch);
  j+=",\"staticIpEnabled\":"+String(staticIpEnabled?"true":"false")+",\"staticIp\":\""+jsonEscape(staticIpText.length()?staticIpText:ip)+"\",\"subnet\":\""+jsonEscape(staticIpEnabled?subnetText:subnet)+"\",\"gateway\":\""+jsonEscape(staticIpEnabled?gatewayText:gateway)+"\",\"dns\":\""+jsonEscape(staticIpEnabled?dnsText:dnsServer)+"\",\"currentSubnet\":\""+subnet+"\",\"currentGateway\":\""+gateway+"\",\"currentDns\":\""+dnsServer+"\"";
  j+=",\"encoderMode\":"+String(encoderTimeMode?"\"time\"":"\"temperature\"")+",\"nanoAckSeq\":"+String(lastAckSeq)+",\"nanoAckOk\":"+String(lastAckOk?"true":"false")+",\"heater\":"+String(heaterOn?"true":"false")+",\"lidClosed\":"+String(lidClosed?"true":"false")+",\"nanoOnline\":"+String(nanoUp?"true":"false")+",\"fault\":"+String(sensorFault?"true":"false")+",\"run\":"+String(runEnabled?"true":"false")+",\"count\":"+String(completed)+",\"remaining\":"+String(bakeState==1&&bakeElapsedS<durationS?durationS-bakeElapsedS:0)+",\"overtime\":"+String(overtimeS);
  j+=",\"bakeAvg\":"+(isnan(avgBake())?String("null"):String(avgBake(),1))+",\"openAvg\":"+(isnan(avgOpen())?String("null"):String(avgOpen(),1))+",\"rate\":"+(isnan(ratePerHour())?String("null"):String(ratePerHour(),1))+",\"uptime\":"+String(uptimeS())+",\"wifiConnected\":"+String(WiFi.status()==WL_CONNECTED?"true":"false")+",\"wifiSsid\":\""+jsonEscape(ssid)+"\",\"wifiRssi\":"+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0)+",\"ip\":\""+ip+"\",\"hostname\":\""+jsonEscape(deviceHostname)+"\"";
  float chipT=temperatureRead();
  j+=",\"freeHeap\":"+String(ESP.getFreeHeap())+",\"minFreeHeap\":"+String(ESP.getMinFreeHeap())+",\"maxAllocHeap\":"+String(ESP.getMaxAllocHeap())+",\"cpuMhz\":"+String(ESP.getCpuFreqMHz())+",\"cpuLoad\":"+String(cpuLoad,1)+",\"flashSize\":"+String(ESP.getFlashChipSize())+",\"sketchFree\":"+String(ESP.getFreeSketchSpace())+",\"chip\":\""+String(ESP.getChipModel())+"\",\"chipRevision\":"+String(ESP.getChipRevision())+",\"chipTemp\":"+(isnan(chipT)?String("null"):String(chipT,1))+",\"resetReason\":\""+String(resetReasonName())+"\"}";
  return j;
}
static void sendStatus(){if(!authorized())return;server.send(200,"application/json",statusJson());}
static void sendHistory(){if(!authorized())return;uint32_t from=server.hasArg("from")?(uint32_t)server.arg("from").toInt():0;String j="{\"samples\":[";bool first=true;uint16_t start=(histHead+1080-histCount)%1080;for(uint16_t i=0;i<histCount;i++){Sample &s=history[(start+i)%1080];if(from&&s.sec<=from)continue;if(!first)j+=',';first=false;j+="["+String(s.sec)+","+(s.temp10==-32768?String("null"):String(s.temp10/10.0f,1))+","+String(s.set10/10.0f,1)+","+String(s.heater)+","+String(s.lid)+"]";}j+="]}";server.send(200,"application/json",j);}
static void sendEvents(){if(!authorized())return;String j="{\"activities\":[";uint16_t start=(activityHead+100-activityCount)%100;for(uint16_t i=0;i<activityCount;i++){Activity &a=activities[(start+i)%100];if(i)j+=',';j+="{\"sec\":"+String(a.sec)+",\"epoch\":"+String(a.epoch)+",\"type\":\""+String(a.type)+"\",\"value\":"+String(a.value)+"}";}j+="],\"heater\":[";bool f=true;start=(heaterHead+200-heaterCount)%200;for(uint16_t i=0;i<heaterCount;i++){HeaterEvent &e=heaterEvents[(start+i)%200];if(!f)j+=',';f=false;j+="["+String(e.sec)+","+String(e.state)+","+String(e.baseline)+"]";}j+="]}";server.send(200,"application/json",j);}

static const char FAVICON[] PROGMEM=R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48"><rect width="48" height="48" rx="12" fill="#20a99d"/><g fill="none" stroke="#fff" stroke-width="2.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="24" cy="24" r="12"/><path d="M24 7v34M9.3 15.5l29.4 17M38.7 15.5l-29.4 17"/></g></svg>)SVG";


static void handleWifi();
static void handleRoot(){if(!pageAuthorized(server.uri()=="/settings"?"/settings":"/"))return;String html=FPSTR(PAGE);String tempAttrs="min=\"";tempAttrs+=WAFFLE_MIN_TEMPERATURE_C;tempAttrs+="\" max=\"";tempAttrs+=WAFFLE_MAX_TEMPERATURE_C;tempAttrs+="\" value=\"";tempAttrs+=WAFFLE_DEFAULT_TEMPERATURE_C;tempAttrs+='\"';String timeAttrs="min=\"";timeAttrs+=WAFFLE_MIN_BAKE_TIME_S;timeAttrs+="\" max=\"";timeAttrs+=WAFFLE_MAX_BAKE_TIME_S;timeAttrs+="\" value=\"";timeAttrs+=WAFFLE_DEFAULT_BAKE_TIME_S;timeAttrs+='\"';html.replace("min=\"100\" max=\"180\" value=\"134\"",tempAttrs);html.replace("min=\"10\" max=\"90\" value=\"30\"",timeAttrs);server.send(200,"text/html; charset=utf-8",html);}
static void handleWifi(){if(!apMode&&!pageAuthorized("/wifi"))return;String html=R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#f3f6fb"><link rel="icon" type="image/svg+xml" href="/favicon.svg"><title>Wi-Fi setup · Oblátkovač</title><style>
:root{color-scheme:light;--bg:#f3f6fb;--surface:#fff;--surface2:#f7f9fc;--ink:#17233d;--muted:#718096;--line:#dce4ee;--blue:#1683b4;--blue-dark:#102850;--teal:#19a99f;--green:#20a765;--red:#dc3545;--amber:#d99812;--shadow:0 4px 16px #13243b0c}body.dark{color-scheme:dark;--bg:#0c1424;--surface:#151f32;--surface2:#101a2c;--ink:#e8eff9;--muted:#9aaac0;--line:#2b3b54;--blue:#43b7e5;--blue-dark:#152c55;--teal:#35c5b8;--green:#4dcc8b;--red:#ff6877;--amber:#edbd57;--shadow:0 4px 18px #0003}*{box-sizing:border-box}body{margin:0;min-height:100vh;background:var(--bg);color:var(--ink);font:14px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;display:flex;flex-direction:column;transition:background .2s,color .2s}button,input,select{font:inherit}button,a,select{ -webkit-tap-highlight-color:transparent}.hidden{display:none!important}.topbar{position:sticky;top:0;z-index:5;height:66px;background:color-mix(in srgb,var(--surface) 96%,transparent);border-top:4px solid var(--teal);border-bottom:1px solid var(--line);box-shadow:var(--shadow);backdrop-filter:blur(12px)}.top-inner{width:100%;max-width:none;height:100%;margin:0;padding:0 24px;display:grid;grid-template-columns:auto 1fr auto auto;gap:20px;align-items:center}.brand{display:flex;align-items:center;gap:9px;color:var(--blue);font-size:21.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;white-space:nowrap;text-decoration:none;justify-self:start}.brand-mark{width:31px;height:31px;color:#fff;background:linear-gradient(135deg,#28b5a7,#1689a1);border-radius:10px;display:grid;place-items:center}.brand-mark svg{width:21px;height:21px}.quick{display:flex;gap:7px;align-items:center;justify-content:center}.nav{display:flex;align-items:center;gap:5px;justify-self:end}.nav-link{position:relative;display:flex;align-items:center;gap:7px;text-decoration:none;color:var(--muted);padding:9px 11px;border:1px solid var(--line);border-radius:9px;font-weight:600;white-space:nowrap;transition:color .16s,background .16s,transform .16s}.nav-link:hover{color:var(--blue);background:var(--surface2);border-color:var(--blue);transform:translateY(-1px)}.nav-link.active{background:var(--blue-dark);color:#fff;box-shadow:0 3px 10px #101f4933}.nav-link.logout .nav-text{display:none}.nav-link.logout .nav-icon{color:var(--red)}.nav-link.logout:hover{border-color:var(--red)}.nav-icon svg{width:17px;height:17px}.nav-icon{width:17px;height:17px;display:inline-grid;place-items:center;font-size:15px;line-height:1}.right-tools{display:flex;align-items:center;gap:9px;justify-self:end}.select{background:var(--surface);color:var(--ink);border:1px solid var(--line);border-radius:8px;padding:7px 8px}.theme-switch{display:flex;padding:3px;border:1px solid var(--line);background:var(--surface2);border-radius:999px;gap:2px}.theme-option{width:30px;height:28px;border:0;background:transparent;color:var(--muted);border-radius:999px;display:grid;place-items:center;cursor:pointer}.theme-option:hover{color:var(--blue);background:var(--surface)}.theme-option.active{background:var(--surface);color:var(--blue);box-shadow:0 1px 5px #12233c22}.theme-option svg{width:15px;height:15px}.lang-option{min-width:34px;height:28px;padding:0 9px;border:0;background:transparent;color:var(--muted);border-radius:999px;font-size:12px;font-weight:600;letter-spacing:.4px;cursor:pointer}.lang-option:hover{color:var(--blue);background:var(--surface)}.lang-option.active{background:var(--surface);color:var(--blue);box-shadow:0 1px 5px #12233c22}.layout{width:min(1110px,92vw);margin:auto;display:grid;grid-template-columns:1fr 1fr;gap:34px;align-items:center;padding:42px 0}.intro h1{font-weight:600;font-size:clamp(28px,4.5vw,42px);line-height:1.1;margin:14px 0;color:var(--blue-dark)}body.dark .intro h1{color:var(--ink)}.intro p{color:var(--muted);line-height:1.65;max-width:480px}.eyebrow{color:var(--teal);text-transform:uppercase;letter-spacing:1.5px;font-size:12px;font-weight:600}.art{height:150px;max-width:360px;display:flex;align-items:center;gap:16px;margin:20px 0}.device{width:110px;height:132px;border:1px solid var(--line);border-radius:18px;background:var(--surface);display:grid;place-items:center;box-shadow:var(--shadow)}.device svg{width:54px;height:54px;color:var(--teal)}.wave{display:flex;align-items:center;gap:8px}.wave i{display:block;width:8px;border-radius:8px;background:var(--teal);opacity:.8}.wave i:nth-child(1),.wave i:nth-child(5){height:22px;opacity:.35}.wave i:nth-child(2),.wave i:nth-child(4){height:42px;opacity:.6}.wave i:nth-child(3){height:65px}.card{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:25px;box-shadow:var(--shadow)}.card h2{font-weight:600;margin:0;font-size:21px;color:var(--blue-dark)}body.dark .card h2{color:var(--ink)}.sub{color:var(--muted);font-size:13px;margin:7px 0 20px}.network{display:flex;align-items:center;gap:12px;background:var(--surface2);border:1px solid var(--line);border-radius:10px;padding:12px 14px;margin-bottom:20px}.wifiIcon{width:40px;height:40px;border-radius:12px;background:color-mix(in srgb,var(--teal) 16%,var(--surface));color:var(--teal);display:grid;place-items:center}.wifiIcon svg{width:23px;height:23px}.netText small{display:block;color:var(--muted);font-size:11px;margin-bottom:3px}.netText strong{font-size:14px}.scan{border:1px solid var(--line);border-radius:8px;background:var(--surface);color:var(--blue);padding:9px 12px;font-weight:600;cursor:pointer;transition:border-color .15s,box-shadow .15s}.scan:hover{border-color:var(--blue);box-shadow:0 4px 12px #15355716}.scan:disabled{opacity:.6;cursor:default}.net-item{text-align:left;border:1px solid var(--line);border-radius:8px;background:var(--surface2);color:var(--ink);padding:10px 12px;cursor:pointer}.net-item:hover{border-color:var(--blue)}.field{margin:15px 0}.field label{display:block;color:var(--muted);font-size:12px;font-weight:600;margin-bottom:7px}.field input{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:var(--surface2);color:var(--ink);font-size:14px;outline:none}.field input:focus{border-color:var(--teal);box-shadow:0 0 0 3px color-mix(in srgb,var(--teal) 16%,transparent)}.pass{position:relative}.pass input{padding-right:44px}.show{position:absolute;right:6px;top:50%;transform:translateY(-50%);width:32px;height:28px;padding:0;border:0;background:transparent;color:var(--muted);cursor:pointer;display:grid;place-items:center}.show:hover{color:var(--blue)}.show svg{width:17px;height:17px}.show .slash{display:none}.show.shown .slash{display:inline}.submit{width:100%;border:1px solid var(--blue);border-radius:8px;padding:11px;background:var(--blue);color:#fff;font-size:14px;font-weight:600;cursor:pointer;transition:filter .15s,box-shadow .15s}.submit:hover{filter:brightness(1.08);box-shadow:0 4px 12px #15355716}.hint{font-size:12px;color:var(--muted);line-height:1.5;margin:15px 0 0}.footer{text-align:center;color:var(--muted);font-size:11px;padding:15px 12px 22px}.steps{display:flex;gap:10px;margin-top:24px;color:var(--muted);font-size:12px}.step{padding:9px 11px;border:1px solid var(--line);border-radius:9px;background:var(--surface)}.step b{color:var(--teal);margin-right:5px}@media(max-width:760px){.layout{grid-template-columns:1fr;gap:10px;padding:30px 0}.intro p{max-width:none}.art{height:90px;margin:12px 0}.device{height:80px;width:76px}.device svg{width:40px}.card{padding:21px}}@media(min-width:1580px){.top-inner{max-width:1150px;margin:0 auto;padding:0 20px;grid-template-columns:auto 1fr auto}.right-tools{position:absolute;right:24px;top:50%;transform:translateY(-50%)}}@media(max-width:1100px){.top-inner{grid-template-columns:auto 1fr auto;gap:12px;padding:8px 18px}.quick{order:4;grid-column:1/-1;width:100%;justify-content:center}}@media(max-width:520px){.brand>span:last-child{display:none}}@media(max-width:750px){.top-inner{padding:8px 12px}.brand{font-size:17px}.quick{width:100%;flex-wrap:wrap}.nav-link{padding:7px 8px;font-size:12px}.nav-link .nav-text{display:none}}@media(min-width:1101px) and (max-width:1220px){.top-inner{gap:12px;padding:0 16px}.nav-link .nav-text{display:none}}@media(max-width:1100px){.topbar{height:auto;min-height:66px}.top-inner{height:auto;min-height:61px;align-content:start;row-gap:8px;padding-block:10px 11px}.quick:empty{display:none}}@media(max-width:750px){.topbar{position:static}.top-inner{padding-block:12px 13px}}@media(max-width:520px){.top-inner{column-gap:8px;padding-inline:10px}.right-tools{gap:6px}.theme-option{width:28px}.lang-option{min-width:30px;padding:0 7px}.quick{display:grid;grid-template-columns:repeat(2,minmax(0,1fr))}.quick label{min-width:0;justify-content:space-between}}
</style></head><body><header class="topbar"><div class="top-inner"><div class="brand"><span class="brand-mark"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="8"/><path d="M12 2v20M3.3 7l17.4 10M20.7 7 3.3 17"/></svg></span><span>Oblátkovač</span></div><div class="quick"></div><nav class="nav"><a class="nav-link" href="/"><span class="nav-icon" aria-hidden="true">📊</span><span class="nav-text" data-i18n="dashboard">Dashboard</span></a><a class="nav-link active" href="/settings"><span class="nav-icon" aria-hidden="true">⚙️</span><span class="nav-text" data-i18n="settings">Settings</span></a><a id="logoutNav" class="nav-link logout hidden" href="/logout" title="Log out" aria-label="Log out"><span class="nav-icon" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 4H5v16h5M14 8l4 4-4 4M18 12H9"/></svg></span><span class="nav-text" data-i18n="logout">Log out</span></a></nav><div class="right-tools"><div class="theme-switch" role="group" aria-label="Language"><button type="button" class="lang-option" data-lang="sk" onclick="changePortalLanguage('sk')">SK</button><button type="button" class="lang-option" data-lang="en" onclick="changePortalLanguage('en')">EN</button></div><div class="theme-switch" role="group" aria-label="Color theme"><button class="theme-option" data-theme="light" title="Light theme" aria-label="Light theme" onclick="setTheme('light')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4M2 12h2m16 0h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg></button><button class="theme-option" data-theme="dark" title="Dark theme" aria-label="Dark theme" onclick="setTheme('dark')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><path d="M20.5 15.2A8.5 8.5 0 0 1 8.8 3.5 8.8 8.8 0 1 0 20.5 15.2Z"/></svg></button><button class="theme-option" data-theme="auto" title="Use device theme" aria-label="Use device theme" onclick="setTheme('auto')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8m-4-4v4"/></svg></button></div></div></div></header><main class="layout"><section class="intro"><div class="eyebrow" data-i18n="firstConnection">First connection</div><h1 data-i18n="connectWifi">Connect to your Wi-Fi.</h1><p data-i18n="wifiIntro">Enter your home network name and password. After saving, the ESP32 will restart and the dashboard will be available on your local network.</p><div class="art"><div class="device"><svg viewBox="0 0 48 48" fill="none" stroke="currentColor" stroke-width="2"><rect x="11" y="5" width="26" height="38" rx="5"/><path d="M19 12h10M19 36h10M17 23h14M20 19l-3 4 3 4M28 19l3 4-3 4"/></svg></div><div class="wave"><i></i><i></i><i></i><i></i><i></i></div><div style="font-size:28px">⌂</div></div><div class="steps"><span class="step"><b>1</b> <span data-i18n="stepChoose">Choose a network</span></span><span class="step"><b>2</b> <span data-i18n="stepPassword">Enter password</span></span><span class="step"><b>3</b> <span data-i18n="stepSave">Save</span></span></div></section><section class="card"><h2 data-i18n="wifiSettings">Wi-Fi connection</h2><p class="sub" data-i18n="savedLocally">Your credentials are stored on the ESP32.</p><div class="network"><div class="wifiIcon"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M2 8.8a15 15 0 0 1 20 0M5 12a10.5 10.5 0 0 1 14 0m-11 3.4a5.5 5.5 0 0 1 8 0M12 19h.01"/></svg></div><div class="netText"><small data-i18n="currentAp">Current access point</small><strong>@@SSID@@</strong></div></div><div style="display:flex;gap:10px;align-items:center;margin:0 0 10px"><button id="scanButton" type="button" onclick="scanWifi()" class="scan" data-i18n="scan">Scan for Wi-Fi networks</button><span id="scanState" style="font-size:12px;color:var(--muted)"></span></div><div id="wifiNetworks" style="display:grid;gap:7px;margin-bottom:14px"></div><form method="post" action="/savewifi"><div class="field"><label for="ssid" data-i18n="ssid">Wi-Fi network name (SSID)</label><input id="ssid" name="ssid" autocomplete="off" required placeholder="e.g. HomeWiFi" data-i18n-placeholder="ssidPlaceholder"></div><div class="field"><label for="pass" data-i18n="password">Wi-Fi password</label><div class="pass"><input id="pass" name="pass" type="password" autocomplete="new-password" placeholder="Enter password" data-i18n-placeholder="wifiPasswordPlaceholder"><button class="show" type="button" onclick="togglePortalPass(this)" title="Show" aria-label="Show"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/><path class="slash" d="M3 21 21 3"/></svg></button></div></div><button class="submit" type="submit" data-i18n="saveConnect">Save and connect&nbsp; →</button></form><p class="hint" data-i18n="reconnectHint">After restart, reconnect to your Wi-Fi. If the connection fails, the device will start this setup access point again.</p></section></main><footer class="footer">Oblátkovač · <span id="firmwareVersion">Firmware</span> · <span data-i18n="pageLoad">Page load</span>: <span id="pageLoadTime">—</span></footer><script>let portalLang="en",portalWords={};function portalTr(k){return portalWords[k]||k}let scanStatus=null;function renderScanStatus(){document.getElementById("scanState").textContent=scanStatus?portalTr(scanStatus.key)+(scanStatus.count?" "+scanStatus.count:""):""}function setScanStatus(key,count){scanStatus={key,count};renderScanStatus()}function labelEye(b){const label=portalTr(b.classList.contains("shown")?"hide":"show");b.title=label;b.setAttribute("aria-label",label)}function togglePortalPass(b){const p=document.getElementById("pass");p.type=p.type==="password"?"text":"password";b.classList.toggle("shown",p.type==="text");labelEye(b)}function applyPortalLanguage(){document.documentElement.lang=portalLang;document.querySelectorAll("[data-i18n]").forEach(e=>{if(portalWords[e.dataset.i18n])e.textContent=portalWords[e.dataset.i18n]});document.querySelectorAll("[data-i18n-placeholder]").forEach(e=>{if(portalWords[e.dataset.i18nPlaceholder])e.placeholder=portalWords[e.dataset.i18nPlaceholder]});document.querySelectorAll(".lang-option").forEach(b=>b.classList.toggle("active",b.dataset.lang===portalLang));if(portalWords.show)document.querySelectorAll(".show").forEach(labelEye);renderScanStatus();const sb=document.getElementById("scanButton");if(sb.disabled)sb.textContent=portalTr("scanning")}async function changePortalLanguage(l){try{portalLang=l==="sk"?"sk":"en";let p=await fetch("/api/language",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:new URLSearchParams({language:portalLang})});if(!p.ok)throw Error();let t=await fetch("/api/translations?lang="+portalLang,{cache:"no-store"});portalWords=await t.json();applyPortalLanguage()}catch(e){}}async function initPortalLanguage(){try{let p=await fetch("/api/language",{cache:"no-store"}),d=await p.json();portalLang=d.language==="sk"?"sk":"en";let t=await fetch("/api/translations?lang="+portalLang,{cache:"no-store"});portalWords=await t.json();applyPortalLanguage()}catch(e){}}initPortalLanguage();async function scanWifi(){const b=document.getElementById("scanButton"),list=document.getElementById("wifiNetworks");b.disabled=true;b.textContent=portalTr("scanning");setScanStatus("scanning");list.replaceChildren();try{let r=await fetch("/api/wifi-scan?start=1",{cache:"no-store"});if(r.status===401){location.href="/login?redir=/wifi";return}let d=await r.json(),tries=0;while(d.state==="scanning"&&tries++<30){await new Promise(ok=>setTimeout(ok,500));r=await fetch("/api/wifi-scan",{cache:"no-store"});d=await r.json()}if(d.state!=="done"){setScanStatus("scanFailed");return}if(!d.networks.length){setScanStatus("noNetworks");return}setScanStatus("networksFound",d.networks.length);for(const n of d.networks){const item=document.createElement("button");item.type="button";item.className="net-item";item.textContent=(n.secure?"🔒 ":"")+"📶 "+n.ssid+" · "+n.rssi+" dBm";item.onclick=()=>{document.getElementById("ssid").value=n.ssid;document.getElementById("pass").focus()};list.appendChild(item)}}catch(e){setScanStatus("scanError")}finally{b.disabled=false;b.textContent=portalTr("scan")}}let themeMode="auto";try{themeMode=localStorage.getItem("waffleTheme")||"auto"}catch(e){}function applyTheme(){const d=themeMode==="dark"||(themeMode==="auto"&&matchMedia("(prefers-color-scheme: dark)").matches);document.body.classList.toggle("dark",d);document.querySelectorAll(".theme-option").forEach(b=>b.classList.toggle("active",b.dataset.theme===themeMode))}function setTheme(m){themeMode=m;try{localStorage.setItem("waffleTheme",m)}catch(e){}applyTheme()}try{matchMedia("(prefers-color-scheme: dark)").addEventListener("change",()=>{if(themeMode==="auto")applyTheme()})}catch(e){}applyTheme();fetch('/api/version',{cache:'no-store'}).then(r=>r.json()).then(v=>{document.getElementById('firmwareVersion').textContent='v'+v.version+' · '+v.buildDate+' '+v.buildTime;document.getElementById('logoutNav').classList.toggle('hidden',!v.auth)}).catch(()=>{});window.addEventListener('load',()=>{document.getElementById('pageLoadTime').textContent=(performance.now()/1000).toFixed(2)+' s'});</script></body></html>)HTML";String shownSsid=(!apMode&&WiFi.status()==WL_CONNECTED)?WiFi.SSID():(setupApSsid.length()?setupApSsid:String("Oblatkovac-Setup"));shownSsid.replace("&","&amp;");shownSsid.replace("<","&lt;");shownSsid.replace(">","&gt;");html.replace("@@SSID@@",shownSsid);server.send(200,"text/html; charset=utf-8",html);}
static void handleSaveWifi(){if(!apMode&&!authorized())return;prefs.putString("ssid",server.arg("ssid"));prefs.putString("wifiPass",server.arg("pass"));Serial.printf("[ESP] Saved Wi-Fi network '%s'; rebooting\n",server.arg("ssid").c_str());server.send(200,"text/plain","Saved. Restarting…");delay(500);ESP.restart();}
static void handleLanguage(){
  if(server.method()==HTTP_POST){String requested=server.arg("language");if(requested!="sk"&&requested!="en"){server.send(400,"application/json","{\"error\":\"Unsupported language\"}");return;}activeLanguage=requested;prefs.putString("language",activeLanguage);Serial.printf("[ESP] UI language set to %s\n",activeLanguage.c_str());}
  server.send(200,"application/json",String("{\"language\":\"")+activeLanguage+"\"}");
}
static void handleTranslations(){String requested=server.arg("lang");if(requested=="sk")server.send_P(200,"application/json; charset=utf-8",UI_LANGUAGE_SK);else server.send_P(200,"application/json; charset=utf-8",UI_LANGUAGE_EN);}
static String jsonEscape(const String &value){String out;out.reserve(value.length()+8);for(size_t i=0;i<value.length();i++){char c=value[i];if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if((uint8_t)c>=0x20)out+=c;}return out;}
static bool wifiScanRunning=false;
static void handleWifiScan(){
  if(!apMode&&!authorized())return;
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
static void handleControl(){if(!authorized())return;String c=server.arg("cmd");if(c=="STOP"){runEnabled=false;bakeState=0;initialPreheatDone=false;addHeaterEvent(uptimeS(),currentIndicatorState());addActivity("run_stopped");Serial.println(F("[ESP] Web STOP accepted"));if(nanoUp)lastControlMs=0;server.send(200,"application/json","{\"ok\":true}");}else if(c=="RUN"){if(!nanoUp||sensorFault){Serial.println(F("[ESP] Web START rejected: Nano offline or sensor fault"));server.send(409,"application/json","{\"ok\":false,\"error\":\"Nano offline or sensor fault\"}");return;}runEnabled=true;initialPreheatDone=(!isnan(tempC)&&tempC>=targetC);addHeaterEvent(uptimeS(),currentIndicatorState());addActivity("run_started");Serial.println(F("[ESP] Web START accepted"));if(nanoUp)lastControlMs=0;server.send(200,"application/json","{\"ok\":true}");}else server.send(400,"text/plain","Unknown command");}
static String timezoneRule(const String &zone){if(zone=="Europe/Bratislava"||zone=="Europe/Prague"||zone=="Europe/Berlin")return "CET-1CEST,M3.5.0,M10.5.0/3";if(zone=="Europe/London")return "GMT0BST,M3.5.0/1,M10.5.0";if(zone=="America/New_York")return "EST5EDT,M3.2.0,M11.1.0";if(zone=="America/Los_Angeles")return "PST8PDT,M3.2.0,M11.1.0";if(zone=="Asia/Tokyo")return "JST-9";return "UTC0";}
static void configureTime(){String rule=timezoneRule(timezoneName);setenv("TZ",rule.c_str(),1);tzset();timeConfigStarted=false;if(ntpEnabled&&WiFi.status()==WL_CONNECTED){configTzTime(rule.c_str(),ntpServerName.c_str());timeConfigStarted=true;}}
static bool validHostname(String host){host.trim();if(!host.length()||host.length()>32||host[0]=='-'||host[host.length()-1]=='-')return false;for(size_t i=0;i<host.length();i++){char c=host[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return false;}return true;}
// getEfuseMac() holds the first MAC octet in the lowest byte, so the last two octets are bits 32-47.
static String macSuffix(){const uint64_t mac=ESP.getEfuseMac();char hex[5];snprintf(hex,sizeof(hex),"%02x%02x",(uint8_t)(mac>>32),(uint8_t)(mac>>40));return String(hex);}
static String defaultHostname(){return String("oblatkovac-")+macSuffix();}
// Suffix used by earlier firmware; a saved copy of it is replaced by the current default.
static String legacyHostname(){char host[24];snprintf(host,sizeof(host),"oblatkovac-%06x",(uint32_t)(ESP.getEfuseMac()&0xFFFFFF));return String(host);}
// Saved password for the Settings form. Only for signed-in clients, and never while login is off,
// because then anyone on the network could read it. Never part of status, metrics or logs.
static void handlePassword(){if(!authorized())return;server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",String("{\"password\":\"")+(authEnabled?jsonEscape(uiPassword):String())+"\"}");}
static void sendSettingsError(const char *code){server.send(400,"application/json",String("{\"error\":\"")+code+"\"}");}
// Saves every panel of the Settings page. Everything is validated before anything is stored.
static void handleSaveSettings(){
  if(!authorized())return;
  if(!server.hasArg("authEnabled")||!server.hasArg("ntpEnabled")||!server.hasArg("staticIpEnabled")){sendSettingsError("invalid_request");return;}
  String host=server.arg("hostname"),ntp=server.arg("ntpServer");host.trim();ntp.trim();
  const String zone=server.arg("timezone"),format=server.arg("timeFormat"),password=server.arg("password");
  const String ipText=server.arg("staticIp"),maskText=server.arg("subnet"),gwText=server.arg("gateway"),dnsServerText=server.arg("dns");
  const bool useStatic=server.arg("staticIpEnabled")=="1",nextNtp=server.arg("ntpEnabled")=="1",nextAuth=server.arg("authEnabled")=="1";
  IPAddress ip,mask,gateway,dnsServer;
  if(!validHostname(host)){sendSettingsError("invalid_hostname");return;}
  if(useStatic&&(!ip.fromString(ipText)||!mask.fromString(maskText)||!gateway.fromString(gwText)||!dnsServer.fromString(dnsServerText))){sendSettingsError("invalid_ip_configuration");return;}
  if(!ntp.length()||ntp.length()>128||(format!="12"&&format!="24")||(zone!="Europe/Bratislava"&&zone!="Europe/Prague"&&zone!="Europe/Berlin"&&zone!="Europe/London"&&zone!="America/New_York"&&zone!="America/Los_Angeles"&&zone!="Asia/Tokyo"&&zone!="UTC")){sendSettingsError("invalid_time_settings");return;}
  const bool passwordChanged=password.length()&&password!=uiPassword;
  if(passwordChanged&&password.length()<8){sendSettingsError("password_too_short");return;}
  if(nextAuth&&!updatePasswordSet&&!password.length()){sendSettingsError("set_password_first");return;}
  const bool netChanged=host!=deviceHostname||useStatic!=staticIpEnabled||(useStatic&&(ipText!=staticIpText||maskText!=subnetText||gwText!=gatewayText||dnsServerText!=dnsText));
  const bool timeChanged=nextNtp!=ntpEnabled||ntp!=ntpServerName||zone!=timezoneName||format!=timeFormat;
  const bool authChanged=nextAuth!=authEnabled||passwordChanged;
  if(netChanged){
    deviceHostname=host;staticIpEnabled=useStatic;staticIpText=ipText;subnetText=maskText;gatewayText=gwText;dnsText=dnsServerText;
    prefs.putString("hostname",deviceHostname);prefs.putBool("staticIpEnabled",staticIpEnabled);prefs.putString("staticIp",staticIpText);prefs.putString("subnet",subnetText);prefs.putString("gateway",gatewayText);prefs.putString("dns",dnsText);
    addActivity("network_changed");
  }
  if(timeChanged){
    ntpEnabled=nextNtp;ntpServerName=ntp;timezoneName=zone;timeFormat=format;
    prefs.putBool("ntpEnabled",ntpEnabled);prefs.putString("ntpServer",ntpServerName);prefs.putString("timezone",timezoneName);prefs.putString("timeFormat",timeFormat);
    configureTime();addActivity("time_configured");
  }
  if(authChanged){
    if(passwordChanged){uiPassword=password;prefs.putString("uiPass",password);updatePasswordSet=true;}
    authEnabled=nextAuth;prefs.putBool("auth",authEnabled);
    addActivity("security_changed");
  }
  // Network and login changes (including the ArduinoOTA password) take effect after a restart.
  const bool restart=netChanged||authChanged;
  server.send(200,"application/json",restart?"{\"ok\":true,\"restarting\":true}":"{\"ok\":true,\"restarting\":false}");
  if(restart){delay(250);ESP.restart();}
}
static void handleLoginPage(){if(isAuthenticated()){redirectTo("/");return;}server.sendHeader("Cache-Control","no-store");server.send_P(200,"text/html; charset=utf-8",LOGIN_PAGE);}
static void handleLoginSubmit(){
  if(!authEnabled){server.send(200,"application/json","{\"ok\":true}");return;}
  if(loginBlocked()){server.send(429,"application/json","{\"error\":\"too_many_attempts\"}");return;}
  const bool userOk=constantTimeEquals(server.arg("username"),String("admin"));
  const bool passwordOk=constantTimeEquals(server.arg("password"),uiPassword);
  if(!userOk||!passwordOk){noteLoginFailure();server.send(401,"application/json","{\"error\":\"invalid_credentials\"}");return;}
  loginFailures=0;
  server.sendHeader("Set-Cookie","wf_session="+createSession()+"; Path=/; Max-Age=43200; HttpOnly; SameSite=Strict");
  server.send(200,"application/json","{\"ok\":true}");
}
static void handleLogout(){Session *s=findSession();if(s)s->token[0]=0;server.sendHeader("Set-Cookie","wf_session=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");redirectTo("/login");}
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

static bool otaUploadAllowed=false, otaUploadOk=false;
// Never sends a response: that is the job of handleUpdateDone() after the upload.
static void handleUpdateUpload(){
  HTTPUpload &u=server.upload();
  if(u.status==UPLOAD_FILE_START){
    otaUploadOk=false;otaUploadAllowed=isAuthenticated();
    if(!otaUploadAllowed)return;
    if(Update.isRunning())Update.abort();
    if(!Update.begin(UPDATE_SIZE_UNKNOWN))Update.printError(Serial);
    return;
  }
  if(!otaUploadAllowed)return;
  if(u.status==UPLOAD_FILE_WRITE){if(Update.isRunning()&&Update.write(u.buf,u.currentSize)!=u.currentSize)Update.printError(Serial);}
  else if(u.status==UPLOAD_FILE_END){if(Update.isRunning()&&Update.end(true)){otaUploadOk=true;Serial.println("OTA web update complete");}else Update.printError(Serial);}
  else if(u.status==UPLOAD_FILE_ABORTED){Update.abort();}
}
static void handleUpdateDone(){
  if(!authorized())return;
  const bool ok=otaUploadOk;otaUploadOk=false;
  if(!ok){server.send(500,"text/plain",Update.errorString());return;}
  server.send(200,"text/plain","Update complete; rebooting");delay(300);ESP.restart();
}
static void startPortal(){stationConnecting=false;stationWasConnected=false;apMode=true;WiFi.mode(WIFI_AP_STA);String suffix=macSuffix();suffix.toUpperCase();setupApSsid="Oblatkovac-Setup-"+suffix;WiFi.softAP(setupApSsid.c_str());dns.start(53,"*",WiFi.softAPIP());Serial.printf("[ESP] Setup AP '%s', address %s\n",setupApSsid.c_str(),WiFi.softAPIP().toString().c_str());}
static void handleReconfigureWifi(){if(!authorized())return;startPortal();String j="{\"ssid\":\""+jsonEscape(setupApSsid)+"\",\"ip\":\""+WiFi.softAPIP().toString()+"\"}";server.send(200,"application/json",j);}
static void handleTimeSync(){if(!authorized())return;if(!ntpEnabled){server.send(409,"application/json","{\"error\":\"ntp_disabled\"}");return;}configureTime();addActivity("time_synced");server.send(200,"application/json","{\"ok\":true}");}
static void handleRestart(){if(!authorized())return;addActivity("device_restart");server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void handleForgetWifi(){if(!authorized())return;prefs.remove("ssid");prefs.remove("wifiPass");server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void handleFactoryReset(){if(!authorized())return;prefs.clear();server.send(200,"application/json","{\"ok\":true,\"restarting\":true}");delay(250);ESP.restart();}
static void startNetwork(){String s=prefString("ssid","");String p=prefString("wifiPass","");WiFi.setHostname(deviceHostname.c_str());if(!s.length()){Serial.println(F("[ESP] No saved Wi-Fi; starting setup AP"));startPortal();return;}Serial.printf("[ESP] Connecting to Wi-Fi '%s'\n",s.c_str());apMode=false;stationConnecting=true;stationStartedMs=millis();WiFi.mode(WIFI_STA);if(staticIpEnabled){IPAddress ip,mask,gateway,dnsServer;if(ip.fromString(staticIpText)&&mask.fromString(subnetText)&&gateway.fromString(gatewayText)&&dnsServer.fromString(dnsText))WiFi.config(ip,gateway,mask,dnsServer);}WiFi.begin(s.c_str(),p.c_str());}
static void startArduinoOTAIfReady(){if(!apMode&&!arduinoOtaStarted&&WiFi.status()==WL_CONNECTED){if(authEnabled)ArduinoOTA.setPassword(uiPassword.c_str());ArduinoOTA.setHostname(deviceHostname.c_str());ArduinoOTA.begin();arduinoOtaStarted=true;Serial.println(F("[ESP] ArduinoOTA ready"));}}

void setup(){
  bootMs=millis();Serial.begin(115200);pinMode(8,OUTPUT);setWifiLed(false);lastWifiLedMs=millis();Serial.println(F("[ESP] Oblatkovac controller boot; USB log at 115200 baud"));Serial.printf("[ESP] Firmware v%s %s %s\n",FIRMWARE_VERSION,FIRMWARE_BUILD_DATE,FIRMWARE_BUILD_TIME);
  esp_register_freertos_idle_hook(idleHook);
  nanoLink.begin(kBaudRate,SERIAL_8N1,kEspRxPin,kEspTxPin);prefs.begin("waffle",false);activeLanguage=prefString("language","en");if(activeLanguage!="sk")activeLanguage="en";deviceHostname=prefString("hostname","");if(!deviceHostname.length()||deviceHostname=="oblatkovac"||deviceHostname==legacyHostname())deviceHostname=defaultHostname();staticIpEnabled=prefs.getBool("staticIpEnabled",false);staticIpText=prefString("staticIp","");subnetText=prefString("subnet","255.255.255.0");gatewayText=prefString("gateway","");dnsText=prefString("dns","");ntpEnabled=prefs.getBool("ntpEnabled",true);ntpServerName=prefString("ntpServer","pool.ntp.org");timezoneName=prefString("timezone","Europe/Bratislava");timeFormat=prefString("timeFormat","24");if(timeFormat!="12")timeFormat="24";configureTime();importTargetPending=!prefs.isKey("target");importDurationPending=!prefs.isKey("duration");targetC=prefs.getInt("target",WAFFLE_DEFAULT_TEMPERATURE_C);if(targetC<WAFFLE_MIN_TEMPERATURE_C||targetC>WAFFLE_MAX_TEMPERATURE_C)targetC=WAFFLE_DEFAULT_TEMPERATURE_C;durationS=prefs.getInt("duration",WAFFLE_DEFAULT_BAKE_TIME_S);if(durationS<WAFFLE_MIN_BAKE_TIME_S||durationS>WAFFLE_MAX_BAKE_TIME_S)durationS=WAFFLE_DEFAULT_BAKE_TIME_S;uiPassword=prefString("uiPass","");updatePasswordSet=uiPassword.length()>0;// Migration from the separate "uiAuth"/"otaAuth" switches: either one turns the common switch on.
  authEnabled=prefs.isKey("auth")?prefs.getBool("auth",false):(prefs.getBool("uiAuth",false)||prefs.getBool("otaAuth",true));authEnabled=authEnabled&&updatePasswordSet;startNetwork();configWaitStartedMs=millis();
  server.on("/",HTTP_GET,handleRoot);server.on("/settings",HTTP_GET,handleRoot);server.on("/wifi",HTTP_GET,handleWifi);server.on("/savewifi",HTTP_POST,handleSaveWifi);
  server.on("/favicon.ico",HTTP_ANY,[]{server.sendHeader("Cache-Control","no-store");server.send_P(200,"image/svg+xml",FAVICON);});server.on("/favicon.svg",HTTP_GET,[]{server.sendHeader("Cache-Control","no-store");server.send_P(200,"image/svg+xml",FAVICON);});server.on("/api/wifi-scan",HTTP_GET,handleWifiScan);
  server.on("/api/version",HTTP_GET,[]{String j="{\"version\":\"" FIRMWARE_VERSION "\",\"buildDate\":\"" FIRMWARE_BUILD_DATE "\",\"buildTime\":\"" FIRMWARE_BUILD_TIME "\",\"auth\":";j+=authEnabled?"true}":"false}";server.send(200,"application/json",j);});server.on("/api/status",HTTP_GET,sendStatus);server.on("/api/history",HTTP_GET,sendHistory);server.on("/api/events",HTTP_GET,sendEvents);server.on("/api/language",HTTP_ANY,handleLanguage);server.on("/api/translations",HTTP_GET,handleTranslations);
  server.on("/api/settings",HTTP_POST,handleSet);server.on("/api/save-settings",HTTP_POST,handleSaveSettings);server.on("/api/password",HTTP_GET,handlePassword);server.on("/api/time-sync",HTTP_POST,handleTimeSync);server.on("/api/control",HTTP_GET,[]{server.send(405,"application/json","{\"error\":\"method_not_allowed\",\"expected\":\"POST\"}");});server.on("/api/control",HTTP_POST,handleControl);server.on("/login",HTTP_GET,handleLoginPage);server.on("/login",HTTP_POST,handleLoginSubmit);server.on("/logout",HTTP_GET,handleLogout);server.on("/api/reconfigure-wifi",HTTP_POST,handleReconfigureWifi);server.on("/api/restart",HTTP_POST,handleRestart);server.on("/api/wifi/forget",HTTP_POST,handleForgetWifi);server.on("/api/factory-reset",HTTP_POST,handleFactoryReset);server.on("/metrics",HTTP_GET,handleMetrics);
  server.on("/update",HTTP_GET,[]{redirectTo("/settings");});
  server.on("/update",HTTP_POST,handleUpdateDone,handleUpdateUpload);
  server.onNotFound([](){if(server.uri().startsWith("/api/")){server.send(404,"application/json","{\"error\":\"not_found\"}");return;}handleWifi();});
  static const char *kRequestHeaders[]={"Cookie"};server.collectHeaders(kRequestHeaders,1);
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
