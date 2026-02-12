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
 * - Green LED: D2
 * - Red LED:   D4
 * - RFID (PN532): SCK=18, MISO=19, MOSI=23, SS=5 (VSPI)
 */

#include "PN532.h"
#include <HTTPClient.h>
#include <PN532_SPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <algorithm>
#include <time.h>
#include <vector>

// --------------------------------------------------------------------------
// CONFIGURATION
// --------------------------------------------------------------------------
const char *ssid = "RKMV_CSMA_ELTG";
const char *password = "MerVer@2.0.3";
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // UTC +5:30 (India Standard Time)
const int daylightOffset_sec = 0;

// Replace with your ngrok URL (must be updated every time ngrok restarts)
String serverUrl = "https://nonmetalliferous-callen-anciently.ngrok-free.dev/"
                   "permitted_students";

// Pin Config
#define LED_GREEN 21
#define LED_RED 4
#define PN532_SCK 18
#define PN532_MISO 19
#define PN532_MOSI 23
#define PN532_SS_1 5
#define PN532_SS_2 22

// --------------------------------------------------------------------------
// DATA STRUCTURES
// --------------------------------------------------------------------------

struct StudentPerm {
  uint32_t uid;
  uint32_t start; // Unix Timestamp
  uint32_t end;   // Unix Timestamp
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
time_t globalRestrictedStart = 0;
time_t globalRestrictedEnd = 0;

SemaphoreHandle_t listMutex;

// NFC Objects
PN532_SPI pn532spi1(SPI, PN532_SS_1);
PN532_SPI pn532spi2(SPI, PN532_SS_2);
PN532 nfc1(pn532spi1);
PN532 nfc2(pn532spi2);
bool nfc1Connected = false;
bool nfc2Connected = false;
unsigned long lastCardRead = 0;
const int COOLDOWN_MS = 2000;

// --------------------------------------------------------------------------
// TASKS
// --------------------------------------------------------------------------
void syncDataTask(void *parameter) {
  while (1) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.begin(serverUrl);
      http.addHeader("Content-Type", "application/json");

      // 1. Prepare JSON Payload from Cache
      String jsonPayload = "{\"tracking\":[";
      std::vector<TrackRecord> tempCache;

      xSemaphoreTake(listMutex, portMAX_DELAY);
      tempCache = trackCache; // Copy cache to temp
      xSemaphoreGive(listMutex);

      for (size_t i = 0; i < tempCache.size(); i++) {
        jsonPayload += "{\"uid\":" + String(tempCache[i].uid) +
                       ",\"ts\":" + String(tempCache[i].ts) +
                       ",\"state\":" + String(tempCache[i].state) + "}";
        if (i < tempCache.size() - 1)
          jsonPayload += ",";
      }
      jsonPayload += "]}";

      // 2. POST (Upload Cache + Get New List)
      int httpCode = http.POST(jsonPayload);

      if (httpCode == HTTP_CODE_OK) {
        // Success! Clear uploaded records from main cache
        if (!tempCache.empty()) {
          xSemaphoreTake(listMutex, portMAX_DELAY);
          if (trackCache.size() >= tempCache.size()) {
            trackCache.erase(trackCache.begin(),
                             trackCache.begin() + tempCache.size());
          } else {
            trackCache.clear();
          }
          xSemaphoreGive(listMutex);
          Serial.printf("[Sync] Uploaded %d tracking records\n",
                        tempCache.size());
        }

        // 3. Process Response (Binary List)
        int len = http.getSize();
        WiFiClient *stream = http.getStreamPtr();

        std::vector<uint8_t> rxBuffer;
        uint32_t tempGlobalStart = 0;
        uint32_t tempGlobalEnd = 0;
        uint32_t tempCount = 0;
        bool headerParsed = false;
        std::vector<StudentPerm> newList;

        while (http.connected() && (len > 0 || len == -1)) {
          size_t size = stream->available();
          if (size) {
            uint8_t chunk[128];
            int c = stream->readBytes(
                chunk, ((size > sizeof(chunk)) ? sizeof(chunk) : size));
            if (c > 0) {
              rxBuffer.insert(rxBuffer.end(), chunk, chunk + c);

              // Parse Header
              if (!headerParsed && rxBuffer.size() >= 12) {
                memcpy(&tempGlobalStart, &rxBuffer[0], 4);
                memcpy(&tempGlobalEnd, &rxBuffer[4], 4);
                memcpy(&tempCount, &rxBuffer[8], 4);
                rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + 12);
                headerParsed = true;
              }

              // Parse Records
              if (headerParsed) {
                size_t processed = 0;
                while (processed + 12 <= rxBuffer.size()) {
                  StudentPerm p;
                  memcpy(&p.uid, &rxBuffer[processed], 4);
                  memcpy(&p.start, &rxBuffer[processed + 4], 4);
                  memcpy(&p.end, &rxBuffer[processed + 8], 4);
                  newList.push_back(p);
                  processed += 12;
                }
                if (processed > 0) {
                  rxBuffer.erase(rxBuffer.begin(),
                                 rxBuffer.begin() + processed);
                }
              }
              if (len > 0)
                len -= c;
            }
          }
          delay(1);
        }

        // Update Permitted List
        xSemaphoreTake(listMutex, portMAX_DELAY);
        permittedStudents = newList;
        globalRestrictedStart = tempGlobalStart;
        globalRestrictedEnd = tempGlobalEnd;
        xSemaphoreGive(listMutex);

        Serial.printf("[Sync] List Updated. Count: %d\n", newList.size());

      } else {
        Serial.printf("[Sync] HTTP Error: %d\n", httpCode);
      }
      http.end();
    } else {
      Serial.println("[Sync] WiFi not connected");
    }

    // Interval: 1 second
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// --------------------------------------------------------------------------
// HELPER FUNCTIONS
// --------------------------------------------------------------------------
void blinkLED(int pin) {
  digitalWrite(pin, HIGH);
  delay(500);
  digitalWrite(pin, LOW);
}

bool initializeNFC(PN532 &nfc_obj) {
  nfc_obj.begin();
  uint32_t versiondata = nfc_obj.getFirmwareVersion();
  if (!versiondata)
    return false;
  nfc_obj.setPassiveActivationRetries(0xFF);
  nfc_obj.SAMConfig();
  return true;
}

// --------------------------------------------------------------------------
// SETUP
// --------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);

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
  digitalWrite(22, HIGH);

  // 2. Init Time (NTP)
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Synchronizing Time...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println(".");
    delay(500);
  }
  Serial.println(&timeinfo, "Time Set: %A, %B %d %Y %H:%M:%S");

  // 3. Start Sync Task
  xTaskCreatePinnedToCore(syncDataTask, "SyncTask", 10000, NULL, 1, NULL, 0);

  // 4. Setup NFC
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, 5); // Default SS

  Serial.print("[Setup] Init Reader 1 (EXIT)... ");
  if (initializeNFC(nfc1)) {
    Serial.println("OK");
    nfc1Connected = true;
  } else {
    Serial.println("FAILED");
  }

  Serial.print("[Setup] Init Reader 2 (ENTRY)... ");
  if (initializeNFC(nfc2)) {
    Serial.println("OK");
    nfc2Connected = true;
  } else {
    Serial.println("FAILED");
  }
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
        Serial.printf("global restricted start: %d\n", globalRestrictedStart);
        Serial.printf("global restricted end: %d\n", globalRestrictedEnd);

        Serial.printf("current time %d\n", now);

        // Logic 1: Default Open (Green) if no restricted period
        if (globalRestrictedStart == 0 && globalRestrictedEnd == 0) {
          accessGranted = true;
          Serial.println("Mode: Unrestricted (Open)");
        }
        // Logic 2: Open if OUTSIDE restricted period
        else if (now < globalRestrictedStart && now > globalRestrictedEnd) {
          accessGranted = true;
          Serial.println("Mode: Outside Restriction (Open)");
        }
        // Logic 3: Restricted Period - Check List
        else {
          Serial.println("Mode: Restricted (Checking List)");
          bool found = false;
          for (const auto &student : permittedStudents) {
            if (student.uid == cardID) {
              if (now >= student.start && now <= student.end) {
                found = true;
                accessGranted = true;
                Serial.println("Student Interval: Match");
              } else {
                found = true;
                Serial.println("Student Interval: Expired/Future");
              }
              break;
            }
          }
          if (!found)
            Serial.println("ID Not in Permitted List");
        }
        xSemaphoreGive(listMutex);

        if (accessGranted) {
          Serial.println("[Scanner 1] ACCESS GRANTED");
          // Cache Exit (State 0)
          xSemaphoreTake(listMutex, portMAX_DELAY);
          trackCache.push_back({(uint32_t)cardID, (uint32_t)now, 0});
          xSemaphoreGive(listMutex);
          blinkLED(LED_GREEN);
        } else {
          Serial.println("[Scanner 1] ACCESS DENIED");
          blinkLED(LED_RED);
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
        blinkLED(LED_GREEN);
      }
    }
  }

  delay(10);
}
