#!/bin/bash

#starting the virtual environment

/workspace/Workspace/Workspace/MY_projects/gatePassManagementSystem/venv/bin/python serverFiles/server.py >> server.log 2>&1 &

#inicializing ngrok
echo "$(date): Starting Ngrok..." >> ngrok.log
ngrok http 5000 --domain=nonmetalliferous-callen-anciently.ngrok-free.dev >> ngrok.log 2>&1


