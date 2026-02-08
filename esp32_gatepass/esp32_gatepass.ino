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
#define PN532_SS 5

// --------------------------------------------------------------------------
// DATA STRUCTURES
// --------------------------------------------------------------------------
struct StudentPerm {
  uint32_t uid;
  uint32_t start; // Unix Timestamp
  uint32_t end;   // Unix Timestamp
};

// --------------------------------------------------------------------------
// GLOBAL VARIABLES (Shared Resources)
// --------------------------------------------------------------------------
std::vector<StudentPerm> permittedStudents;
time_t globalRestrictedStart = 0;
time_t globalRestrictedEnd = 0;

SemaphoreHandle_t listMutex;

// NFC Objects
PN532_SPI pn532spi(SPI, PN532_SS);
PN532 nfc(pn532spi);
bool nfcConnected = false;
unsigned long lastCardRead = 0;
const int COOLDOWN_MS = 2000;

// --------------------------------------------------------------------------
// TASKS
// --------------------------------------------------------------------------
void syncDataTask(void *parameter) {
  while (1) {
    if (WiFi.status() == WL_CONNECTED) {
      // Create a local buffer to process incoming data
      HTTPClient http;
      http.begin(serverUrl);
      int httpCode = http.GET();

      if (httpCode == HTTP_CODE_OK) {
        int len = http.getSize();
        WiFiClient *stream = http.getStreamPtr();

        std::vector<uint8_t> rxBuffer;

        // Temp storage for parsing
        uint32_t tempGlobalStart = 0;
        uint32_t tempGlobalEnd = 0;
        uint32_t tempCount = 0;
        bool headerParsed = false;

        std::vector<StudentPerm> newList;

        // Read loop
        while (http.connected() && (len > 0 || len == -1)) {
          size_t size = stream->available();
          if (size) {
            uint8_t chunk[128];
            int c = stream->readBytes(
                chunk, ((size > sizeof(chunk)) ? sizeof(chunk) : size));

            if (c > 0) {
              rxBuffer.insert(rxBuffer.end(), chunk, chunk + c);

              // 1. Try to parse Header (12 bytes)
              if (!headerParsed && rxBuffer.size() >= 12) {
                memcpy(&tempGlobalStart, &rxBuffer[0], 4);
                memcpy(&tempGlobalEnd, &rxBuffer[4], 4);
                memcpy(&tempCount, &rxBuffer[8], 4);

                rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + 12);
                headerParsed = true;
              }

              // 2. Parse Records (12 bytes each)
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

        // Critical Section: Update
        xSemaphoreTake(listMutex, portMAX_DELAY);
        permittedStudents = newList;
        globalRestrictedStart = tempGlobalStart;
        globalRestrictedEnd = tempGlobalEnd;
        xSemaphoreGive(listMutex);

        Serial.printf("[Sync] Updated. Global: %u-%u, Count: %d\n",
                      tempGlobalStart, tempGlobalEnd, newList.size());

      } else {
        Serial.printf("[Sync] HTTP Error: %d\n", httpCode);
      }
      http.end();
    } else {
      Serial.println("[Sync] WiFi not connected");
    }

    // Period: 30 seconds
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

bool initializeNFC() {
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata)
    return false;
  nfc.setPassiveActivationRetries(0xFF);
  nfc.SAMConfig();
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
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  if (initializeNFC()) {
    Serial.println("[NFC] Ready");
    nfcConnected = true;
  } else {
    Serial.println("[NFC] Not Found");
  }
}

// --------------------------------------------------------------------------
// LOOP (Core 1)
// --------------------------------------------------------------------------
void loop() {
  if (!nfcConnected) {
    delay(5000);
    if (initializeNFC())
      nfcConnected = true;
    return;
  }

  uint8_t uid[7];
  uint8_t uidLength;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
    if (millis() - lastCardRead < COOLDOWN_MS)
      return;
    lastCardRead = millis();

    unsigned long cardID = 0;
    if (uidLength == 4) {
      cardID = ((unsigned long)uid[3] << 24) | ((unsigned long)uid[2] << 16) |
               ((unsigned long)uid[1] << 8) | ((unsigned long)uid[0]);
    } else if (uidLength == 7) {
      cardID = ((unsigned long)uid[6] << 24) | ((unsigned long)uid[5] << 16) |
               ((unsigned long)uid[4] << 8) | ((unsigned long)uid[3]);
    }

    Serial.printf("[Scanner] Card ID: %u\n", cardID);

    // TIME LOGIC CHECK
    time_t now = time(nullptr);
    bool accessGranted = false;

    xSemaphoreTake(listMutex, portMAX_DELAY);
    Serial.printf("[Access] Global: %lu-%lu, now: %lu, Count: %d\n",
                  (unsigned long)globalRestrictedStart,
                  (unsigned long)globalRestrictedEnd, (unsigned long)now,
                  permittedStudents.size());

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
          Serial.printf("Found uid: %lu\n startInterval: %lu, end interval: "
                        "%lu\n, now: %lu\n",
                        (unsigned long)student.uid,
                        (unsigned long)student.start,
                        (unsigned long)student.end, (unsigned long)now);
          // Check Individual Interval
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
      Serial.println("[Scanner] ACCESS GRANTED");
      blinkLED(LED_GREEN);
    } else {
      Serial.println("[Scanner] ACCESS DENIED");
      blinkLED(LED_RED);
    }
  }
  delay(10);
}
