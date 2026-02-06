# WiFi Tester

A programmable WiFi test instrument built on ESP32-C3 with ESP-IDF. Control a WiFi access point over USB serial to automate integration tests for any WiFi-connected embedded device.

## What It Does

```
                        USB Serial
  Dev Machine  <=====================>  WiFi Tester (ESP32-C3)
  (pytest)        Commands + HTTP relay       |
                                            WiFi AP
                                              |
                                         Your Device
```

The WiFi Tester is a **dumb instrument** — it provides WiFi primitives over serial and knows nothing about what device you're testing:

| Command | What it does |
|---------|-------------|
| `AP_START` | Create a WiFi network with any SSID/password |
| `AP_STOP` | Kill the network (simulate AP dropout) |
| `STA_JOIN` | Join an existing WiFi network (e.g., a device's captive portal) |
| `HTTP` | Relay an HTTP request to a device on the WiFi network |
| `SCAN` | List visible WiFi networks |

Your test scripts orchestrate these primitives to test connection, reconnection, captive portals, credential handling, and more.

## Why

WiFi integration tests are usually manual: "power on the device, check if it connects, unplug the router, wait, plug it back in..." This tool lets you automate all of that from pytest.

## Hardware

- 1x **ESP32-C3 SuperMini** (~$3) flashed with this firmware
- 1x **USB cable** to your dev machine
- That's it. No wiring to the device under test.

## Quick Example

```python
# Start a WiFi network
wifi_tester.ap_start("MY-TEST-NET", "password123")

# Wait for a device to connect
station = wifi_tester.wait_for_station(timeout=30)
print(f"Device connected at {station['ip']}")

# Talk to the device through the WiFi Tester
resp = wifi_tester.http_get(f"http://{station['ip']}/api/status")
assert resp.status_code == 200

# Simulate AP dropout
wifi_tester.ap_stop()
time.sleep(5)
wifi_tester.ap_start("MY-TEST-NET", "password123")

# Device should reconnect
station = wifi_tester.wait_for_station(timeout=30)
```

## Serial Protocol

Line-based ASCII protocol at 115200 baud. Three message types:

```
CMD AP_START {"ssid":"TEST","pass":"secret","channel":6}    # host → device
RSP AP_START OK {"ip":"192.168.4.1"}                        # device → host
EVT STA_CONNECT {"mac":"AA:BB:CC:DD:EE:FF","ip":"192.168.4.2"}  # async event
```

HTTP responses are base64-encoded to keep everything on one line. Full protocol spec in [docs/WiFi-Tester-FSD.md](docs/WiFi-Tester-FSD.md).

## Project Structure

```
wifi-tester/
├── main/                       # ESP-IDF firmware
│   ├── main.c                  # Entry point, main loop
│   ├── serial_protocol.c/h     # Command parsing
│   ├── wifi_controller.c/h     # AP/STA management
│   └── http_relay.c/h          # HTTP client relay
├── pytest/                     # Python test driver
│   ├── wifi_tester_driver.py   # Serial driver class
│   ├── conftest.py             # Generic pytest fixtures
│   └── test_instrument.py      # Self-tests for the instrument
└── docs/
    └── WiFi-Tester-FSD.md      # Full specification
```

## Building

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/) v5.x.

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash -p /dev/ttyACM0
```

## Python Driver

```bash
pip install pyserial
```

```python
from wifi_tester_driver import WiFiTesterDriver

tester = WiFiTesterDriver("/dev/ttyACM0")
tester.open()
tester.ping()  # verify connection

tester.ap_start("TEST-NET", "password")
# ... run your tests ...
tester.ap_stop()
tester.close()
```

## Documentation

- **[Functional Specification](docs/WiFi-Tester-FSD.md)** — Full protocol spec, firmware architecture, test cases, and usage examples

## Status

Under development. The FSD is complete; firmware implementation is next.

## License

MIT
