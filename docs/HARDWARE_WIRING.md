# Hardware Wiring Guide (3-Controller Robot)

This document describes the current wiring for:

1. ESP32-P4 (WebRTC + MQTT + CAN sender)
2. ESP32-C3 (servo controller)
3. ESP32 (robot moving platform controller)

Pin definitions below are taken from current source code:

- `IDF_esp32p4_webrtc/main/main.c`
- `IDF_esp32c3_servo_control/main/app_config.h`
- `IDF_robot_control/main/main.c`
- `IDF_robot_control/main/control/control_callback.h`

## 1) System topology

- ESP32-P4 receives web commands via MQTT/WebRTC and sends CAN commands.
- ESP32-C3 receives CAN commands and drives 2 servos (camera pan/tilt).
- ESP32 robot controller receives CAN commands and drives 4-wheel motor outputs through MCP23017 + motor driver stage.
- All CAN nodes share the same CANH/CANL bus and common GND.

## 2) CAN bus wiring

Use one CAN transceiver per MCU node (for example SN65HVD230 class 3.3V modules, or any equivalent compatible with your board voltage domain).

Required shared lines across all nodes:

- `CANH` (bus high)
- `CANL` (bus low)
- `GND` (common reference)

Termination:

- Use exactly two `120 ohm` terminators, one at each physical end of the CAN trunk.
- Do not place 120 ohm on every node.

## 3) ESP32-P4 wiring

### 3.1 CAN interface (from `IDF_esp32p4_webrtc/main/main.c`)

| Function | ESP32-P4 pin |
|---|---|
| CAN RX | `GPIO26` |
| CAN TX | `GPIO27` |

Connect:

- P4 `GPIO27 (TX)` -> transceiver `TXD`
- P4 `GPIO26 (RX)` <- transceiver `RXD`
- transceiver `CANH/CANL` -> shared CAN bus
- transceiver GND -> system GND

### 3.2 Camera

The default P4 board setup uses onboard camera wiring in the P4 project. Keep board default camera wiring unless you intentionally remap it.

## 4) ESP32-C3 wiring (servo controller)

From `IDF_esp32c3_servo_control/main/app_config.h`:

| Function | ESP32-C3 pin |
|---|---|
| CAN RX | `GPIO5` |
| CAN TX | `GPIO6` |
| Servo LR PWM | `GPIO4` |
| Servo UD PWM | `GPIO3` |

Servo notes:

- Servos should be powered from a stable external `5V` rail.
- Connect servo ground to ESP32-C3 ground (common GND required).
- Do not power servos directly from weak USB rails on dev boards.

## 5) ESP32 robot platform wiring

### 5.1 CAN interface (from `IDF_robot_control/main/main.c`)

| Function | ESP32 pin |
|---|---|
| CAN RX | `GPIO26` |
| CAN TX | `GPIO25` |

### 5.2 MCP23017 I2C interface (from `IDF_robot_control/main/control/control_callback.h`)

| Function | ESP32 pin |
|---|---|
| I2C SDA | `GPIO21` |
| I2C SCL | `GPIO22` |
| MCP23017 address | `0x27` |

### 5.3 Motor PWM outputs (from `control_callback.h`)

| Function | ESP32 pin |
|---|---|
| Motor A PWM | `GPIO16` |
| Motor B PWM | `GPIO17` |
| Motor C PWM | `GPIO18` |
| Motor D PWM | `GPIO19` |

### 5.4 MCP23017 motor logic lines

MCP23017 pins used by firmware:

| MCP23017 pin index | Function |
|---|---|
| `0` | `MOTOR_STBY` |
| `1` | `MOTOR_A_BIN1` |
| `2` | `MOTOR_A_BIN2` |
| `3` | `MOTOR_B_BIN1` |
| `4` | `MOTOR_B_BIN2` |
| `5` | `MOTOR_C_BIN1` |
| `6` | `MOTOR_C_BIN2` |
| `7` | `MOTOR_D_BIN1` |
| `8` | `MOTOR_D_BIN2` |

Connect these MCP23017 outputs to your motor driver direction/standby inputs.

## 6) Power and grounding checklist

- Keep logic rails and motor/servo power rails stable under load.
- Tie all grounds together:
  - ESP32-P4 GND
  - ESP32-C3 GND
  - ESP32 robot controller GND
  - CAN transceiver GND
  - servo/motor power GND
- For long motor wires, prefer:
  - local decoupling near driver and MCU
  - twisted pair or short routing for sensitive signals
  - separated high-current motor wiring from logic/control wiring

## 7) Bring-up order

1. Power only one controller and verify serial logs.
2. Verify CAN transceiver TX/RX wiring on each node.
3. Connect CAN bus with proper two-end 120 ohm termination.
4. Verify `ROOM_ID/status` updates from P4 over MQTT.
5. Verify servo movement on C3 from `SERVO_*` commands.
6. Verify robot movement on ESP32 from `ROBOT_*` commands.
7. Verify speed control (`ROBOT_SPEED`) and stop behavior.
