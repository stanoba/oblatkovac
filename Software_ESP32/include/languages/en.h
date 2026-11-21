#pragma once

// English is the default UI language. Keys are shared with the browser UI.
static const char UI_LANGUAGE_EN[] PROGMEM = R"JSON({
  "language":"Language","dashboard":"Dashboard","settings":"Settings","wifi":"Wi-Fi","ota":"OTA",
  "temperature":"Temperature","setTemperature":"Set temperature","heater":"Heater","lid":"Lid","timer":"Timer","wafers":"Wafers",
  "chartTitle":"Temperature and baking history","range15":"15 min","range60":"60 min","range180":"180 min",
  "chartLegend":"Blue: actual temperature · teal: set temperature · green: heater · markers: lid opened",
  "averageBake":"Average bake time","averageLid":"Average lid-open time","perHour":"Estimated wafers per hour","openings":"Lid openings",
  "controls":"Controls and settings","bakeTemperature":"Temperature °C","bakeTime":"Time s","save":"Save","start":"Start","stop":"Stop",
  "encoderTemperature":"Encoder: temperature","encoderTime":"Encoder: time","authRequired":"Require password for UI","passwordPlaceholder":"UI/OTA password","saveSecurity":"Save security settings",
  "ovenOpen":"Open","ovenClosed":"Closed","nanoOnline":"● Nano online","nanoOffline":"● Nano offline","lidOpenedFor":"lid open for","noOpenings":"No lid openings yet","connectionError":"Connection to ESP32 failed","settingsSaved":"Settings saved",
  "commandAccepted":"Command accepted","securitySaved":"Security settings saved","scan":"Scan for Wi-Fi networks","scanning":"Scanning nearby networks…",
  "scanFailed":"Wi-Fi scan failed. Please try again.","noNetworks":"No networks found.","networksFound":"Networks found:","setupTitle":"Device setup",
  "firstConnection":"First connection","connectWifi":"Connect to your Wi-Fi.","wifiIntro":"Enter your home network name and password. After saving, the ESP32 will restart and the dashboard will be available on your local network.",
  "currentAp":"Current access point","wifiSettings":"Wi-Fi connection","savedLocally":"Your credentials are stored on the ESP32.","ssid":"Wi-Fi network name (SSID)",
  "ssidPlaceholder":"e.g. HomeWiFi","password":"Wi-Fi password","show":"Show","hide":"Hide","saveConnect":"Save and connect","reconnectHint":"After restart, reconnect to your Wi-Fi. If the connection fails, the device will start this setup access point again.",
  "stepChoose":"Choose a network","stepPassword":"Enter password","stepSave":"Save","scanError":"Wi-Fi scan failed.","savingWifi":"Saved. Restarting…","wifiSetupFooter":"Oblátkovač · ESP32-C3 setup"
})JSON";
