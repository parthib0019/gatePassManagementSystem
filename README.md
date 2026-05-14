# Gate Pass Management System V2

An intelligent ESP32-based access control system featuring real-time WebSocket synchronization, smart state-based entry/exit tracking, and discontinuous time slot scheduling for students.

## Features

- **Smart Single NFC Reader**: 
  - A single PN532 scanner intelligently handles both entries and exits.
  - Checks the local `out_Stu` cache: If the student is already outside, tapping the card grants entry (State `1`). If they are inside, it validates rules before granting exit (State `0`).
- **Discontinuous Free Time Slots**: Authority can configure multiple discontinuous free time blocks for a given day.
- **Granular Student Permissions**: Individual students can be granted multiple, discontinuous gate pass time slots on the same day.
- **Real-time Synchronization**: Uses WebSockets to instantly push configuration changes, permission updates, and the list of currently absent students to the ESP32.
- **Power-Loss Recovery**: The server maintains an `OUT_STUDENTS` database table and syncs it to the ESP32 upon connection, ensuring the system remembers who is outside even after a power failure.
- **Visual & Audio Feedback**: Uses an Adafruit NeoPixel Matrix and a buzzer for clear feedback (Green = Exit, Blue = Entry, Red = Denied).

## Hardware Requirements

- **Microcontroller**: ESP32 Development Board
- **NFC Reader**: 1x PN532 Module (SPI Interface)
- **Indicators**: 
  - Adafruit NeoPixel Matrix (16 pixels)
  - Active Buzzer
- **Wiring**:

| Component | ESP32 Pin | Note |
|---|---|---|
| PN532 Data | SCK(18), MISO(19), MOSI(23) | SPI Bus |
| PN532 Chip Select | GPIO 5 | SS |
| NeoPixel Matrix | GPIO 14 | DIN |
| Buzzer | GPIO 21 | Active High |

## Software Setup

### Server (Python/Flask)
1.  **Prerequisites**: Python 3.x, `pip`.
2.  **Installation**:
    ```bash
    pip install flask flask-sock
    ```
3.  **Run Server**:
    ```bash
    python3 serverFiles/server.py
    ```
    The server runs on `http://0.0.0.0:5000` by default.

### Firmware (ESP32)
1.  **IDE**: Arduino IDE or PlatformIO.
2.  **Libraries**:
    -   `Adafruit_PN532`
    -   `Adafruit_NeoPixel`
    -   `WebSockets`
3.  **Configuration**:
    -   Open `esp32_gatepass/esp32_gatepass.ino`.
    -   Update `ssid` and `password` with your WiFi credentials.
    -   Update `wsHost` and `timeServerUrl` with your server's domain/IP.

## API Endpoints

### 1. Upload Permissions (CSV)
-   **Endpoint**: `/PermitedPDFSubmission`
-   **Method**: `POST`
-   **Parameters**: `password`, `file`
-   **CSV Format**: `STARTING_DATE, ENDING_DATE, STARTING_TIME, ENDING_TIME, RFID`
-   **Description**: Uploads student-specific gate passes. Overlapping intervals for the same student are fully supported.

### 2. Set Free Time Slots
-   **Endpoint**: `/restrictedTimeDeclearation`
-   **Method**: `POST`
-   **Parameters**: 
    - `password`: Server password
    - `date`: Format `YYYY-MM-DD`
    - `free_times`: JSON array of datetime strings (e.g., `["2026-05-01 07:19:00", "2026-05-01 07:30:00"]`)

### 3. Download Today's Permissions
-   **Endpoint**: `/todayList`
-   **Method**: `GET`
-   **Description**: Downloads a CSV of all active permission slots for the current day.

### 4. Export Tracking Logs
-   **Endpoint**: `/get_tracker_csv`
-   **Method**: `POST`
-   **Parameters**: `password`, `date` (Optional)

## Database Schema (`additionals/database.db`)

The SQLite database is auto-initialized by the server.
-   **Student_tracker**: Stores all entry (1) and exit (0) events with timestamps.
-   **PERMISSION_LIST**: Stores individual student gate pass intervals (`STARTING_DATE`, `ENDING_DATE`, `STARTING_TIME`, `ENDING_TIME`, `RFID`).
-   **FREE_TIME_LOG**: Chronological log of global free time configurations.
-   **OUT_STUDENTS**: Real-time table storing the RFIDs of all students currently outside the campus.


## curl command to change the free time:
curl -X POST \
  -d "password=GatePassSecurity" \
  -d "date=2026-05-01" \
  -d 'free_times=["2026-05-01 07:39:00", "2026-05-01 07:40:00","2026-05-01 09:40:00","2026-05-01 10:55:00"]' \
  https://nonmetalliferous-callen-anciently.ngrok-free.dev/restrictedTimeDeclearation

## curl commant to upload the permission list:
curl -X POST -F "password=GatePassSecurity" -F "file=@test.csv" https://nonmetalliferous-callen-anciently.ngrok-free.dev/PermitedPDFSubmission

## curl commant to download the tracking log:
curl -X POST -d "password=GatePassSecurity" -d "date=2026-05-01" https://nonmetalliferous-callen-anciently.ngrok-free.dev/get_tracker_csv -o logs.csv