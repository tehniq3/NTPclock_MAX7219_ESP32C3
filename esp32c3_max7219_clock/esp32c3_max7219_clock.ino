// ===============================================
// ESP32C3 MAX7219 NTP Clock - Enhanced Version
// Features: WiFi recovery, EEPROM config, auto-geolocation, power optimization
// https://github.com/stechiez/esp32-c3-max7217-ntp
// https://www.youtube.com/watch?v=Py2IAD0TNvI
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

// Display SPI pins (ESP32-C3 Super Mini pinout)
// On the Super Mini breakout the exposed SPI pins are:
//   SCK  -> GPIO4
//   MOSI -> GPIO6
//   CS   -> GPIO7
// (MISO is not used by MAX7219)
#define CLK_PIN   4   // SCK
#define DATA_PIN  6   // MOSI
#define CS_PIN    7   // SS / CS

MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// =============== DISPLAY CONFIGURATION ===============
#define SPEED_TIME  75
#define PAUSE_TIME  0
#define MAX_MESG    20

// =============== TIME CONFIGURATION ===============
// Timezone in seconds: Auto-detected from geolocation during startup
// Fallback: 0 = UTC (will be overridden by geolocation)
int32_t TIMEZONE_SECONDS = 0;
const int DST = 0;

// NTP servers for time synchronization
const char* NTP_SERVERS[] = {"pool.ntp.org", "time.nist.gov"};

// Geolocation API endpoint
const char* GEOLOCATION_API = "http://ip-api.com/json/?fields=country,city,lat,lon,timezone,offset";

// =============== EEPROM CONFIGURATION ===============
#define EEPROM_SIZE 512
#define SSID_ADDR 0
#define PASS_ADDR 33
#define INIT_FLAG_ADDR 66
#define TIMEZONE_ADDR 70
#define LOCATION_ADDR 80

// =============== WIFI CONFIGURATION ===============
#define WIFI_RETRY_DELAY 500      // ms between connection attempts
#define WIFI_MAX_RETRIES 20       // max attempts during setup
#define WIFI_RECONNECT_INTERVAL 30000  // check connection every 30s
#define WIFI_RECONNECT_TIMEOUT 10000   // timeout for reconnection attempt

// =============== GLOBAL VARIABLES ===============
// Time variables
uint16_t h, m, s;
uint8_t dow, day, month;
String year;

// Display buffers
char szTime[9];      // HH:MM\0
char szsecond[4];    // SS\0

// WiFi state tracking
uint32_t lastWiFiCheck = 0;
bool wifiConnected = false;

// Geolocation data
struct {
  char country[32];
  char city[32];
  float latitude;
  float longitude;
  char timezone[40];
  int32_t utcOffset;  // UTC offset in seconds
} location;

// Configuration storage
struct {
  char ssid[32];
  char password[65];
} wifiConfig;


// =============== EEPROM MANAGEMENT ===============
/**
 * Initialize EEPROM and load WiFi credentials if available
 */
void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  
  uint8_t initFlag = EEPROM.read(INIT_FLAG_ADDR);
  if (initFlag == 0xAA) {
    // Load previously saved credentials
    EEPROM.readString(SSID_ADDR, wifiConfig.ssid, sizeof(wifiConfig.ssid));
    EEPROM.readString(PASS_ADDR, wifiConfig.password, sizeof(wifiConfig.password));
    
    // Validate loaded credentials - if SSID is empty, reset to defaults
    if (strlen(wifiConfig.ssid) == 0) {
      debugPrint("EEPROM credentials corrupted! Resetting to defaults...");
      strcpy(wifiConfig.ssid, DEFAULT_SSID);
      strcpy(wifiConfig.password, DEFAULT_PASSWORD);
      saveWiFiConfig();
    } else {
      debugPrint("Loaded WiFi credentials from EEPROM");
    }
  } else {
    // First run - use default credentials from WiFiConfig.h
    strcpy(wifiConfig.ssid, DEFAULT_SSID);
    strcpy(wifiConfig.password, DEFAULT_PASSWORD);
    saveWiFiConfig();
    debugPrint("Using default WiFi credentials");
  }
}

/**
 * Save WiFi credentials to EEPROM for persistence
 */
void saveWiFiConfig() {
  EEPROM.writeString(SSID_ADDR, wifiConfig.ssid);
  EEPROM.writeString(PASS_ADDR, wifiConfig.password);
  EEPROM.write(INIT_FLAG_ADDR, 0xAA);
  EEPROM.commit();
}

// =============== DEBUG UTILITIES ===============
/**
 * Wrapper for Serial print with minimal overhead
 */
void debugPrint(const char* msg) {
  Serial.println(msg);
}

// =============== GEOLOCATION & TIMEZONE ===============
/**
 * Fetch geolocation and timezone information from IP-API
 * Updates global location struct and TIMEZONE_SECONDS
 */
bool fetchGeolocation() {
  debugPrint("Fetching geolocation and timezone...");
  
  HTTPClient http;
  http.begin(GEOLOCATION_API);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  
  int httpCode = http.GET();
  
  if (httpCode != HTTP_CODE_OK) {
    debugPrint("Geolocation API request failed");
    http.end();
    return false;
  }
  
  String payload = http.getString();
  http.end();
  
  Serial.print("Geolocation response: ");
  Serial.println(payload);
  
  // Parse JSON response
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    debugPrint("JSON parsing failed");
    return false;
  }
  
  // Extract location data
  strlcpy(location.country, doc["country"] | "Unknown", sizeof(location.country));
  strlcpy(location.city, doc["city"] | "Unknown", sizeof(location.city));
  location.latitude = doc["lat"] | 0.0f;
  location.longitude = doc["lon"] | 0.0f;
  strlcpy(location.timezone, doc["timezone"] | "UTC", sizeof(location.timezone));
  location.utcOffset = doc["offset"] | 0;

  // Debug: show raw offset value returned by API
  Serial.print("Raw timezone offset from API: ");
  Serial.println(location.utcOffset);

  // Normalize offset: if the API returned a small number (likely hours),
  // convert to seconds expected by configTime(). If it's already in
  // seconds (large magnitude), leave as-is.
  if (abs(location.utcOffset) < 1000) {
    Serial.print("Interpreting offset as hours, converting to seconds: ");
    Serial.println(location.utcOffset);
    location.utcOffset = location.utcOffset * 3600;
    Serial.print("Normalized timezone offset (seconds): ");
    Serial.println(location.utcOffset);
  }

  // Update timezone offset used by NTP config
  TIMEZONE_SECONDS = location.utcOffset;
  
  // Log location info (use helper for consistent formatting)
  printLocationInfo();
  
  return true;
}

/**
 * Print formatted location and timezone information to Serial
 */
void printLocationInfo() {
  debugPrint("=== Geolocation Information ===");
  // Location: City, Country (lat, lon)
  Serial.print("Location: ");
  Serial.print(location.city[0] ? location.city : "Unknown");
  Serial.print(", ");
  Serial.print(location.country[0] ? location.country : "Unknown");
  Serial.print(" (");
  Serial.print(location.latitude, 4);
  Serial.print(", ");
  Serial.print(location.longitude, 4);
  Serial.print(")\t");

  // Timezone: Name (UTC±hours)
  float hours = location.utcOffset / 3600.0f;
  Serial.print("Timezone: ");
  Serial.print(location.timezone[0] ? location.timezone : "UTC");
  Serial.print(" (UTC");
  Serial.print(hours, 2);
  Serial.println(")");
}

/**
 * Save location data to EEPROM
 */
void saveLocationData() {
  // Note: Full location struct is large for EEPROM
  // Save critical data (timezone offset) only
  // Ensure we store seconds (not hours). If utcOffset looks like hours, convert.
  int32_t storeOffset = location.utcOffset;
  if (abs(storeOffset) < 1000) {
    Serial.print("Converting location.utcOffset from hours to seconds before saving: ");
    Serial.println(storeOffset);
    storeOffset = storeOffset * 3600;
    Serial.print("Stored timezone offset (seconds): ");
    Serial.println(storeOffset);
  }
  EEPROM.put(TIMEZONE_ADDR, storeOffset);
  EEPROM.commit();
}

/**
 * Load location data from EEPROM
 */
void loadLocationData() {
  EEPROM.get(TIMEZONE_ADDR, location.utcOffset);
  Serial.print("Raw timezone offset read from EEPROM: ");
  Serial.println(location.utcOffset);

  // If value looks like hours (small magnitude), convert to seconds
  if (abs(location.utcOffset) < 1000 && location.utcOffset != 0) {
    Serial.print("Interpreting EEPROM offset as hours, converting to seconds: ");
    Serial.println(location.utcOffset);
    location.utcOffset = location.utcOffset * 3600;
    Serial.print("Normalized EEPROM timezone offset (seconds): ");
    Serial.println(location.utcOffset);
  }

  TIMEZONE_SECONDS = location.utcOffset;
  if (location.utcOffset != 0) {
    debugPrint("Loaded timezone offset from EEPROM");
  }
}

// =============== DISPLAY FUNCTIONS ===============
/**
 * Format seconds into "SS" string
 */
void formatSeconds(char* buffer) {
  sprintf(buffer, "%02d", s);
}

/**
 * Format time into "HH:MM" or "HH MM" string (with flasher effect)
 */
void formatTime(char* buffer, bool showColon = true) {
  sprintf(buffer, "%02d%c%02d", h, (showColon ? ':' : ' '), m);
}

// =============== TIME SYNCHRONIZATION ===============
/**
 * Synchronize time from NTP servers
 * Uses configTime() which handles timezone offset
 */
void syncTimeFromNTP() {
  debugPrint("Synchronizing time from NTP...");
  Serial.print("Using timezone offset (seconds): ");
  Serial.println(TIMEZONE_SECONDS);
  
  configTime(TIMEZONE_SECONDS, DST, NTP_SERVERS[0], NTP_SERVERS[1]);
  
  // Wait for time to be set (max 10 seconds)
  uint32_t startTime = millis();
  time_t now = time(nullptr);
  
  while (now < 24 * 3600 && (millis() - startTime) < 10000) {
    delay(100);
    now = time(nullptr);
  }
  
  if (now > 24 * 3600) {
    debugPrint("Time synchronized successfully");
    // Print the synchronized time
    time_t syncTime = time(nullptr);
    struct tm* p_tm = localtime(&syncTime);
    Serial.print("Fetched UTC time: ");
    Serial.print(p_tm->tm_year + 1900);
    Serial.print("-");
    if (p_tm->tm_mon + 1 < 10) Serial.print("0");
    Serial.print(p_tm->tm_mon + 1);
    Serial.print("-");
    if (p_tm->tm_mday < 10) Serial.print("0");
    Serial.print(p_tm->tm_mday);
    Serial.print(" ");
    if (p_tm->tm_hour < 10) Serial.print("0");
    Serial.print(p_tm->tm_hour);
    Serial.print(":");
    if (p_tm->tm_min < 10) Serial.print("0");
    Serial.print(p_tm->tm_min);
    Serial.print(":");
    if (p_tm->tm_sec < 10) Serial.print("0");
    Serial.println(p_tm->tm_sec);
  } else {
    debugPrint("WARNING: Time sync timed out");
  }
}

/**
 * Update global time variables from system time
 */
void updateTime() {
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);
  
  h = p_tm->tm_hour;
  m = p_tm->tm_min;
  s = p_tm->tm_sec;
}

// =============== WIFI MANAGEMENT ===============
/**
 * Initialize WiFi connection with timeout
 * Returns true if connected successfully
 */
bool initializeWiFi() {
  debugPrint("Connecting to WiFi...");
  Serial.print("SSID: ");
  Serial.println(wifiConfig.ssid);
  Serial.print("Password: ");
  Serial.println(wifiConfig.password);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  
  uint32_t startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > 10000) {  // 10 second timeout
      debugPrint("WiFi connection timeout!");
      Serial.print("Final WiFi status: ");
      Serial.println(WiFi.status());
      return false;
    }
    delay(500);
    Serial.print(".");
  }
  
  Serial.println();
  Serial.println("WiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  wifiConnected = true;
  return true;
}

/**
 * Check WiFi connection and attempt reconnection if needed
 * Call this periodically in the main loop
 */
void checkAndRestoreWiFi() {
  if (millis() - lastWiFiCheck < WIFI_RECONNECT_INTERVAL) {
    return;  // Not time to check yet
  }
  
  lastWiFiCheck = millis();
  
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      debugPrint("WiFi reconnected!");
    }
  } else {
    if (wifiConnected) {
      debugPrint("WiFi disconnected! Attempting to reconnect...");
      wifiConnected = false;
    }
    // Try to reconnect
    WiFi.begin(wifiConfig.ssid, wifiConfig.password);
  }
}

// =============== INITIALIZATION ===============
void setup(void) {
  Serial.begin(115200);
  delay(10);
  
  debugPrint("\n\n=== ESP32C3 MAX7219 NTP Clock with Auto-Geolocation ===");
  
  // Initialize EEPROM and load configuration
  initEEPROM();
  loadLocationData();  // Load previously detected timezone if available
  
  // Initialize WiFi
  if (!initializeWiFi()) {
    debugPrint("ERROR: Could not connect to WiFi");
    debugPrint("Retrying with default credentials from WiFiConfig.h...");
    
    // Disconnect and reset WiFi before retry
    WiFi.disconnect(true);  // true = turn off radio
    delay(500);
    
    strcpy(wifiConfig.ssid, DEFAULT_SSID);
    strcpy(wifiConfig.password, DEFAULT_PASSWORD);
    
    if (!initializeWiFi()) {
      debugPrint("ERROR: Could not connect with default credentials either");
      debugPrint("Using UTC as fallback timezone");
    }
    // Continue anyway - display will show even if WiFi fails
  }

  // Detect geolocation and timezone automatically whenever WiFi is connected,
  // including after fallback credentials succeed.
  if (wifiConnected) {
    if (fetchGeolocation()) {
      saveLocationData();
      Serial.print("Auto-detected timezone: UTC");
      if (location.utcOffset >= 0) Serial.print("+");
      Serial.print(location.utcOffset / 3600.0, 2);
      Serial.println("");
    } else {
      debugPrint("Geolocation failed, using fallback timezone");
    }
  }
  
  // Synchronize time from NTP using detected timezone
  if (wifiConnected) {
    syncTimeFromNTP();
  } else {
    Serial.print("WiFi not connected. Using timezone offset: ");
    Serial.print(TIMEZONE_SECONDS);
    Serial.println(" seconds");
    configTime(TIMEZONE_SECONDS, DST, NTP_SERVERS[0], NTP_SERVERS[1]);
  }
  
  // Show current time after sync attempt
  updateTime();
  Serial.print("Display time will show: ");
  if (h < 10) Serial.print("0");
  Serial.print(h);
  Serial.print(":");
  if (m < 10) Serial.print("0");
  Serial.println(m);
  
  // Initialize display
  // Ensure SPI is initialized on the correct pins for this board
  SPI.begin(CLK_PIN, -1, DATA_PIN, CS_PIN); // SCK, MISO(not used), MOSI, SS
  P.begin(4);
  P.setInvert(false);
  
  // Setup display zones (0=seconds, 1=time)
  P.setZone(0, 0, 0);      // Zone 0: first LED module
  P.setZone(1, 1, 3);      // Zone 1: remaining LED modules
  P.setFont(0, numeric7Seg);
  P.setFont(1, numeric7Se);
  
  // Initialize display with fixed print effect
  P.displayZoneText(0, szsecond, PA_LEFT, SPEED_TIME, 0, PA_PRINT, PA_NO_EFFECT);
  P.displayZoneText(1, szTime, PA_CENTER, SPEED_TIME, PAUSE_TIME, PA_PRINT, PA_NO_EFFECT);
  P.setIntensity(0);  // 0..15 brightness control
  
  // Show initial time
  updateTime();
  formatTime(szTime);
  Serial.print("Display time (HH:MM): ");
  Serial.println(szTime);
  
  debugPrint("Setup complete!");
}

// =============== MAIN LOOP ===============
void loop(void) {
  static uint32_t lastUpdate = 0;  // Track last display update
  static bool flasher = false;     // Colon flasher for time display
  
  // Update display animation
  P.displayAnimate();
  
  // Check WiFi connection periodically (power efficient)
  checkAndRestoreWiFi();
  
  // Update time and display every second
  if (millis() - lastUpdate >= 1000) {
    lastUpdate = millis();
    
    // Update time from system
    updateTime();
    
    // Format time with colon flasher effect
    formatTime(szTime, flasher);
    formatSeconds(szsecond);
    
    // Toggle colon for blinking effect
    flasher = !flasher;
    
    // Update display zones with new time values
    P.displayZoneText(0, szsecond, PA_LEFT, SPEED_TIME, 0, PA_PRINT, PA_NO_EFFECT);
    P.displayZoneText(1, szTime, PA_CENTER, SPEED_TIME, PAUSE_TIME, PA_PRINT, PA_NO_EFFECT);
  }
}

void getTimentp() __attribute__((deprecated("Use syncTimeFromNTP() instead")));
void getTimentp() {
  syncTimeFromNTP();
}
