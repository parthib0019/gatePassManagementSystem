#!/bin/bash

#starting the virtual environment

PYTHONUNBUFFERED=1 /workspace/Workspace/Workspace/MY_projects/gatePassManagementSystem/venv/bin/gunicorn serverFiles.server:app --bind 127.0.0.1:5000 --workers 4  >> server.log 2>&1 &

#inicializing ngrok
echo "$(date): Starting Ngrok..." >> ngrok.log
ngrok http 5000 --domain=nonmetalliferous-callen-anciently.ngrok-free.dev >> ngrok.log 2>&1


