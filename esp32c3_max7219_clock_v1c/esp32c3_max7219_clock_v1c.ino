// ===============================================
// ESP32C3 MAX7219 NTP Clock - Enhanced Version from https://github.com/stechiez/esp32-c3-max7217-ntp
// Features: WiFi recovery, EEPROM config, auto-geolocation, power optimization
// ===============================================
// v.1 - Nicu Florica (niq_ro) used AI to added info from open-meteo.com (temperature and humidity)
// ===============================================

#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <EEPROM.h>
#include <time.h>
#include <MD_Parola.h>  
#include <SPI.h>
#include <ArduinoJson.h>

#include "Font_Data.h"
#include "WiFiConfig.h"

// =============== HARDWARE CONFIGURATION ===============
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4

#define CLK_PIN   4
#define DATA_PIN  6
#define CS_PIN    7

MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// =============== DISPLAY CONFIGURATION ===============
#define SPEED_TIME       75   
#define WEATHER_SPEED    45   
#define PAUSE_TIME       0
#define MAX_MESG         40   

// =============== TIME CONFIGURATION ===============
int32_t TIMEZONE_SECONDS = 0;
const int DST = 0;
const char* NTP_SERVERS[] = {"pool.ntp.org", "time.nist.gov"};
const char* GEOLOCATION_API = "http://ip-api.com/json/?fields=country,city,lat,lon,timezone,offset";

// =============== EEPROM CONFIGURATION ===============
#define EEPROM_SIZE 512
#define SSID_ADDR 0
#define PASS_ADDR 33
#define INIT_FLAG_ADDR 66
#define TIMEZONE_ADDR 70
#define LOCATION_ADDR 80

// =============== WIFI CONFIGURATION ===============
#define WIFI_RETRY_DELAY 500
#define WIFI_MAX_RETRIES 20
#define WIFI_RECONNECT_INTERVAL 30000
#define WIFI_RECONNECT_TIMEOUT 10000

// =============== GLOBAL VARIABLES ===============
uint16_t h, m, s;
uint8_t dow, day, month;
String yearStr;

char szTime[9];      
char szsecond[4];    
char szScroll[MAX_MESG]; 

uint32_t lastWiFiCheck = 0;
bool wifiConnected = false;

float currentTemp = 0.0;
int currentHum = 0;

struct {
  char country[32];
  char city[32];
  float latitude;
  float longitude;
  char timezone[40];
  int32_t utcOffset;
} location;

struct {
  char ssid[32];
  char password[65];
} wifiConfig;

unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_FETCH_INTERVAL = 600000; 

// --- SECVENTA CU STERGERE (WIPE) ---
enum DisplayMode { 
  MODE_CLOCK, 
  MODE_CLEAR, 
  MODE_TEMP_IN,      
  MODE_TEMP_WIPE,    // Spatii goale care sterg temperatura
  MODE_HUM_IN,       
  MODE_HUM_WIPE      // Spatii goale care sterg umiditatea
};
DisplayMode currentMode = MODE_CLOCK;
unsigned long lastModeSwitch = 0;
const unsigned long CLOCK_DISPLAY_TIME = 25000; 


// =============== EEPROM MANAGEMENT ===============
void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t initFlag = EEPROM.read(INIT_FLAG_ADDR);
  if (initFlag == 0xAA) {
    EEPROM.readString(SSID_ADDR, wifiConfig.ssid, sizeof(wifiConfig.ssid));
    EEPROM.readString(PASS_ADDR, wifiConfig.password, sizeof(wifiConfig.password));
    if (strlen(wifiConfig.ssid) == 0) {
      strcpy(wifiConfig.ssid, DEFAULT_SSID);
      strcpy(wifiConfig.password, DEFAULT_PASSWORD);
      saveWiFiConfig();
    }
  } else {
    strcpy(wifiConfig.ssid, DEFAULT_SSID);
    strcpy(wifiConfig.password, DEFAULT_PASSWORD);
    saveWiFiConfig();
  }
}

void saveWiFiConfig() {
  EEPROM.writeString(SSID_ADDR, wifiConfig.ssid);
  EEPROM.writeString(PASS_ADDR, wifiConfig.password);
  EEPROM.write(INIT_FLAG_ADDR, 0xAA);
  EEPROM.commit();
}

void debugPrint(const char* msg) { Serial.println(msg); }

// =============== FUNCTII PENTRU VREME ===============
void fetchWeatherData() {
  if (location.latitude == 0.0 && location.longitude == 0.0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(location.latitude, 4) + 
               "&longitude=" + String(location.longitude, 4) + 
               "&current=temperature_2m,relative_humidity_2m";
  
  http.begin(url);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      currentTemp = doc["current"]["temperature_2m"].as<float>();
      currentHum = doc["current"]["relative_humidity_2m"].as<int>();
      Serial.printf("Vreme actualizata: %.1fC %d%%\n", currentTemp, currentHum);
    }
  }
  http.end();
}

// =============== GEOLOCATION & TIMEZONE ===============
bool fetchGeolocation() {
  debugPrint("Fetching geolocation and timezone...");
  HTTPClient http;
  http.begin(GEOLOCATION_API);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) { http.end(); return false; }
  
  String payload = http.getString();
  http.end();
  
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) return false;
  
  strlcpy(location.country, doc["country"] | "Unknown", sizeof(location.country));
  strlcpy(location.city, doc["city"] | "Unknown", sizeof(location.city));
  location.latitude = doc["lat"] | 0.0f;
  location.longitude = doc["lon"] | 0.0f;
  strlcpy(location.timezone, doc["timezone"] | "UTC", sizeof(location.timezone));
  location.utcOffset = doc["offset"] | 0;

  if (abs(location.utcOffset) < 1000) location.utcOffset *= 3600;
  TIMEZONE_SECONDS = location.utcOffset;
  printLocationInfo();
  return true;
}

void printLocationInfo() {
  debugPrint("=== Geolocation Information ===");
  Serial.print("Location: "); Serial.print(location.city); Serial.print(", "); Serial.println(location.country);
  Serial.print("Coords: "); Serial.print(location.latitude, 4); Serial.print(", "); Serial.println(location.longitude, 4);
  Serial.print("Timezone: UTC"); Serial.println(location.utcOffset / 3600.0, 2);
}

void saveLocationData() {
  int32_t storeOffset = location.utcOffset;
  if (abs(storeOffset) < 1000) storeOffset *= 3600;
  EEPROM.put(TIMEZONE_ADDR, storeOffset);
  EEPROM.commit();
}

void loadLocationData() {
  EEPROM.get(TIMEZONE_ADDR, location.utcOffset);
  if (abs(location.utcOffset) < 1000 && location.utcOffset != 0) location.utcOffset *= 3600;
  TIMEZONE_SECONDS = location.utcOffset;
}

// =============== DISPLAY FUNCTIONS ===============
void formatSeconds(char* buffer) { sprintf(buffer, "%02d", s); }
void formatTime(char* buffer, bool showColon = true) { sprintf(buffer, "%02d%c%02d", h, (showColon ? ':' : ' '), m); }

// =============== TIME SYNCHRONIZATION ===============
void syncTimeFromNTP() {
  debugPrint("Synchronizing time from NTP...");
  configTime(TIMEZONE_SECONDS, DST, NTP_SERVERS[0], NTP_SERVERS[1]);
  uint32_t startTime = millis();
  time_t now = time(nullptr);
  while (now < 24 * 3600 && (millis() - startTime) < 10000) { delay(100); now = time(nullptr); }
  if (now > 24 * 3600) debugPrint("Time synchronized successfully");
  else debugPrint("WARNING: Time sync timed out");
}

void updateTime() {
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);
  h = p_tm->tm_hour; m = p_tm->tm_min; s = p_tm->tm_sec;
  dow = p_tm->tm_wday; day = p_tm->tm_mday; month = p_tm->tm_mon + 1;
  yearStr = String(p_tm->tm_year + 1900);
}

// =============== WIFI MANAGEMENT ===============
bool initializeWiFi() {
  debugPrint("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  uint32_t startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > 10000) return false;
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  wifiConnected = true;
  return true;
}

void checkAndRestoreWiFi() {
  if (millis() - lastWiFiCheck < WIFI_RECONNECT_INTERVAL) return;
  lastWiFiCheck = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) { wifiConnected = true; debugPrint("WiFi reconnected!"); }
  } else {
    if (wifiConnected) { wifiConnected = false; debugPrint("WiFi disconnected!"); }
    WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  }
}

// =============== INITIALIZATION ===============
void setup(void) {
  Serial.begin(115200);
  delay(10);
  debugPrint("\n\n=== ESP32C3 MAX7219 NTP Clock with Temp/Hum ===");
  
  initEEPROM();
  loadLocationData();
  
  if (!initializeWiFi()) {
    WiFi.disconnect(true); delay(500);
    strcpy(wifiConfig.ssid, DEFAULT_SSID);
    strcpy(wifiConfig.password, DEFAULT_PASSWORD);
    if (!initializeWiFi()) debugPrint("ERROR: WiFi failed completely.");
  }

  if (wifiConnected) {
    if (fetchGeolocation()) saveLocationData();
    syncTimeFromNTP();
    fetchWeatherData();
    lastWeatherFetch = millis();
  } else {
    configTime(TIMEZONE_SECONDS, DST, NTP_SERVERS[0], NTP_SERVERS[1]);
  }
  
  updateTime();
  
  SPI.begin(CLK_PIN, -1, DATA_PIN, CS_PIN);
  P.begin(2); 
  P.setInvert(false);
  currentMode = MODE_CLOCK;
  lastModeSwitch = millis();
  
  setupClockMode();
  P.setIntensity(0); 
  
  debugPrint("Setup complete!");
}

void setupClockMode() {
  P.setZone(0, 0, 0);      
  P.setZone(1, 1, 3);      
  P.setFont(0, numeric7Seg);
  P.setFont(1, numeric7Se); 
  
  formatTime(szTime);
  formatSeconds(szsecond);
  P.displayZoneText(0, szsecond, PA_LEFT, SPEED_TIME, 0, PA_PRINT, PA_NO_EFFECT);
  P.displayZoneText(1, szTime, PA_CENTER, SPEED_TIME, PAUSE_TIME, PA_PRINT, PA_NO_EFFECT);
}

void setupClearMode() {
  P.setZone(0, 0, MAX_DEVICES - 1); 
  P.setFont(0, numeric7Se);          
  P.displayZoneText(0, "        ", PA_CENTER, 40, 0, PA_DISSOLVE, PA_NO_EFFECT);
}

// --- SECVENTE NOI ---

// 1. Temperatura intra
void setupTempIn() {
  sprintf(szScroll, " %.1f%cC", currentTemp, (char)176);
  P.displayZoneText(0, szScroll, PA_LEFT, WEATHER_SPEED, 0, PA_SCROLL_LEFT, PA_NO_EFFECT);
}

// 2. Wipe: Semicne goale care curg peste ecran si il sterg complet
void setupWipe() {
  // 8 spatii acopera exact latimea ecranului cu fontul mare
  P.displayZoneText(0, "        ", PA_LEFT, WEATHER_SPEED, 0, PA_SCROLL_LEFT, PA_NO_EFFECT);
}

// 3. Umiditatea intra
void setupHumIn() {
  sprintf(szScroll, "  %d%%", currentHum);
  P.displayZoneText(0, szScroll, PA_LEFT, WEATHER_SPEED, 0, PA_SCROLL_LEFT, PA_NO_EFFECT);
}

// =============== MAIN LOOP ===============
void loop(void) {
  static bool flasher = false;
  
  P.displayAnimate();
  checkAndRestoreWiFi();
  
  if (millis() - lastWeatherFetch >= WEATHER_FETCH_INTERVAL) {
    lastWeatherFetch = millis();
    if (wifiConnected) fetchWeatherData();
  }

  switch (currentMode) {
    case MODE_CLOCK:
      {
        static uint32_t lastUpdate = 0;
        if (millis() - lastUpdate >= 1000) {
          lastUpdate = millis();
          updateTime();
          formatTime(szTime, flasher);
          formatSeconds(szsecond);
          flasher = !flasher;
          
          P.displayZoneText(0, szsecond, PA_LEFT, SPEED_TIME, 0, PA_PRINT, PA_NO_EFFECT);
          P.displayZoneText(1, szTime, PA_CENTER, SPEED_TIME, PAUSE_TIME, PA_PRINT, PA_NO_EFFECT);
        }
        
        if (millis() - lastModeSwitch >= CLOCK_DISPLAY_TIME) {
          currentMode = MODE_CLEAR;
          setupClearMode();
        }
      }
      break;

    case MODE_CLEAR:
      if (P.getZoneStatus(0)) {
        currentMode = MODE_TEMP_IN;
        setupTempIn();
      }
      break;

    case MODE_TEMP_IN:
      if (P.getZoneStatus(0)) { 
        delay(2500); // Pauza fixa, simpla. Ecranul ingheata.
        currentMode = MODE_TEMP_WIPE;
        setupWipe(); // Porneste semnele goale
      }
      break;

    case MODE_TEMP_WIPE:
      if (P.getZoneStatus(0)) { // Dupa ce semnele goale au trecut, ecranul e curat!
        currentMode = MODE_HUM_IN;
        setupHumIn(); // Intra umiditatea fara niciun dublu
      }
      break;

    case MODE_HUM_IN:
      if (P.getZoneStatus(0)) { 
        delay(2500); // Pauza fixa
        currentMode = MODE_HUM_WIPE;
        setupWipe(); // Porneste semnele goale
      }
      break;

    case MODE_HUM_WIPE:
      if (P.getZoneStatus(0)) { 
        currentMode = MODE_CLOCK;
        lastModeSwitch = millis();
        setupClockMode(); // Revenire la ceas
      }
      break;
  }
}

void getTimentp() __attribute__((deprecated("Use syncTimeFromNTP() instead")));
void getTimentp() { syncTimeFromNTP(); }
