/*
  ESP32 Weather Station with dual 8x8 WS2812 LED matrix
  - Left 8x8 panel: displays weather animation
  - Right 8x8 panel: displays current temperature
  - Weather data from Open-Meteo API (no API key needed)
  - WiFi config via WiFiManager AP portal
  - ArduinoOTA support for wireless firmware update

  Required libraries:
  - Adafruit_NeoPixel
  - ArduinoJson
  - WiFiManager (by tzapu)
  - ArduinoOTA (built-in for ESP32)
  - WiFi / WebServer / HTTPClient (built-in for ESP32)

  Hardware wiring (ESP32):
  - 3.3V  -> ESP32 3.3V
  - GND   -> ESP32 GND
  - WS2812 DIN -> GPIO 13
  - Config button -> GPIO 0 (optional, pull to GND to reset config)
  - Two 8x8 panels chained: left panel first, right panel second
  - Each panel uses sequential row-major pixel layout (not zigzag)
  - Display is rotated 90° clockwise
*/

#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "weather_icons.h"
#include "temperature_range.h"

// ======================== USER CONFIGURATION ========================

// LED settings
#define LED_PIN       13
#define LED_COUNT     128     // two 8x8 panels
#define BRIGHTNESS    50      // 0-255

// Panel offsets
#define WEATHER_PANEL_OFFSET  0    // Left panel:  LED 0-63
#define TEMP_PANEL_OFFSET     64   // Right panel: LED 64-127

// Open-Meteo settings
#define UPDATE_INTERVAL_MS  600000L  // update weather every 10 minutes
#define TIMEOUT_MS          10000    // HTTP request timeout

// WiFiManager settings
#define AP_SSID     "WeatherStation-Config"
#define AP_PASSWORD "12345678"
#define CONFIG_PORTAL_TIMEOUT_S  180  // 3 minutes

// OTA settings
#define OTA_PASSWORD "12345678"

// Config reset button (hold GPIO0 LOW for 3s to clear WiFi settings)
#define CONFIG_TRIGGER_PIN  0
#define TRIGGER_HOLD_MS     3000

// Display refresh rate limit (keeps WiFi/OTA stable)
#define DISPLAY_INTERVAL_MS 33  // ~30 FPS

// ======================== GLOBAL VARIABLES ========================

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

WiFiManager wm;
Preferences prefs;

// Stored configuration
float cfgLatitude  = 39.9042;   // default: Beijing
float cfgLongitude = 116.4074;

// Current weather data
float currentTemperature = 25.0;
int currentWeatherCode   = 0;
bool weatherAvailable    = false;
unsigned long lastWeatherUpdate = 0;

// Animation state
int currentWeatherIcon = WEATHER_SUNNY;
int currentFrame       = 0;
unsigned long lastFrameTime = 0;
unsigned long lastDisplayUpdate = 0;

// System state
bool configPortalActive = false;
bool wifiConnected = false;  // false -> play Tetris default animation (offline mode)

// Default animation shown whenever WiFi is not connected.
// Uses the cloudy GIF from weather_icons.h (its later frames contain
// Tetris-style falling-block patterns).
#define DEFAULT_ANIMATION_ICON  WEATHER_CLOUDY

// Reconnect cadence in offline mode (ms between attempts)
#define OFFLINE_RECONNECT_INTERVAL_MS 10000

// ======================== FORWARD DECLARATIONS ========================

void loadConfig();
void saveConfig();
void setupOTA();
void checkConfigTrigger();
void configModeCallback(WiFiManager* myWiFiManager);
void fetchWeather();
void mapWeatherCodeToIcon();
void updateDisplay();
void displayFrame(const uint32_t* framePtr, int offset);
void clearLeds();
void playDefaultAnimation();

// ======================== SETUP ========================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\nESP32 Weather Station starting...");

  // Optional config reset button
  pinMode(CONFIG_TRIGGER_PIN, INPUT_PULLUP);

  // Initialize LED strip
  strip.begin();
  strip.show();
  strip.setBrightness(BRIGHTNESS);
  clearLeds();

  // Load saved config (latitude/longitude)
  loadConfig();

  // Check if user wants to reset WiFi config
  if (digitalRead(CONFIG_TRIGGER_PIN) == LOW) {
    Serial.println("Config trigger detected, resetting WiFi settings...");
    wm.resetSettings();
    delay(1000);
    ESP.restart();
  }

  // Setup WiFiManager
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);
  wm.setAPCallback(configModeCallback);

  // Custom parameters for lat/lon
  char latBuf[12], lonBuf[12];
  dtostrf(cfgLatitude,  0, 4, latBuf);
  dtostrf(cfgLongitude, 0, 4, lonBuf);

  WiFiManagerParameter custom_lat("lat", "Latitude",  latBuf, 12);
  WiFiManagerParameter custom_lon("lon", "Longitude", lonBuf, 12);
  wm.addParameter(&custom_lat);
  wm.addParameter(&custom_lon);

  // Try to connect, or start config portal
  bool connected = wm.autoConnect(AP_SSID, AP_PASSWORD);

  if (connected) {
    wifiConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.println("IP: " + WiFi.localIP().toString());

    // Save lat/lon from portal
    cfgLatitude  = atof(custom_lat.getValue());
    cfgLongitude = atof(custom_lon.getValue());
    saveConfig();

    // Start OTA
    setupOTA();

    // Fetch weather immediately
    fetchWeather();
  } else {
    // Offline mode: keep running so the default Tetris animation can play.
    // WiFi will be re-attempted periodically from loop().
    wifiConnected = false;
    Serial.println("\nWiFi not connected - entering offline mode.");
    Serial.println("Playing default Tetris animation on weather panel.");
  }
}

// ======================== LOOP ========================

void loop() {
  // Handle OTA updates (only meaningful once WiFi is up)
  if (wifiConnected) {
    ArduinoOTA.handle();
  }

  // Check config reset button
  checkConfigTrigger();

  // Track WiFi state and handle (re)connection
  bool currentlyConnected = (WiFi.status() == WL_CONNECTED);

  if (currentlyConnected) {
    // Just (re)connected?
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.println("\nWiFi (re)connected!");
      Serial.println("IP: " + WiFi.localIP().toString());
      // Start OTA now that we have WiFi (it is a no-op if already started)
      setupOTA();
      // Reset weather fetch timer so we fetch immediately
      lastWeatherUpdate = 0;
      // Reset animation frame so weather icon starts clean
      currentFrame = 0;
    }
  } else {
    // Was connected, now lost? Or still offline?
    if (wifiConnected) {
      wifiConnected = false;
      Serial.println("WiFi lost - switching to offline Tetris animation.");
      currentFrame = 0;
    }

    // Periodically attempt to reconnect (no restart on failure)
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > OFFLINE_RECONNECT_INTERVAL_MS) {
      lastReconnectAttempt = millis();
      Serial.println("Offline: attempting WiFi.reconnect()...");
      WiFi.reconnect();
    }
  }

  // Fetch weather periodically (only when connected)
  if (wifiConnected && (millis() - lastWeatherUpdate > UPDATE_INTERVAL_MS)) {
    fetchWeather();
  }

  // Update display at limited frame rate
  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL_MS) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }

  delay(1);  // yield to WiFi/OTA tasks
}

// ======================== CONFIGURATION ========================

void loadConfig() {
  prefs.begin("weather_cfg", false);
  cfgLatitude  = prefs.getFloat("lat", 39.9042);
  cfgLongitude = prefs.getFloat("lon", 116.4074);
  prefs.end();

  Serial.println("Loaded config:");
  Serial.println("  Lat:  " + String(cfgLatitude, 4));
  Serial.println("  Lon:  " + String(cfgLongitude, 4));
}

void saveConfig() {
  prefs.begin("weather_cfg", false);
  prefs.putFloat("lat", cfgLatitude);
  prefs.putFloat("lon", cfgLongitude);
  prefs.end();

  Serial.println("Config saved:");
  Serial.println("  Lat: " + String(cfgLatitude, 4));
  Serial.println("  Lon: " + String(cfgLongitude, 4));
}

void configModeCallback(WiFiManager* myWiFiManager) {
  configPortalActive = true;
  Serial.println("\n=================================");
  Serial.println("Config portal started!");
  Serial.println("Connect to WiFi: " + String(AP_SSID));
  Serial.println("Password: " + String(AP_PASSWORD));
  Serial.println("Then open: http://" + WiFi.softAPIP().toString());
  Serial.println("=================================\n");
}

void checkConfigTrigger() {
  static bool lastState = HIGH;
  static unsigned long pressStart = 0;

  bool currentState = digitalRead(CONFIG_TRIGGER_PIN);

  if (lastState == HIGH && currentState == LOW) {
    pressStart = millis();
  }

  if (lastState == LOW && currentState == LOW) {
    if (millis() - pressStart >= TRIGGER_HOLD_MS) {
      Serial.println("\nConfig trigger held, resetting WiFi settings and restarting...");
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
  }

  lastState = currentState;
}

void setupOTA() {
  ArduinoOTA.setHostname("weather-station");
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA update starting...");
    clearLeds();
    strip.show();
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA update finished!");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)     Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.println("OTA ready at weather-station.local or IP " + WiFi.localIP().toString());
}

// ======================== WEATHER FETCHING ========================

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Cannot fetch weather: WiFi not connected");
    return;
  }

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(cfgLatitude, 4)
             + "&longitude=" + String(cfgLongitude, 4)
             + "&current=temperature_2m,weather_code&timezone=auto";

  Serial.println("Fetching: " + url);

  WiFiClientSecure client;
  client.setInsecure();  // skip certificate verification for Open-Meteo

  HTTPClient http;
  http.setTimeout(TIMEOUT_MS);
  http.begin(client, url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    Serial.println("Weather data received.");

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      JsonObject current = doc["current"];
      currentTemperature = current["temperature_2m"] | 25.0;
      currentWeatherCode = current["weather_code"] | 0;
      weatherAvailable   = true;
      lastWeatherUpdate  = millis();

      Serial.print("Temperature: "); Serial.println(currentTemperature);
      Serial.print("Weather code: "); Serial.println(currentWeatherCode);

      mapWeatherCodeToIcon();
    } else {
      Serial.print("JSON parse failed: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.print("HTTP error code: ");
    Serial.println(httpCode);
  }

  http.end();
}

void mapWeatherCodeToIcon() {
  int code = currentWeatherCode;

  if (code == 0 || code == 1 || code == 2) {
    currentWeatherIcon = WEATHER_SUNNY;
  } else if (code == 3 || code == 45 || code == 48) {
    currentWeatherIcon = WEATHER_CLOUDY;
  } else if ((code >= 51 && code <= 67) || (code >= 71 && code <= 86)) {
    currentWeatherIcon = WEATHER_RAINY;
  } else if (code == 95) {
    currentWeatherIcon = WEATHER_LIGHTNING;
  } else if (code == 96 || code == 99) {
    currentWeatherIcon = WEATHER_LIGHTNING_RAIN;
  } else {
    currentWeatherIcon = WEATHER_SUNNY;
  }

  Serial.print("Selected icon: ");
  Serial.println(weatherIcons[currentWeatherIcon].name);
}

// ======================== DISPLAY ========================

void displayFrame(const uint32_t* framePtr, int offset) {
  // Direct sequential mapping (same as LedMatrix.ino)
  // Data is pre-rotated 90° CW in the .h files
  for (int i = 0; i < 64; i++) {
    uint32_t color = pgm_read_dword(&framePtr[i]);

    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    strip.setPixelColor(offset + i, strip.Color(r, g, b));
  }
}

void displayWeatherIcon(int iconIndex, int frameIndex) {
  WeatherIcon icon = weatherIcons[iconIndex];

  if (frameIndex < 0) frameIndex = 0;
  if (frameIndex >= icon.frameCount) frameIndex = icon.frameCount - 1;

  // Read frame pointer from PROGMEM
  const uint32_t* framePtr;
  memcpy_P(&framePtr, &icon.frames[frameIndex], sizeof(uint32_t*));

  displayFrame(framePtr, WEATHER_PANEL_OFFSET);
}

void displayTemperature(int temp) {
  // Clamp to available range: -30 to 40
  if (temp < TEMP_MIN) temp = TEMP_MIN;
  if (temp > TEMP_MAX) temp = TEMP_MAX;

  int index = temp - TEMP_MIN;  // -30 -> 0, 40 -> 70

  // Read frame pointer from PROGMEM
  const uint32_t* framePtr;
  memcpy_P(&framePtr, &tempFrameData[index], sizeof(uint32_t*));

  displayFrame(framePtr, TEMP_PANEL_OFFSET);
}

void updateDisplay() {
  // Offline: play the default Tetris animation (cloudy GIF) on the left panel
  // and keep the right (temperature) panel blank.
  if (!wifiConnected) {
    playDefaultAnimation();
    return;
  }

  // Online: normal weather + temperature display
  // Select weather icon
  const WeatherIcon& icon = weatherIcons[currentWeatherIcon];

  // Advance animation frame
  if (millis() - lastFrameTime >= icon.frameDelay) {
    lastFrameTime = millis();
    currentFrame++;
    if (currentFrame >= icon.frameCount) {
      currentFrame = 0;
    }
  }

  // Draw weather animation on left panel
  displayWeatherIcon(currentWeatherIcon, currentFrame);

  // Draw temperature on right panel
  int displayTemp = round(currentTemperature);
  displayTemperature(displayTemp);

  strip.show();
}

// Plays the default Tetris animation when WiFi is not connected.
// Uses the cloudy GIF from weather_icons.h on the left panel and
// clears the right panel since we have no temperature data offline.
void playDefaultAnimation() {
  const WeatherIcon& icon = weatherIcons[DEFAULT_ANIMATION_ICON];

  // Advance animation frame
  if (millis() - lastFrameTime >= icon.frameDelay) {
    lastFrameTime = millis();
    currentFrame++;
    if (currentFrame >= icon.frameCount) {
      currentFrame = 0;
    }
  }

  // Draw default animation on the left (weather) panel
  displayWeatherIcon(DEFAULT_ANIMATION_ICON, currentFrame);

  // Clear the right (temperature) panel
  for (int i = 0; i < 64; i++) {
    strip.setPixelColor(TEMP_PANEL_OFFSET + i, 0);
  }

  strip.show();
}

void clearLeds() {
  strip.clear();
}
