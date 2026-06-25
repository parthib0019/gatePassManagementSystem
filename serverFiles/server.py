from flask import Flask, request, Response
from flask_cors import CORS
import sqlite3
import struct
import os
import time
import csv
import io
from datetime import datetime, date
import json # Added for JSON handling
from flask_sock import Sock # Added for WebSocket functionality

app = Flask(__name__)
# Enable CORS for all routes (important for cross-origin requests)
CORS(app)
sock = Sock(app) # Initialize Flask-Sock
active_clients = set() # Keep track of active WebSocket clients

# Database Config
DB_FILE = 'additionals/database.db'
PASSWORD = "GatePassSecurity" # Replace with actual password logic if needed

def get_db_connection():
    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    """Initialize database tables if they don't exist"""
    if not os.path.exists('additionals'):
        os.makedirs('additionals')
        
    conn = get_db_connection()
    try:
        conn.execute('''
            CREATE TABLE IF NOT EXISTS PERMISSION_LIST (
                ID INTEGER PRIMARY KEY AUTOINCREMENT, 
                STARTING_DATE TEXT NOT NULL, 
                ENDING_DATE TEXT NOT NULL, 
                STARTING_TIME TEXT NOT NULL, 
                ENDING_TIME NOT NULL, 
                RFID TEXT NOT NULL, 
                STATUS TEXT NOT NULL DEFAULT 'ACTIVE' CHECK(STATUS IN ('ACTIVE', 'EXPIRED', 'DEACTIVE')), 
                TYPE TEXT NOT NULL DEFAULT 'ONETIME' CHECK(TYPE IN('ONETIME', 'MONTHLY')));
        ''')
        
        conn.execute('''
            CREATE TABLE IF NOT EXISTS OUT_STUDENTS (
                RFID INTEGER PRIMARY KEY
            );
        ''')
        
        # Table for Free Time Slots Configuration
        conn.execute('''
            CREATE TABLE FREE_TIME_LOG (
                ID INTEGER PRIMARY KEY AUTOINCREMENT,
                CONFIG_DATE DATE NOT NULL,
                TIME_LIST TEXT NOT NULL, 
                TYPE CHAR(1));
        ''')
        
        # Table for Student Tracking (Entry/Exit)
        conn.execute('''
            CREATE TABLE IF NOT EXISTS Student_tracker (
                ID INTEGER PRIMARY KEY AUTOINCREMENT,
                RFID INTEGER NOT NULL,
                STATE INTEGER NOT NULL,
                DATE_TIME TEXT NOT NULL
            );
        ''')
        
        # Drop old table if exists (Migration)
        conn.execute('DROP TABLE IF EXISTS OUTGOING_LOG')
        conn.commit()
    except Exception as e:
        print(f"DB Init Error: {e}")
    finally:
        conn.close()

# Initialize on startup
init_db()

def Permitted_List_genarater():
    today_str = date.today().isoformat()
    conn = get_db_connection()
    
    # 1. Normal data
    normal_data = conn.execute('''
        SELECT * FROM PERMISSION_LIST 
        WHERE STARTING_DATE <= ? AND ENDING_DATE >= ?
    ''', (today_str, today_str)).fetchall()
    
    # 2. Today's exits
    tracker_data = conn.execute('''
        SELECT RFID, DATE_TIME FROM Student_tracker
        WHERE STATE = 0 AND DATE_TIME LIKE ?
    ''', (today_str + '%',)).fetchall()
    
    exits = {}
    for row in tracker_data:
        rfid = str(row['RFID'])
        if rfid not in exits:
            exits[rfid] = []
        exits[rfid].append(row['DATE_TIME'])
        
    # 3. Filter
    valid_grouped = {}
    fmt = "%Y-%m-%d %H:%M:%S"
    
    for row in normal_data:
        rfid = str(row['RFID'])
        start_time_str = row['STARTING_TIME']
        end_time_str = row['ENDING_TIME']
        
        start_dt_str = f"{today_str} {start_time_str}"
        end_dt_str = f"{today_str} {end_time_str}"
        
        used = False
        if rfid in exits:
            for exit_dt in exits[rfid]:
                if start_dt_str <= exit_dt <= end_dt_str:
                    used = True
                    break
        
        if not used:
            try:
                start_ts = int(datetime.strptime(start_dt_str, fmt).timestamp())
                end_ts = int(datetime.strptime(end_dt_str, fmt).timestamp())
                if rfid not in valid_grouped:
                    valid_grouped[rfid] = []
                valid_grouped[rfid].append((start_ts, end_ts))
            except Exception as e:
                print(f"Time parse error: {e}")
                
    conn.close()
    
    # Pack to binary
    # Format: [student_count(4)] then for each student: [RFID(4)][slot_count(4)][start1][end1]...
    blob = bytearray()
    blob.extend(struct.pack('<I', len(valid_grouped)))
    for rfid_str, slots in valid_grouped.items():
        try:
            rfid_int = int(rfid_str)
            blob.extend(struct.pack('<II', rfid_int, len(slots)))
            for start_ts, end_ts in slots:
                blob.extend(struct.pack('<II', start_ts, end_ts))
        except ValueError:
            pass # Invalid RFID
            
    return blob

def get_current_binary_payload():
    """Returns the Header + BLOB representing current free times and daily permissions"""
    today = date.today().isoformat()
    conn = get_db_connection()
    row = conn.execute('SELECT * FROM FREE_TIME_LOG WHERE CONFIG_DATE = ? ORDER BY ID DESC LIMIT 1', (today,)).fetchone()
    
    if not row:
        default_row = conn.execute("SELECT * FROM FREE_TIME_LOG WHERE TYPE = 'D' ORDER BY ID DESC LIMIT 1").fetchone()
        if default_row and default_row['TIME_LIST']:
            try:
                default_times = json.loads(default_row['TIME_LIST'])
                today_dt = date.today()
                modified_times = []
                for ts in default_times:
                    dt = datetime.fromtimestamp(ts)
                    new_dt = dt.replace(year=today_dt.year, month=today_dt.month, day=today_dt.day)
                    modified_times.append(int(new_dt.timestamp()))
                
                new_time_list = json.dumps(modified_times)
                cursor = conn.execute("INSERT INTO FREE_TIME_LOG (CONFIG_DATE, TIME_LIST, TYPE) VALUES (?, ?, ?)", (today, new_time_list, 'D'))
                conn.commit()
                row = conn.execute("SELECT * FROM FREE_TIME_LOG WHERE CONFIG_DATE = ? ORDER BY ID DESC LIMIT 1", (today,)).fetchone()
            except Exception as e:
                print(f"Error processing default TIME_LIST: {e}")
    
    free_times = []
    if row and row['TIME_LIST']:
        try:
            free_times = json.loads(row['TIME_LIST'])
        except Exception as e:
            print(f"Error parsing TIME_LIST: {e}")
            free_times = []
            
    conn.close()
    
    free_time_count = len(free_times) // 2
    
    # Pack header: [free_time_count (4)]
    header = struct.pack('<I', free_time_count)
    
    # Pack free times
    free_time_blob = bytearray()
    for t in free_times:
        free_time_blob.extend(struct.pack('<I', int(t)))
        
    perm_blob = Permitted_List_genarater()
    
    # Pack out_students
    conn = get_db_connection()
    out_rows = conn.execute('SELECT RFID FROM OUT_STUDENTS').fetchall()
    conn.close()
    
    out_blob = bytearray()
    out_blob.extend(struct.pack('<I', len(out_rows)))
    for r in out_rows:
        out_blob.extend(struct.pack('<I', int(r['RFID'])))
        
    return header + free_time_blob + perm_blob + out_blob

@app.route('/', methods=['GET'])
def home():
    return "GatePass Server V2 Running"

@app.route('/current_time', methods=['GET'])
def current_time():
    """Returns current server time as Unix timestamp"""
    return str(int(time.time()))

@app.route('/todayList', methods=['GET'])
def today_list():
    today_str = date.today().isoformat()
    conn = get_db_connection()
    rows = conn.execute('SELECT * FROM PERMISSION_LIST WHERE STARTING_DATE <= ? AND ENDING_DATE >= ?', (today_str, today_str)).fetchall()
    conn.close()
    
    output = io.StringIO()
    writer = csv.writer(output)
    writer.writerow(['RFID', 'STARTING_DATE', 'ENDING_DATE', 'STARTING_TIME', 'ENDING_TIME'])
    for r in rows:
        writer.writerow([r['RFID'], r['STARTING_DATE'], r['ENDING_DATE'], r['STARTING_TIME'], r['ENDING_TIME']])
        
    return Response(
        output.getvalue(),
        mimetype="text/csv",
        headers={"Content-disposition": f"attachment; filename=permitted_students_{today_str}.csv"}
    )

def handle_tracking_data(tracks):
    """Save incoming tracking data from ESP32 to DB"""
    if not tracks: return
    conn = get_db_connection()
    try:
        for track in tracks:
            uid = track.get('uid')
            ts = track.get('ts')
            state = track.get('state')
            if uid is not None and ts is not None and state is not None:
                dt_str = datetime.fromtimestamp(int(ts)).strftime('%Y-%m-%d %H:%M:%S')
                conn.execute('INSERT INTO Student_tracker (RFID, STATE, DATE_TIME) VALUES (?, ?, ?)',
                             (uid, state, dt_str))
                
                # OUT_STUDENTS logic
                if state == 0: # Exit
                    conn.execute('INSERT OR IGNORE INTO OUT_STUDENTS (RFID) VALUES (?)', (uid,))
                elif state == 1: # Entry
                    cursor = conn.execute('DELETE FROM OUT_STUDENTS WHERE RFID = ?', (uid,))
                    if cursor.rowcount == 0:
                        print(f"Log: ID {uid} not found in OUT_STUDENTS during entry.")
        conn.commit()
        print(f"Synced {len(tracks)} tracking records.")
    except Exception as e:
        print(f"WS Sync Error: {e}")
    finally:
        conn.close()

@sock.route('/ws')
def ws_gatepass(ws):
    active_clients.add(ws)
    print("New WebSocket client connected.")
    try:
        # Immediately send current binary state upon connection
        initial_payload = get_current_binary_payload()
        ws.send(initial_payload)
        
        # Listen for tracking uploads
        while True:
            data = ws.receive()
            if data:
                try:
                    json_data = json.loads(data)
                    if 'tracking' in json_data:
                        handle_tracking_data(json_data['tracking'])
                except json.JSONDecodeError:
                    print("Received invalid JSON on WS")
    except Exception as e:
        print(f"WS Disconnected: {e}")
    finally:
        if ws in active_clients:
            active_clients.remove(ws)

def broadcast_payload():
    """Send the current binary payload to all active clients"""
    new_payload = get_current_binary_payload()
    dead_clients = set()
    for client in active_clients:
        try:
            client.send(new_payload)
        except Exception:
            dead_clients.add(client)
    active_clients.difference_update(dead_clients)

@app.route('/PermitedPDFSubmission', methods=['POST'])
def submit_permissions():
    """
    Form Submission: 'password', 'file' (CSV)
    CSV Format: STARTING_DATE, ENDING_DATE, STARTING_TIME, ENDING_TIME, RFID
    """
    password = request.form.get('password')
    if password != PASSWORD:
        return "Unauthorized", 401
    
    file = request.files.get('file')
    if not file:
        return "No file uploaded", 400
        
    try:
        content = file.read().decode('utf-8').strip().split('\n')
    except Exception as e:
        return f"Error reading file: {e}", 400

    conn = get_db_connection()
    inserted = 0
    try:
        for line in content:
            line = line.strip()
            if not line or line.lower().startswith('start'): continue
            
            parts = [p.strip() for p in line.split(',')]
            if len(parts) >= 5:
                conn.execute('''
                    INSERT INTO PERMISSION_LIST (STARTING_DATE, ENDING_DATE, STARTING_TIME, ENDING_TIME, RFID)
                    VALUES (?, ?, ?, ?, ?)
                ''', (parts[0], parts[1], parts[2], parts[3], parts[4]))
                inserted += 1
        conn.commit()
    except Exception as e:
        conn.close()
        return f"DB error: {e}", 500
        
    conn.close()
    broadcast_payload()  # Push update to ESP32s
    return f"Success. Inserted {inserted} records.", 200

@app.route('/restrictedTimeDeclearation', methods=['POST'])
def set_restricted_time():
    """
    Form: 'date' (YYYY-MM-DD)
    Form: 'free_times' (JSON Array String of Normal Timestamps: ["2026-04-30 08:00:00", "2026-04-30 12:00:00"])
    """
    password = request.form.get('password')
    if password != PASSWORD:
        return "Unauthorized", 401
        
    try:
        free_times_str = request.form.get('free_times', '[]')
        free_times_raw = json.loads(free_times_str)
        type = request.form.get('type', 'T');
        if not isinstance(free_times_raw, list) or len(free_times_raw) % 2 != 0:
            return "Invalid free_times format. Must be an array of even length.", 400
            
        # Convert "YYYY-MM-DD HH:MM:SS" to unix timestamps
        free_times_unix = []
        fmt = "%Y-%m-%d %H:%M:%S"
        for t_str in free_times_raw:
            dt = datetime.strptime(str(t_str).strip(), fmt)
            free_times_unix.append(int(dt.timestamp()))
            
        today_str = request.form.get("date")
        if not today_str:
            today_str = date.today().isoformat()
            
        final_free_times_str = json.dumps(free_times_unix)

        conn = get_db_connection()
        if type == 'T':
            conn.execute('INSERT INTO FREE_TIME_LOG (CONFIG_DATE, TIME_LIST, TYPE) VALUES (?, ?, ?)',(today_str, final_free_times_str, 'T'))
        else:
            conn.execute('INSERT INTO FREE_TIME_LOG (CONFIG_DATE, TIME_LIST, TYPE) VALUES (?, ?, ?)',(today_str, final_free_times_str, 'D'))
        conn.commit()
        conn.close()
        broadcast_payload()  # Push update to ESP32s
        return "Free Time Configured", 200
    except json.JSONDecodeError:
        return "Invalid JSON in free_times", 400
    except ValueError as e:
        return f"Invalid time format, expected YYYY-MM-DD HH:MM:SS. Error: {e}", 400
    except Exception as e:
        return f"DB Error: {e}", 500

@app.route('/permitted_students', methods=['GET', 'POST'])
def get_permitted_students():
    """Legacy HTTP Sync route - Supported alongside WebSockets"""
    if request.method == 'POST':
        try:
            data = request.get_json()
            if data and 'tracking' in data:
                handle_tracking_data(data['tracking'])
        except Exception:
            pass
            
    payload = get_current_binary_payload()
    return Response(payload, mimetype='application/octet-stream')

@app.route('/get_tracker_csv', methods=['POST'])
def get_tracker_csv():
    """
    Download Student_tracker as CSV.
    Requires 'password' in form data.
    Optional 'date' (YYYY-MM-DD) to filter.
    """
    password = request.form.get('password')
    date = request.form.get('date')
    if password != PASSWORD:
        return "Unauthorized", 401
    
    conn = get_db_connection()
    try:
        # Get logs (filter by date if provided)
        if date:
            cursor = conn.execute('SELECT * FROM Student_tracker WHERE DATE_TIME LIKE ? ORDER BY ID DESC', (date + '%',))
        else:
            cursor = conn.execute('SELECT * FROM Student_tracker ORDER BY ID DESC')
        rows = cursor.fetchall()
    except Exception as e:
        conn.close()
        return f"DB Error: {e}", 500
    conn.close()

    # Generate CSV
    output = io.StringIO()
    writer = csv.writer(output)
    # Headers
    writer.writerow(['ID', 'RFID', 'STATE', 'DATE_TIME'])
    
    for row in rows:
        writer.writerow([row['ID'], row['RFID'], row['STATE'], row['DATE_TIME']])
        
    return Response(
        output.getvalue(),
        mimetype="text/csv",
        headers={"Content-disposition": "attachment; filename=student_tracking.csv"}
    )


if __name__ == '__main__':
    app.run(host='127.0.0.1', port=5000)
