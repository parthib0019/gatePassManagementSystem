# Gate Pass Management System

A dual-core ESP32-based access control system featuring offline caching, real-time server synchronization, and dual NFC reader support for tracking student entry and exit.

## Features

- **Dual NFC Readers**: 
  - **Reader 1 (Exit)**: Checks permissions against a local database (synced from server). Logs exits (State `0`).
  - **Reader 2 (Entry)**: Logs all entries (State `1`).
- **Offline Capability**: Caches access logs locally on the ESP32 when WiFi is unavailable.
- **Real-time Sync**: Automatically uploads cached logs to the server every **1 second**.
- **Server UI/API**: 
  - Upload permission lists via CSV.
  - Set restricted time periods.
  - Download tracking logs as CSV (with date filtering).
- **Status Indicators**: Visual feedback via Green and Red LEDs.

## Hardware Requirements

- **Microcontroller**: ESP32 Development Board
- **NFC Readers**: 2x PN532 Modules (SPI Interface)
- **Indicators**: 
  - Green LED (Access Granted)
  - Red LED (Access Denied)
- **Wiring**:

| Component | ESP32 Pin | Note |
|Data (Both)| SCK(18), MISO(19), MOSI(23) | Shared SPI Bus |
| Reader 1 (Exit) | GPIO 5 | Chip Select (SS) |
| Reader 2 (Entry)| GPIO 22 | Chip Select (SS) |
| Green LED | GPIO 21 | Active High |
| Red LED | GPIO 4 | Active High |

## Software Setup

### Server (Python/Flask)
1.  **Prerequisites**: Python 3.x, `pip`.
2.  **Installation**:
    ```bash
    pip install flask
    ```
3.  **Run Server**:
    ```bash
    python3 serverFiles/server.py
    ```
    The server runs on `http://0.0.0.0:5000` by default.

### Firmware (ESP32)
1.  **IDE**: Arduino IDE or PlatformIO.
2.  **Libraries**:
    -   `PN532` (Adafruit or similar SPI-compatible library)
    -   `HTTPClient`
    -   `WiFi`
3.  **Configuration**:
    -   Open `esp32_gatepass/esp32_gatepass.ino`.
    -   Update `ssid` and `password` with your WiFi credentials.
    -   Update `serverUrl` (e.g., your ngrok URL or local IP).

## API Endpoints

### 1. Sync & Tracking
-   **Endpoint**: `/permitted_students`
-   **Method**: `POST`
-   **Description**: Receives tracking logs from ESP32 and returns updated permission list.
-   **Payload**: `{"tracking": [{"uid": 123, "ts": 170..., "state": 1}]}`

### 2. Export Logs
-   **Endpoint**: `/get_tracker_csv`
-   **Method**: `POST`
-   **Parameters**:
    -   `password`: Server password (Default: `GatePassSecurity`)
    -   `date` (Optional): Filter by date (Format: `YYYY-MM-DD`)
-   **Example**:
    ```bash
    curl -X POST -d "password=GatePassSecurity" -d "date=2026-02-10" http://localhost:5000/get_tracker_csv -o logs.csv
    ```

### 3. Upload Permissions
-   **Endpoint**: `/PermitedPDFSubmission`
-   **Method**: `POST`
-   **Parameters**: `password`, `file` (CSV with headers `RFID, START, END`)

### 4. Set Restricted Time
-   **Endpoint**: `/restrictedTimeDeclearation`
-   **Method**: `POST`
-   **Parameters**: `password`, `restricted_time_start`, `restricted_time_ends` (Unix Timestamps)

## Database
SQLite database is stored at `additionals/database.db`.
-   **Student_tracker**: Stores all entry/exit events.
-   **PERMISSION_LIST**: Stores authorized student IDs and time slots.
-   **RESTRICTED_PERIOD**: Stores global lockdown times.