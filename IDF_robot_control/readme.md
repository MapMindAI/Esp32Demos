







### Esp32C3 **Pin Mappings**

| **Pin Name** | **GPIO** | **Function**                           |
|--------------|----------|----------------------------------------|
| 0            | GPIO0    | ADC1, PWM                             |
| 1            | GPIO1    | ADC1, PWM                             |
| 2            | GPIO2    | ADC1, Strapping Pin (Boot Mode)        |
| 3            | GPIO3    | ADC1, PWM                             |
| 4            | GPIO4    | JTAG, ADC1                            |
| 5            | GPIO5    | JTAG                                  |
| 8            | GPIO8    | Status LED (inverted), Strapping Pin  |
| 9            | GPIO9    | BOOT Button, Strapping Pin            |
| 10           | GPIO10   | PWM                                   |
| 20           | GPIO20   | General-purpose I/O                   |
| 21           | GPIO21   | General-purpose I/O                   |



## Troubleshooting: ESP32-WROOM-32 CH340 detected but no `/dev/ttyUSB0`

When using an ESP32-WROOM-32 development board with a CH340 USB-to-serial chip, Linux may detect the USB device but fail to keep the serial port available.

The issue is caused by brltty, a Linux service for Braille display support. On some systems, brltty incorrectly claims the CH340 USB serial device, which disconnects the ch341 driver and removes /dev/ttyUSB0.

Fix by remove brltty : `sudo apt remove brltty`
