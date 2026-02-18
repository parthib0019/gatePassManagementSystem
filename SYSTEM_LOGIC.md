# System Logic Breakdown

This document explains exactly how the **Gate Pass Management System** works, step-by-step ("line-by-line" logic).

---

## 1. ESP32 Firmware Logic (`esp32_gatepass.ino`)

The firmware runs on two concurrent CPU cores.

### A. Setup Phase (`setup()`)
1.  **Initialize Serial**: Starts communication at 115200 baud for debugging.
2.  **GPIO Init**: Sets LED pins (Green/Red) as Outputs.
3.  **WiFi Connection**: Tries to connect to the configured WiFi SSID. Blocks until connected.
4.  **Time Sync (NTP)**: Fetches real-time from `pool.ntp.org`. This is critical for timestamping logs.
5.  **Task Launch**: Creates the `syncDataTask` on **Core 0** (Background).
6.  **NFC Init**:
    -   Initializes **Reader 1 (Exit)** on CS Pin 5.
    -   Initializes **Reader 2 (Entry)** on CS Pin 22.

### B. Core 1: The Scanning Loop (`loop()`)
This loop runs continuously to detect cards.

**Step 1: Check Reader 1 (Exit)**
-   **Scan**: Checks if a card is near Reader 1.
-   **Debounce**: Ignores reads if less than 2 seconds since last read.
-   **Logic**:
    1.  **Permission Check**:
        -   If `globalRestrictedStart/End` are 0 (No restriction) -> **GRANT**.
        -   If Current Time is *outside* Global Restriction -> **GRANT**.
        -   If Inside Restriction -> Check `permittedStudents` list for matching UID & Time Slot.
    2.  **Action**:
        -   **Granted**: Blink Green LED. Log to `trackCache` with **State 0 (Exit)**.
        -   **Denied**: Blink Red LED.

**Step 2: Check Reader 2 (Entry)**
-   **Scan**: Checks Reader 2.
-   **Logic**:
    -   Entry is **Always Allowed** (Assumes students returning are valid).
    -   **Action**: Blink Green LED. Log to `trackCache` with **State 1 (Entry)**.

---

### C. Core 0: The Sync Task (`syncDataTask()`)
This task runs in infinite loop every **1 second**.

1.  **Prepare Payload**:
    -   Locks `trackCache` (Mutex).
    -   Copies all pending records to a JSON payload: `{"tracking": [{"uid":..., "ts":..., "state":...}]}`.
    -   Unlocks Mutex.
2.  **Upload (POST)**:
    -   Sends JSON to Server (`/permitted_students`).
    -   **Success (200 OK)**: Clears the uploaded records from `trackCache`.
3.  **Download Updates**:
    -   Reads response from Server.
    -   **Header (12 bytes)**: Global Start Time, Global End Time, Record Count.
    -   **Body**: List of Permitted Students `{UID, Start, End}`.
    -   **Update**: Locks `permittedStudents` list and replaces it with the new data.

---

## 2. Server Logic (`server.py`)

The Python Flask server handles the backend logic.

### A. Startup
-   **Database**: Checks if `additionals/database.db` exists.
-   **Tables**: Ensures `Student_tracker`, `PERMISSION_LIST`, and `RESTRICTED_PERIOD` tables exist.

### B. Endpoint: `/permitted_students` (POST)
This is the heartbeat endpoint called by the ESP32.

1.  **Receive Data**: Reads the `tracking` JSON array from the request.
2.  **Process Logs**:
    -   Iterates through each log `{uid, state, ts}`.
    -   Converts timestamp `ts` to readable String (`YYYY-MM-DD HH:MM:SS`).
    -   Inserts into SQLite table `Student_tracker`.
3.  **Prepare Response**:
    -   Fetches the **Latest Restricted Period** from DB.
    -   Fetches **Today's Permissions** (Binary Blob) from `PERMISSION_LIST`.
    -   **Combine**: Packs Global Times (Head) + Permissions (Body) into binary format.
    -   **Send**: Returns binary stream to ESP32.

### C. Endpoint: `/get_tracker_csv` (POST)
1.  **Auth**: Checks password.
2.  **Output**:
    -   Queries `Student_tracker`.
    -   Filters by `DATE_TIME` if a `date` parameter is provided.
    -   Generates a CSV file and streams it as a download.

---

## 3. Data Flow Summary

1.  **Tap Card** (ESP32) -> **Log to RAM** (`trackCache`).
2.  **Sync Task** (Every 1s) -> **POST to Server**.
3.  **Server** -> **Save to DB** (`Student_tracker`).
4.  **Server** -> **Reply with Permissions** -> **Update ESP32 RAM**.
