# ESS MQTT Firmware (`ess-mqtt/ess-mqtt.ino`)

This firmware is the MQTT-based ESS version (Blynk removed).

## Folder / sketch naming

The main sketch file name matches the folder name as required:

- Folder: `ess-mqtt/`
- Sketch: `ess-mqtt.ino`

## Credentials and broker config

Create `secrets.h` in the same folder as `ess-mqtt.ino`.

At minimum, define:

```cpp
#define MQTT_HOST "your-broker-host"
#define MQTT_PORT 8883
#define MQTT_USE_TLS 1

// Required when MQTT_USE_TLS == 1
const char AWS_ROOT_CA[] = R"EOF(...ca cert...)EOF";

// Optional depending on broker auth mode
const char DEVICE_CERT[] = R"EOF(...client cert...)EOF";
const char PRIVATE_KEY[] = R"EOF(...private key...)EOF";
```

> Reuse the same credentials strategy used by `smart-pole.ino`.

## Topics

Topic prefix is `ess/update/<deviceId>/`.

- Telemetry data: `ess/update/<deviceId>/data`
- Status / ACK / errors: `ess/update/<deviceId>/status`
- Commands (subscribe): `ess/update/<deviceId>/cmd`

No `device_registration` topic is used in this version.

## Telemetry payload (`/data`)

Published at the configured telemetry interval:

```json
{
  "timestamp": 12345678,
  "deviceID": "ABCDEF123456",
  "battVoltage": 52.1,
  "battCurrent": 10.3,
  "soc": 88,
  "battTemp": 34.2,
  "gridVoltage": 228.5,
  "gridCurrent": 5.2,
  "gridFreq": 50.0,
  "invTemp": 41.2,
  "inverterVoltage": 230.0,
  "inverterCurrent": 4.9,
  "inverterFreq": 50.0,
  "fan1": 1,
  "fan2": 0,
  "fan3": 0,
  "manualOverride": false
}
```

## Status payload (`/status`)

Examples:

- MQTT connected event:

```json
{ "timestamp": 1234, "deviceID": "ABCDEF123456", "event": "mqtt_connected", "result": "ok" }
```

- Generic command ACK / error:

```json
{ "timestamp": 1234, "deviceID": "ABCDEF123456", "event": "command_ack", "result": "ok", "details": "set_charge_mode" }
```

## Commands (`/cmd`)

All commands are JSON with a `command` field.

### 1) Inverter run/stop

```json
{ "command": "set_inverter_run_stop", "value": 0 }
```

Valid values:
- `0` = stop
- `1` = run

### 2) Charge mode

```json
{ "command": "set_charge_mode", "value": 2 }
```

Valid values:
- `0` = normal
- `1` = reserved
- `2` = force charge

### 3) Fan manual override

```json
{ "command": "set_fan_manual", "value": 1 }
```

Valid values:
- `0` = disable manual override (return to automatic logic)
- `1` = enable manual override (force all fans ON)

### 4) Telemetry interval (seconds)

```json
{ "command": "set_telemetry_interval_sec", "value": 300 }
```

Valid values:
- `300` (5 minutes)
- `600` (10 minutes)
- `900` (15 minutes)

### 5) Modbus write single register

```json
{ "command": "modbus_write_single", "reg": 10120, "value": 2 }
```

Validation:
- `reg`: `10000..30000`
- `value`: `0..65535`

### 6) Modbus read single register

```json
{ "command": "modbus_read_single", "reg": 15112 }
```

Validation:
- `reg`: `1..65535`

ACK on `/status` includes `reg` and either:
- `result: "ok"` + `value`
- or `result: "error"`

## Notes

- Wi-Fi profile manager, AP portal, reconnect logic, and RS485 recovery were kept from the original ESS firmware.
- OTA remains enabled when Wi-Fi is connected.
