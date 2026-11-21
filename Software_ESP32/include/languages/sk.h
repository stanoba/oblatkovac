#pragma once

// Slovak translation. Add future languages as another header with these keys.
static const char UI_LANGUAGE_SK[] PROGMEM = R"JSON({
  "language":"Jazyk","dashboard":"Prehľad","settings":"Nastavenia","wifi":"Wi-Fi","ota":"OTA",
  "temperature":"Teplota","setTemperature":"Nastavená teplota","heater":"Špirála","lid":"Veko","timer":"Čas","wafers":"Oblátky",
  "chartTitle":"Teplota a priebeh pečenia","range15":"15 min","range60":"60 min","range180":"180 min",
  "chartLegend":"Modrá: skutočná teplota · tyrkysová: nastavená teplota · zelená: špirála · značky: otvorenie veka",
  "averageBake":"Priemerné pečenie","averageLid":"Priemer otvoreného veka","perHour":"Odhad oblátok za hodinu","openings":"Otvorenia veka",
  "controls":"Ovládanie a nastavenia","bakeTemperature":"Teplota °C","bakeTime":"Čas s","save":"Uložiť","start":"Štart","stop":"Stop",
  "encoderTemperature":"Enkóder: teplota","encoderTime":"Enkóder: čas","authRequired":"Vyžadovať heslo pre UI","passwordPlaceholder":"Heslo UI/OTA","saveSecurity":"Uložiť zabezpečenie",
  "ovenOpen":"Otvorené","ovenClosed":"Zatvorené","nanoOnline":"● Nano pripojené","nanoOffline":"● Nano odpojené","lidOpenedFor":"veko otvorené","noOpenings":"Zatiaľ bez otvorení","connectionError":"Chyba spojenia s ESP32","settingsSaved":"Nastavenia uložené",
  "commandAccepted":"Príkaz prijatý","securitySaved":"Zabezpečenie uložené","scan":"Vyhľadať dostupné Wi-Fi","scanning":"Vyhľadávam siete v okolí…",
  "scanFailed":"Skenovanie Wi-Fi zlyhalo. Skús to znova.","noNetworks":"Nenašli sa žiadne siete.","networksFound":"Nájdených sietí:","setupTitle":"Nastavenie zariadenia",
  "firstConnection":"Prvé pripojenie","connectWifi":"Pripojme ho k tvojej Wi-Fi.","wifiIntro":"Zadaj názov a heslo domácej siete. Po uložení sa ESP32 reštartuje a dashboard bude dostupný v tvojej lokálnej sieti.",
  "currentAp":"Práve pripojený bod","wifiSettings":"Pripojenie k sieti","savedLocally":"Údaje sa uložia lokálne do ESP32.","ssid":"Názov Wi-Fi siete (SSID)",
  "ssidPlaceholder":"napr. DomacaWiFi","password":"Heslo Wi-Fi","show":"Zobraziť","hide":"Skryť","saveConnect":"Uložiť a pripojiť","reconnectHint":"Po reštarte sa pripoj späť k svojej Wi-Fi. Ak pripojenie zlyhá, zariadenie znovu spustí tento nastavovací bod.",
  "stepChoose":"Vyber sieť","stepPassword":"Zadaj heslo","stepSave":"Ulož","scanError":"Chyba pri skenovaní Wi-Fi.","savingWifi":"Uložené, reštartujem","wifiSetupFooter":"Oblátkovač · nastavenie ESP32-C3"
})JSON";
