# esp32p4 h.264 rtsp Demo

```
source $IDF_PATH/export.sh
```

receive the stream with :
```
ffplay -fflags nobuffer -flags low_delay -analyzeduration 1000000 rtsp://192.168.19.80:8554
```

Using [ESP32-P4-Module Dev Board](https://www.waveshare.com/esp32-p4-module-dev-kit.htm?srsltid=AfmBOoqSfmLctnNxVbsyf52gsBkHxYxHHTrGT1l1HtxYNDqSV2z0tax8)
