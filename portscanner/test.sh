#!/bin/bash
python3 -m http.server 8080 >/dev/null 2>&1 &
python3 -m http.server 9090 >/dev/null 2>&1 &
sleep 1
./scanner 127.0.0.1 -p 1-10000
pkill -f http.server
