/*
 * ESP32 Dual-Core Gatepass System
 *
 * Core 0: Background Sync (WiFi -> Server -> Update RAM Permitted List)
 * Core 1: Real-time Scanning (RFID -> Check List -> Blink LED)
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
#include <vector>

// --------------------------------------------------------------------------
// CONFIGURATION
// --------------------------------------------------------------------------
const char *ssid = "RKMV_CSMA_ELTG";
const char *password = "MerVer@2.0.3";
// Replace with your ngrok URL (must be updated every time ngrok restarts)
String serverUrl = "https://9284120ee26f.ngrok-free.app/permitted_students";

// Pin Config
#define LED_GREEN 2
#define LED_RED 4
#define PN532_SCK 18
#define PN532_MISO 19
#define PN532_MOSI 23
#define PN532_SS 5

// --------------------------------------------------------------------------
// GLOBAL VARIABLES (Shared Resources)
// --------------------------------------------------------------------------
std::vector<uint32_t> permittedStudents;
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
      Serial.println("[Sync] Fetching permitted list...");

      HTTPClient http;
      http.begin(serverUrl);

      // We expect binary stream
      int httpCode = http.GET();

      if (httpCode == HTTP_CODE_OK) {
        // Get length
        int len = http.getSize();
        uint8_t buff[128] = {0};

        WiFiClient *stream = http.getStreamPtr();

        // Temporary vector to build new list
        std::vector<uint32_t> newList;

        // Read bytes
        while (http.connected() && (len > 0 || len == -1)) {
          size_t size = stream->available();
          if (size) {
            // Read up to 128 bytes (32 integers)
            int c = stream->readBytes(
                buff, ((size > sizeof(buff)) ? sizeof(buff) : size));

            // We need chunks of 4 bytes
            // (This simple logic assumes packets arrive aligned or we process
            // aligned chunks. For robust stream, we should buffer remainders.
            // Here we assume reasonable chunks esp for small lists)
            for (int i = 0; i < c; i += 4) {
              if (i + 3 < c) {
                uint32_t id;
                // Little Endian unpack
                memcpy(&id, &buff[i], 4);
                newList.push_back(id);
              }
            }

            if (len > 0)
              len -= c;
          }
          delay(1);
        }

        // Linear search requested, so no need to sort
        // std::sort(newList.begin(), newList.end());

        // CRITICAL SECTION: Update shared list
        xSemaphoreTake(listMutex, portMAX_DELAY);
        permittedStudents = newList;
        xSemaphoreGive(listMutex);

        Serial.printf("[Sync] Updated list. Count: %d\n",
                      permittedStudents.size());

      } else {
        Serial.printf("[Sync] HTTP Error: %d\n", httpCode);
      }
      http.end();

    } else {
      Serial.println("[Sync] WiFi not connected");
      // Try to reconnect? WiFi.begin usually auto-reconnects but we can check
    }

    // Period: 30 seconds
    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

// --------------------------------------------------------------------------
// HELPER FUNCTIONS
// --------------------------------------------------------------------------
void blinkLED(int pin) {
  digitalWrite(pin, HIGH);
  delay(500); // Blocking delay ok here as this runs on main scanner loop
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

  // LEDs
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, LOW);

  // Constants
  String currentUrl = serverUrl; // Keep a local copy if needed

  // Create Mutex
  listMutex = xSemaphoreCreateMutex();

  Serial.println("--- ESP32 Dual Core Gatepass ---");

  // 1. Start WiFi (runs on default event loop, but we monitor in Task 0)
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int i = 0;
  while (WiFi.status() != WL_CONNECTED && i < 20) {
    delay(500);
    Serial.print(".");
    i++;
  }
  Serial.println("\nWiFi Init Done (Connected or Timeout)");

  // 2. Start Sync Task on Core 0
  xTaskCreatePinnedToCore(syncDataTask, /* Function */
                          "SyncTask",   /* Name */
                          10000,        /* Stack size */
                          NULL,         /* Parameter */
                          1,            /* Priority */
                          NULL,         /* Handle */
                          0             /* Core ID */
  );

  // 3. Setup NFC (Core 1 / Main Loop)
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  if (initializeNFC()) {
    Serial.println("[NFC] Ready");
    nfcConnected = true;
  } else {
    Serial.println("[NFC] Not Found (Check Switch/Wires)");
  }
}

// --------------------------------------------------------------------------
// LOOP (Core 1)
// --------------------------------------------------------------------------
void loop() {
  if (!nfcConnected) {
    // Retry init occasionally
    delay(5000);
    if (initializeNFC())
      nfcConnected = true;
    return;
  }

  uint8_t uid[7];
  uint8_t uidLength;

  // Scan
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
    if (millis() - lastCardRead < COOLDOWN_MS)
      return;
    lastCardRead = millis();

    // Calculate ID
    unsigned long cardID = 0;
    if (uidLength == 4) {
      cardID = ((unsigned long)uid[3] << 24) | ((unsigned long)uid[2] << 16) |
               ((unsigned long)uid[1] << 8) | ((unsigned long)uid[0]);
    } else if (uidLength == 7) {
      cardID = ((unsigned long)uid[6] << 24) | ((unsigned long)uid[5] << 16) |
               ((unsigned long)uid[4] << 8) | ((unsigned long)uid[3]);
    }

    Serial.printf("[Scanner] Card ID: %u\n", cardID);

    // Check List
    bool allowed = false;

    xSemaphoreTake(listMutex, portMAX_DELAY);
    // Use linear search (std::find) as requested:
    allowed = (std::find(permittedStudents.begin(), permittedStudents.end(),
                         cardID) != permittedStudents.end());
    xSemaphoreGive(listMutex);

    if (allowed) {
      Serial.println("[Scanner] ACCESS GRANTED");
      blinkLED(LED_GREEN);
    } else {
      Serial.println("[Scanner] ACCESS DENIED");
      blinkLED(LED_RED);
    }
  }

  delay(10); // Yield
}
