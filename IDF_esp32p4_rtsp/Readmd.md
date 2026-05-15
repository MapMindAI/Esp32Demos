# esp32p4 websocket jpeg corner Demo

```bash
source $IDF_PATH/export.sh
```

```bash
idf.py build flash monitor
```

## Remote corner debug (WebSocket)

1. Flash and run on board:
```bash
idf.py build flash monitor
```

2. On remote PC, install Python deps:
```bash
pip install opencv-python websocket-client numpy
```

3. Run debug viewer from this repo:
```bash
python3 IDF_esp32p4_rtsp/tools/ws_corner_debug.py \
  --ws-url ws://192.168.19.80:8080/ws
```

Press `q` to quit the viewer window.

Using [ESP32-P4-Module Dev Board](https://www.waveshare.com/esp32-p4-module-dev-kit.htm?srsltid=AfmBOoqSfmLctnNxVbsyf52gsBkHxYxHHTrGT1l1HtxYNDqSV2z0tax8)
