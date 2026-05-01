/*
 * ESP32 Dual-Core Gatepass System V2
 *
 * Core 0: Background Sync (WiFi -> Server -> Update RAM Permitted List)
 * Core 1: Real-time Scanning (RFID -> Check Time Rules -> Check List -> Blink
 * LED)
 *
 * Features:
 * - NTP Time Sync
 * - SQLite-backed Binary Protocol
 * - Global Restricted Periods
 * - Individual Student Time Slots
 *
 * Hardware Config:
 * - RGB LED Matrix: Pin 6 (16 pixels)
 * - RFID (PN532): SCK=18, MISO=19, MOSI=23, SS=5 (VSPI)
 */
#include <Adafruit_PN532.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <Wire.h>
#include <algorithm>
#include <time.h>
#include <vector>
#include <Adafruit_NeoPixel.h>

#define PIN 14
#define NUMPIXELS 16

Adafruit_NeoPixel pixels(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

// --------------------------------------------------------------------------
// CONFIGURATION
// --------------------------------------------------------------------------
const char *ssid = "RKMV_CSMA_ELTG";
const char *password = "MerVer@2.0.3";
//time delta
const long gmtOffset_sec = 19800; // UTC +5:30 (India Standard Time)
const int daylightOffset_sec = 0;

// Replace with your ngrok URL (must be updated every time ngrok restarts)
// String timeServerUrl ="https://gatepass.rkmvmfamily.in/current_time";
String timeServerUrl ="https://nonmetalliferous-callen-anciently.ngrok-free.dev/current_time";

String wsHost = "nonmetalliferous-callen-anciently.ngrok-free.dev";
int wsPort = 443;
String wsPath = "/ws";

WebSocketsClient webSocket;

// Pin Config
#define PN532_SCK 18
#define PN532_MISO 19
#define PN532_MOSI 23
#define PN532_SS_1 5
#define PN532_SS_2 22
#define buzzer 21

// --------------------------------------------------------------------------
// DATA STRUCTURES
// --------------------------------------------------------------------------

struct StudentPerm {
  uint32_t uid;
  std::vector<uint32_t> slots;
};

struct TrackRecord {
  uint32_t uid;
  uint32_t ts;
  int state; // 0 = Exit, 1 = Entry
};

// --------------------------------------------------------------------------
// GLOBAL VARIABLES (Shared Resources)
// --------------------------------------------------------------------------
std::vector<StudentPerm> permittedStudents;
std::vector<TrackRecord> trackCache;
std::vector<uint32_t> freeTimeSlots;

SemaphoreHandle_t listMutex;

// NFC Objects
// Using Hardware SPI (Default VSPI pins: 18, 19, 23)
Adafruit_PN532 nfc1(PN532_SS_1);
Adafruit_PN532 nfc2(PN532_SS_2);
bool nfc1Connected = false;
bool nfc2Connected = false;
unsigned long lastCardRead = 0;
const int COOLDOWN_MS = 2000;

// --------------------------------------------------------------------------
// WEBSOCKET HANDLER
// --------------------------------------------------------------------------
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WSc] Disconnected!\n");
      break;
    case WStype_CONNECTED:
      Serial.printf("[WSc] Connected to url: %s\n", payload);
      break;
    case WStype_TEXT:
      Serial.printf("[WSc] get text: %s\n", payload);
      break;
    case WStype_BIN:
      Serial.printf("[WSc] get binary length: %u\n", length);
      if (length >= 4) {
          uint32_t freeTimeCount = 0;
          memcpy(&freeTimeCount, &payload[0], 4);
          
          size_t processed = 4;
          std::vector<uint32_t> newFreeTimes;
          
          if (length >= processed + (freeTimeCount * 8)) {
              for (uint32_t i = 0; i < freeTimeCount * 2; i++) {
                  uint32_t t;
                  memcpy(&t, &payload[processed], 4);
                  newFreeTimes.push_back(t);
                  processed += 4;
              }
          }
          
          std::vector<StudentPerm> newList;
          if (length >= processed + 4) {
              uint32_t studentCount = 0;
              memcpy(&studentCount, &payload[processed], 4);
              processed += 4;
              
              for (uint32_t i = 0; i < studentCount; i++) {
                  if (processed + 8 > length) break;
                  StudentPerm p;
                  memcpy(&p.uid, &payload[processed], 4);
                  
                  uint32_t slotCount = 0;
                  memcpy(&slotCount, &payload[processed + 4], 4);
                  processed += 8;
                  
                  for (uint32_t j = 0; j < slotCount; j++) {
                      if (processed + 8 > length) break;
                      uint32_t start, end;
                      memcpy(&start, &payload[processed], 4);
                      memcpy(&end, &payload[processed + 4], 4);
                      p.slots.push_back(start);
                      p.slots.push_back(end);
                      processed += 8;
                  }
                  newList.push_back(p);
              }
          }
          
          xSemaphoreTake(listMutex, portMAX_DELAY);
          permittedStudents = newList;
          freeTimeSlots = newFreeTimes;
          xSemaphoreGive(listMutex);
          
          Serial.printf("[WSc] List Updated. Free slots: %d, Students: %d\n", freeTimeCount, newList.size());
      }
      break;
    case WStype_ERROR:      
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }
}

// --------------------------------------------------------------------------
// TASKS
// --------------------------------------------------------------------------
void syncDataTask(void *parameter) {
  while (1) {
    webSocket.loop();
    
    xSemaphoreTake(listMutex, portMAX_DELAY);
    bool hasData = !trackCache.empty();
    xSemaphoreGive(listMutex);
    
    static unsigned long lastSend = 0;
    if (hasData && millis() - lastSend > 1000) { 
        lastSend = millis();
        String jsonPayload = "{\"tracking\":[";
        std::vector<TrackRecord> tempCache;

        xSemaphoreTake(listMutex, portMAX_DELAY);
        tempCache = trackCache; 
        xSemaphoreGive(listMutex);

        for (size_t i = 0; i < tempCache.size(); i++) {
            jsonPayload += "{\"uid\":" + String(tempCache[i].uid) +
                           ",\"ts\":" + String(tempCache[i].ts) +
                           ",\"state\":" + String(tempCache[i].state) + "}";
            if (i < tempCache.size() - 1)
              jsonPayload += ",";
        }
        jsonPayload += "]}";
        
        bool success = webSocket.sendTXT(jsonPayload);
        if (success) {
            xSemaphoreTake(listMutex, portMAX_DELAY);
            if (trackCache.size() >= tempCache.size()) {
               trackCache.erase(trackCache.begin(), trackCache.begin() + tempCache.size());
            } else {
               trackCache.clear();
            }
            xSemaphoreGive(listMutex);
            Serial.printf("[WSc] Sent %d tracking records\n", tempCache.size());
        }
    }
    
    vTaskDelay(20 / portTICK_PERIOD_MS); 
  }
}

// --------------------------------------------------------------------------
// HELPER FUNCTIONS
// --------------------------------------------------------------------------
void playTone(int count, int onDuration, int offDuration) {
  for (int i = 0; i < count; i++) {
    digitalWrite(buzzer, HIGH);
    delay(onDuration);
    digitalWrite(buzzer, LOW);
    if (i < count - 1)
      delay(offDuration);
  }
}

void setMatrixColor(uint32_t color) {
  for(int i = 0; i < NUMPIXELS; i++) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void signalGranted() {
  // Green Matrix + 1 Beep
  setMatrixColor(pixels.Color(0, 255, 0));
  playTone(1, 200, 0);
  delay(1000); // Keep matrix green for a bit
  setMatrixColor(pixels.Color(255, 255, 255)); // Revert to white
}

void signalDenied() {
  // Red Matrix + 3 Fast Beeps
  setMatrixColor(pixels.Color(255, 0, 0));
  playTone(5, 100, 100);
  delay(500);
  setMatrixColor(pixels.Color(255, 255, 255)); // Revert to white
}

bool initializeNFC(Adafruit_PN532 &nfc_obj) {
  nfc_obj.begin();
  uint32_t versiondata = nfc_obj.getFirmwareVersion();
  if (!versiondata)
    return false;
  nfc_obj.setPassiveActivationRetries(0xFF);
  nfc_obj.SAMConfig();
  return true;
}

void syncTimeWithServer() {
  Serial.println("Synchronizing Time with Server...");
  WiFiClient *client = nullptr;
  if (timeServerUrl.startsWith("https://")) {
    WiFiClientSecure *secClient = new WiFiClientSecure();
    secClient->setInsecure(); // Skip SSL cert verification
    client = secClient;
  } else {
    client = new WiFiClient();
  }
  HTTPClient http;
  http.begin(*client, timeServerUrl);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    long ts = payload.toInt();
    if (ts > 1000000) { // Valid timestamp check
      struct timeval tv;
      tv.tv_sec =
          ts +
          19800; // Add GMT Offset (19800s = 5h 30m) manually if raw TS is UTC
      // Wait, server returns UTC timestamp? user said "actual time count by the
      // sever". Usually time.time() in Python is UTC. settimeofday expects UTC.
      // But we have localized logic elsewhere?
      // Let's assume server sends UTC and we set UTC, then local time handling
      // works via timezone. Python time.time() is UTC.
      tv.tv_sec = ts;
      tv.tv_usec = 0;
      settimeofday(&tv, NULL);

      // Apply Timezone for local print
      setenv("TZ", "IST-5:30", 1);
      tzset();

      Serial.printf("Time synced: %ld\n", ts);
    }
  } else {
    Serial.printf("Time Sync Failed. HTTP: %d\n", httpCode);
  }
  http.end();
  delete client;
}

// --------------------------------------------------------------------------
// SETUP
// --------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(buzzer, OUTPUT);
  digitalWrite(buzzer, LOW);

  // Initialize NeoPixel Matrix
  pixels.begin();
  pixels.setBrightness(10); // Set brightness to avoid excessive current draw
  setMatrixColor(pixels.Color(255, 255, 255)); // Set default to white

  // Startup Sound
  playTone(1, 100, 0);

  listMutex = xSemaphoreCreateMutex();

  Serial.println("--- ESP32 Dual Core Gatepass V2 ---");

  // 1. Connect WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int i = 0;
  while (WiFi.status() != WL_CONNECTED && i < 20) {
    delay(500);
    Serial.print(".");
    i++;
  }
  Serial.println("\nWiFi Connected");
  // WiFi Connected Sound: 1s wait, then 2 beeps
  delay(1000);
  playTone(2, 200, 100);

  // 2. Init Time (Server)
  // configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2,
  //            ntpServer3);
  // Serial.println("Synchronizing Time (Google/Pool/NIST)...");
  syncTimeWithServer();

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println(&timeinfo, "Time Set: %A, %B %d %Y %H:%M:%S");
  } else {
    Serial.println("Failed to obtain time");
  }

  // 3. Setup WebSocket
  // use beginSSL for ngrok WSS endpoints
  webSocket.beginSSL(wsHost, wsPort, wsPath);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);

  // 4. Start Sync Task
  xTaskCreatePinnedToCore(syncDataTask, "SyncTask", 10000, NULL, 1, NULL, 0);

  // 4. Setup NFC
  // manually set SS to HIGH to deselect
  pinMode(PN532_SS_1, OUTPUT);
  pinMode(PN532_SS_2, OUTPUT);
  digitalWrite(PN532_SS_1, HIGH);
  digitalWrite(PN532_SS_2, HIGH);

  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI,
            -1); // -1 to disable default SS handling
  delay(100);

  Serial.print("[Setup] Init Reader 1 (EXIT)... ");
  if (initializeNFC(nfc1)) {
    Serial.println("OK");
    nfc1Connected = true;
  } else {
    Serial.println("FAILED");
  }
  delay(100);

  Serial.print("[Setup] Init Reader 2 (ENTRY)... ");
  if (initializeNFC(nfc2)) {
    Serial.println("OK");
    nfc2Connected = true;
  } else {
    Serial.println("FAILED");
  }
  playTone(3, 100, 100);
}

// --------------------------------------------------------------------------
// LOOP (Core 1)
// --------------------------------------------------------------------------
void loop() {
  time_t now = time(nullptr);

  // --------------------------------------------------------
  // Check Reader 1 (EXIT) - Logic: Check Permissions -> Log State 0
  // --------------------------------------------------------
  if (nfc1Connected) {
    uint8_t uid[7];
    uint8_t uidLength;

    if (nfc1.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
      if (millis() - lastCardRead > COOLDOWN_MS) {
        lastCardRead = millis();

        unsigned long cardID = 0;
        if (uidLength == 4) {
          cardID = ((unsigned long)uid[3] << 24) |
                   ((unsigned long)uid[2] << 16) |
                   ((unsigned long)uid[1] << 8) | ((unsigned long)uid[0]);
        }

        Serial.printf("\n>>> [Reader 1: EXIT] Card Detect: %u\n", cardID);

        // --- EXIT PERMISSION CHECK ---
        bool accessGranted = false;
        xSemaphoreTake(listMutex, portMAX_DELAY);

        bool inFreeTime = false;
        for (size_t i = 0; i + 1 < freeTimeSlots.size(); i += 2) {
            if (now >= freeTimeSlots[i] && now <= freeTimeSlots[i+1]) {
                inFreeTime = true;
                break;
            }
        }

        if (inFreeTime) {
            accessGranted = true;
            Serial.println("Mode: Free Time (Open)");
        } else {
            Serial.println("Mode: Restricted (Checking List)");
            bool found = false;
            
            for (auto studentIt = permittedStudents.begin(); studentIt != permittedStudents.end(); ++studentIt) {
                if (studentIt->uid == cardID) {
                    for (size_t i = 0; i + 1 < studentIt->slots.size(); i += 2) {
                        if (now >= studentIt->slots[i] && now <= studentIt->slots[i+1]) {
                            found = true;
                            accessGranted = true;
                            Serial.println("Student Interval: Match");
                            
                            // Erase this slot to prevent double-usage before server sync
                            studentIt->slots.erase(studentIt->slots.begin() + i, studentIt->slots.begin() + i + 2);
                            break;
                        }
                    }
                    if (found) break; // Break out of student loop if we granted access
                }
            }
            if (!found)
                Serial.println("ID Not in Permitted List or No Active Slots");
        }
        xSemaphoreGive(listMutex);

        if (accessGranted) {
          Serial.println("[Scanner 1] ACCESS GRANTED");
          // Cache Exit (State 0)
          xSemaphoreTake(listMutex, portMAX_DELAY);
          trackCache.push_back({(uint32_t)cardID, (uint32_t)now, 0});
          xSemaphoreGive(listMutex);
          signalGranted();
        } else {
          Serial.println("[Scanner 1] ACCESS DENIED");
          signalDenied();
        }
      }
    }
  }

  // --------------------------------------------------------
  // Check Reader 2 (ENTRY) - Logic: Always Open -> Log State 1
  // --------------------------------------------------------
  if (nfc2Connected) {
    uint8_t uid[7];
    uint8_t uidLength;

    if (nfc2.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
      if (millis() - lastCardRead > COOLDOWN_MS) {
        lastCardRead = millis();

        unsigned long cardID = 0;
        if (uidLength == 4) {
          cardID = ((unsigned long)uid[3] << 24) |
                   ((unsigned long)uid[2] << 16) |
                   ((unsigned long)uid[1] << 8) | ((unsigned long)uid[0]);
        }

        Serial.printf("\n>>> [Reader 2: ENTRY] Card Detect: %u\n", cardID);

        // --- ENTRY LOGIC (Always Log State 1) ---
        xSemaphoreTake(listMutex, portMAX_DELAY);
        trackCache.push_back({(uint32_t)cardID, (uint32_t)now, 1});
        xSemaphoreGive(listMutex);

        Serial.println("[Scanner 2] Entry Logged");
        signalGranted();
      }
    }
  }

  delay(10);
}
