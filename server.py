from flask import Flask, request, Response
import csv
import struct
import os

app = Flask(__name__)

# CSV File Path
CSV_FILE = 'CSVsheets/GatePassPermited.csv'


@app.route('/permitted_students', methods=['GET'])
def get_permitted_students():
    """
    Reads the CSV file and returns valid card IDs as a binary stream 
    of 4-byte little-endian integers (uint32_t).
    """
    if not os.path.exists(CSV_FILE):
        print("CSV file not found!")
        return "CSV not found", 404
        
    binary_data = bytearray()
    
    try:
        with open(CSV_FILE, mode='r') as f:
            count = 0
            for line in f:
                line = line.strip()
                
                if not line:
                    continue
                try:
                    card_id = int(line)
                    if card_id > 0:
                        # Pack as 4-byte unsigned int, little-endian
                        binary_data.extend(struct.pack('<I', card_id))
                        count += 1
                except ValueError:
                    continue # Skip non-integer lines
            
            print(f"Serving {count} permitted IDs to ESP32.")
            
            # Return binary response
            return Response(bytes(binary_data), mimetype='application/octet-stream')
            
    except Exception as e:
        print(f"Error reading CSV: {e}")
        return str(e), 500

if __name__ == '__main__':
    # Run on default port 5000
    app.run(host='0.0.0.0', port=5000)
