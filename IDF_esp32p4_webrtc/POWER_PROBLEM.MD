
## ESP32-P4 Battery Power Instability Issue

### 1. Symptom

When powering the ESP32-P4 development board from a lithium battery through 5V, the board powers on, but the WebRTC demo does not start correctly. The system appears to stop during initialization.

When powering the same board from a PC USB port, WebRTC starts and runs normally.

### 2. Diagnosis

By measuring the voltage between the ESP32-P4 board's 5V and GND pins, we found that the 5V rail becomes unstable when Wi-Fi, camera, WebRTC, and related modules start.

The voltage briefly dropped to around:

```text
4.0V ~ 4.1V
````

This indicates that the issue is not the average current rating, but the transient stability of the battery or boost converter during startup.

### 3. Root Cause

The ESP32-P4 WebRTC demo starts several high-load modules, such as:

* ESP32-C6 / Wi-Fi
* Camera
* WebRTC streaming
* PSRAM and other peripherals

These modules can create short current spikes during startup. If the 5V power supply cannot respond quickly enough, the voltage drops and the system may hang or fail to complete initialization.

### 4. Solution

Use a more stable 5V power supply path:

* Use a higher-quality 5V boost converter or regulator
* Use a battery power module with better transient current capability
* Use short and thick power wires
* Avoid long Dupont wires or breadboards for power delivery
* Add a 470uF ~ 1000uF capacitor near the ESP32-P4 5V and GND input
* Do not power servos, motors, or other high-current peripherals through the ESP32-P4 board
* Make sure all external power supplies and the ESP32-P4 share a common ground

Recommended capacitor connection:

```text
Capacitor positive  -> ESP32-P4 5V
Capacitor negative  -> ESP32-P4 GND
```

### 5. Conclusion

If the ESP32-P4 works from USB power but fails to start WebRTC from a 5V lithium battery supply, first measure the voltage directly on the board's 5V input.

In this case, the voltage dropped to around 4.0V ~ 4.1V during WebRTC startup, so the issue was caused by unstable transient power delivery. The fix is to use a more stable 5V power module, improve the power wiring, and add bulk capacitance near the board input.
