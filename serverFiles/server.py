from flask import Flask, request, Response
import sqlite3
import struct
import os
import time
from datetime import datetime, date

app = Flask(__name__)

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
        # Table for Permission Blobs (One blob per day)
        # Storing DATE_TIME as TEXT (YYYY-MM-DD) for simplicity in SQLite
        conn.execute('''
            CREATE TABLE IF NOT EXISTS PERMISSION_LIST (
                DATE_TIME TEXT PRIMARY KEY,
                PERMISSIONS BLOB NOT NULL
            );
        ''')
        
        # Table for Restricted Periods
        # ID Auto-increment
        # Start/End as Unix Timestamps (INTEGERS) based on user feedback
        conn.execute('''
            CREATE TABLE IF NOT EXISTS RESTRICTED_PERIOD (
                ID INTEGER PRIMARY KEY AUTOINCREMENT,
                DATE_TIME_OF_START INTEGER NOT NULL,
                DATE_TIME_OF_END INTEGER NOT NULL
            );
        ''')
        conn.commit()
    except Exception as e:
        print(f"DB Init Error: {e}")
    finally:
        conn.close()

# Initialize on startup
init_db()

@app.route('/', methods=['GET'])
def home():
    return "GatePass Server V2 Running"

@app.route('/PermitedPDFSubmission', methods=['POST'])
def submit_permissions():
    """
    Form Submission: 'password', 'file' (CSV)
    CSV Format: RFID, INTERVAL_START, INTERVAL_ENDS
    Logic: Parse CSV, Convert to Binary Struct [RFID][START][END], 
           Merge with existing blob for Today, Update DB.
    """
    password = request.form.get('password')
    if password != PASSWORD:
        return "Unauthorized", 401
    
    file = request.files.get('file')
    if not file:
        return "No file uploaded", 400
        
    # Read CSV content
    try:
        content = file.read().decode('utf-8').strip().split('\n')
    except Exception as e:
        return f"Error reading file: {e}", 400

    new_records = {} # Map RFID -> (Start, End)

    for line in content:
        line = line.strip()
        if not line or line.lower().startswith('rfid'): continue # Skip empty or header
        
        parts = line.split(',')
        if len(parts) >= 3:
            try:
                rfid = int(parts[0].strip())
                
                # Check if it's integer (Unix Timestamp) or String (Datetime)
                s_str = parts[1].strip()
                e_str = parts[2].strip()
                
                try:
                    start = int(s_str)
                    end = int(e_str)
                except ValueError:
                    # Try Parsing Datetime String
                    fmt = "%Y-%m-%d %H:%M:%S"
                    start = int(datetime.strptime(s_str, fmt).timestamp())
                    end = int(datetime.strptime(e_str, fmt).timestamp())

                new_records[rfid] = (start, end)
            except ValueError:
                continue

    # Fetch existing data for TODAY
    today_str = date.today().isoformat() # YYYY-MM-DD
    conn = get_db_connection()
    existing_blob = None
    try:
        row = conn.execute('SELECT PERMISSIONS FROM PERMISSION_LIST WHERE DATE_TIME = ?', (today_str,)).fetchone()
        if row:
            existing_blob = row['PERMISSIONS']
    except Exception as e:
        print(f"DB Read Error: {e}")
        
    # Merge Logic
    final_records = {}
    
    # Parse existing blob if any
    if existing_blob:
        # BLOB Format: [RFID(4)][START(4)][END(4)] ...
        # No header in the BLOB stored in DB? Plan says "Binary Protocol Change" for *Response*.
        # Storing just records in DB is cleaner.
        chunk_size = 12
        for i in range(0, len(existing_blob), chunk_size):
            chunk = existing_blob[i:i+chunk_size]
            if len(chunk) == chunk_size:
                r_id, r_start, r_end = struct.unpack('<III', chunk)
                final_records[r_id] = (r_start, r_end)
    
    # Update with new records (Overwrite existing)
    final_records.update(new_records)
    
    # Pack back to Blob
    packed_data = bytearray()
    for r_id, (r_start, r_end) in final_records.items():
        packed_data.extend(struct.pack('<III', r_id, r_start, r_end))
        
    # Save to DB
    try:
        conn.execute('''
            INSERT INTO PERMISSION_LIST (DATE_TIME, PERMISSIONS) 
            VALUES (?, ?)
            ON CONFLICT(DATE_TIME) DO UPDATE SET PERMISSIONS=excluded.PERMISSIONS
        ''', (today_str, packed_data))
        conn.commit()
    except Exception as e:
        conn.close()
        return f"DB error: {e}", 500
        
    conn.close()
    return f"Success. Total records for {today_str}: {len(final_records)}", 200

@app.route('/restrictedTimeDeclearation', methods=['POST'])
def set_restricted_time():
    """
    Form: 'restricted_time_start', 'restricted_time_ends' (Unix Timestamps)
    """
    password = request.form.get('password')
    if password != PASSWORD:
        return "Unauthorized", 401
        
    try:
        start = request.form.get('restricted_time_start')
        end = request.form.get('restricted_time_ends')
        
        conn = get_db_connection()
        conn.execute('INSERT INTO RESTRICTED_PERIOD (DATE_TIME_OF_START, DATE_TIME_OF_END) VALUES (?, ?)',
                     (start, end))
        conn.commit()
        conn.close()
        return "Restricted Period Set", 200
    except ValueError:
        return "Invalid integers", 400
    except Exception as e:
        return f"DB Error: {e}", 500

@app.route('/permitted_students', methods=['GET'])
def get_permitted_students():
    """
    Protocol V2:
    Header (12B): [Global_Start(4)][Global_End(4)][Record_Count(4)]
    Body (N*12B): [RFID(4)][Start(4)][End(4)] ...
    """
    conn = get_db_connection()
    
    # 1. Get Restricted Period for TODAY
    # Need to filter by "today"? User schema has just ID/Start/End. 
    # Assumption: We take the LATEST entry added? Or specific to today?
    # User said: "check if there is any restricted time limit... for the current day"
    # We'll assume the client pushes correct timestamps. We'll fetch the latest one that *overlaps* today?
    # Simpler: Fetch the *last* entry added. 
    
    
    # Get latest restriction
    # Get latest restriction
    row = conn.execute('SELECT * FROM RESTRICTED_PERIOD ORDER BY ID DESC LIMIT 1').fetchone()
    if row:
        # Check if it covers "today" roughly? 
        # Or just blindly send it and let ESP32 decide?
        # User logic: "send the binary data... and check if there is any restricted time limit"
        # We will send the timestamps.
        # Parse SQLite DATETIME string (YYYY-MM-DD HH:MM:SS) to Unix Timestamp
        try:
            # Check if it's already an integer (old data?)
            global_start = int(row['DATE_TIME_OF_START'])
            global_end = int(row['DATE_TIME_OF_END'])
        except ValueError:
            # Parse string format
            fmt = "%Y-%m-%d %H:%M:%S"
            dt_s = datetime.strptime(str(row['DATE_TIME_OF_START']), fmt)
            dt_e = datetime.strptime(str(row['DATE_TIME_OF_END']), fmt)
            global_start = int(dt_s.timestamp())
            global_end = int(dt_e.timestamp())
        
    # 2. Get Permissions for TODAY
    today_str = date.today().isoformat()
    perm_row = conn.execute('SELECT PERMISSIONS FROM PERMISSION_LIST WHERE DATE_TIME = ?', (today_str,)).fetchone()
    
    blob = perm_row['PERMISSIONS'] if perm_row else b''
    
    conn.close()
    
    # 3. Construct Response
    record_count = len(blob) // 12
    header = struct.pack('<III', global_start, global_end, record_count)
    
    return Response(header + blob, mimetype='application/octet-stream')

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)
