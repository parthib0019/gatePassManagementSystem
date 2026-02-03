/*
 * ESP32 RFID Reader (Simplified)
 *
 * Pin Configuration (Standard VSPI):
 * SCK  -> D18
 * MISO -> D19
 * MOSI -> D23
 * SS   -> D5
 */

#include "PN532.h"
#include <PN532_SPI.h>
#include <SPI.h>

#define PN532_SCK 18
#define PN532_MISO 19
#define PN532_MOSI 23
#define PN532_SS 5

PN532_SPI pn532spi(SPI, PN532_SS);
PN532 nfc(pn532spi);

bool nfcConnected = false;
unsigned long lastCardRead = 0;
const int COOLDOWN_MS = 2000;

void setup() {
  Serial.begin(115200);
  // Wait a moment for serial to be ready
  delay(2000);

  // Initialize SPI
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);

  // Init NFC
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();

  if (!versiondata) {
    Serial.println("Error: PN532 not found.");
    while (1)
      ; // Halt
  }

  nfc.setPassiveActivationRetries(0xFF);
  nfc.SAMConfig();

  Serial.println("Waiting for the card...");
  nfcConnected = true;
}

void loop() {
  if (!nfcConnected)
    return; // Should allow retry, but keeping it simple as requested

  uint8_t uid[7];
  uint8_t uidLength;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50)) {
    if (millis() - lastCardRead < COOLDOWN_MS) {
      return;
    }
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

    // Print JUST the number as requested
    Serial.println(cardID);
  }
}