# WiFi Tester

![Platform](https://img.shields.io/badge/platform-ESP32--C3-blue)
![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v5.x-red)
![Language](https://img.shields.io/badge/firmware-C-yellow)
![Driver](https://img.shields.io/badge/driver-Python-green)
![License](https://img.shields.io/badge/license-MIT-brightgreen)
![Status](https://img.shields.io/badge/status-under%20development-orange)

## The Problem

Testing WiFi behavior on embedded devices is manual and painful. "Power on the device, check if it connects, unplug the router, wait 30 seconds, plug it back in, see if it reconnects, now trigger the captive portal..." Every change requires repeating this by hand.

## The Solution

Plug a $3 ESP32-C3 into your laptop. It becomes a **programmable WiFi access point** you control from Python. Start a network, kill it, change the password, watch devices connect and disconnect -- all from pytest, all automated, all repeatable.

## What It Does

- **Create WiFi networks on demand** -- any SSID, any password, any channel
- **Tear them down instantly** -- simulate router crashes, AP dropouts, network changes
- **Join existing networks** -- connect to a device's captive portal AP to test provisioning flows
- **Relay HTTP requests** -- talk to devices on the test WiFi without your laptop joining the network
- **Report events** -- get notified the moment a device connects or disconnects, with its IP
- **Scan the airwaves** -- verify a device's portal AP appeared (or disappeared)
- Works with **any WiFi device** -- the instrument knows nothing about what you're testing

## How It Works

```
                         USB Serial (115200 baud)
  Your Laptop  <================================>  WiFi Tester
  (pytest)          "start AP" / "relay HTTP"       (ESP32-C3)
                                                        |
                                                      WiFi
                                                   192.168.4.x
                                                        |
                                                   Your Device
                                                  (anything with WiFi)
```

The WiFi Tester sits between your test scripts and your device. Your laptop stays on its own network (internet preserved). All communication with the device goes through serial commands to the WiFi Tester, which relays HTTP over WiFi.

The firmware is a **dumb instrument** -- it has no idea what device you're testing, what endpoints it has, or what "success" means. It just provides WiFi building blocks. Your test scripts have all the intelligence.

## Hardware

- 1x **ESP32-C3 SuperMini** (~$3)
- 1x **USB cable**
- That's it. No wiring to the device under test.

## Quick Start

### Flash the firmware

Requires [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/).

```bash
git clone https://github.com/SensorsIot/Wifi-Tester.git
cd Wifi-Tester
idf.py set-target esp32c3
idf.py build
idf.py flash -p /dev/ttyACM0
```

### Install the Python driver

```bash
pip install git+https://github.com/SensorsIot/Wifi-Tester.git
```

Or add to your project's `requirements.txt`:

```
wifi-tester @ git+https://github.com/SensorsIot/Wifi-Tester.git
```

### Run your first test

```python
from wifi_tester import WiFiTesterDriver

tester = WiFiTesterDriver("/dev/ttyACM0")
tester.open()
tester.ping()  # verify serial link

# Create a WiFi network
tester.ap_start("MY-TEST-NET", "password123")

# Wait for a device to connect
station = tester.wait_for_station(timeout=30)
print(f"Device connected at {station['ip']}")

# Talk to the device via HTTP relay
resp = tester.http_get(f"http://{station['ip']}/api/status")
print(resp.json())

# Simulate AP dropout and recovery
tester.ap_stop()
time.sleep(5)
tester.ap_start("MY-TEST-NET", "password123")
station = tester.wait_for_station(timeout=30)
print(f"Device reconnected at {station['ip']}")

tester.ap_stop()
tester.close()
```

## CLI

The package includes a `wifi-tester` command for interactive use:

```bash
# Interactive REPL
wifi-tester --port /dev/ttyACM0

# Single command
wifi-tester --port /dev/ttyACM0 --command ping
wifi-tester -c "ap_start MY-NET secret123"
wifi-tester -c scan
```

Commands: `ping`, `ap_start`, `ap_stop`, `ap_status`, `sta_join`, `sta_leave`, `scan`, `http`, `reset`.

## Usage Examples

### Test WiFi reconnection after dropout

```python
def test_reconnect_after_dropout(wifi_tester, dut_ip):
    wifi_tester.ap_stop()
    time.sleep(5)
    wifi_tester.ap_start("TEST-NET", "password")
    station = wifi_tester.wait_for_station(timeout=30)
    resp = wifi_tester.http_get(f"http://{station['ip']}/health")
    assert resp.status_code == 200
```

### Test a captive portal (any device)

```python
def test_captive_portal(wifi_tester):
    # Device fails WiFi 3 times and enters portal mode...
    time.sleep(100)

    # Verify the portal AP appeared
    networks = wifi_tester.scan()
    assert any(n["ssid"] == "MyDevice-Setup" for n in networks)

    # Join the portal, submit credentials
    wifi_tester.sta_join("MyDevice-Setup")
    resp = wifi_tester.http_post("http://192.168.4.1/setup",
                                 json_data={"ssid": "TEST-NET", "pass": "secret"})
    assert resp.status_code == 200

    # Leave portal, start our AP, wait for device
    wifi_tester.sta_leave()
    wifi_tester.ap_start("TEST-NET", "secret")
    station = wifi_tester.wait_for_station(timeout=45)
```

### Test invalid credentials

```python
def test_wrong_password(wifi_tester):
    wifi_tester.ap_start("SECURED-NET", "correct-password")
    # Device was provisioned with "wrong-password"...
    # It should NOT connect
    with pytest.raises(TimeoutError):
        wifi_tester.wait_for_station(timeout=35)
```

## Serial Protocol

Line-based ASCII at 115200 baud. Three message types:

```
CMD AP_START {"ssid":"TEST","pass":"secret","channel":6}              # you send
RSP AP_START OK {"ip":"192.168.4.1"}                                  # device replies
EVT STA_CONNECT {"mac":"AA:BB:CC:DD:EE:FF","ip":"192.168.4.2"}       # async event
```

### Commands

| Command | Arguments | Description |
|---------|-----------|-------------|
| `PING` | - | Heartbeat, returns firmware version and uptime |
| `AP_START` | `ssid`, `pass`, `channel` | Start a WiFi access point |
| `AP_STOP` | - | Stop the access point |
| `AP_STATUS` | - | Query AP state and connected stations |
| `STA_JOIN` | `ssid`, `pass`, `timeout` | Join an existing WiFi network |
| `STA_LEAVE` | - | Disconnect from WiFi network |
| `HTTP` | `method`, `url`, `headers`, `body` | Relay an HTTP request (body is base64) |
| `SCAN` | - | List visible WiFi networks |
| `RESET` | - | Restart the WiFi Tester |

### Events

| Event | Payload | When |
|-------|---------|------|
| `STA_CONNECT` | `mac`, `ip` | A device connected to our AP |
| `STA_DISCONNECT` | `mac` | A device disconnected |
| `LOG` | `level`, `msg` | Diagnostic output |

Full protocol specification: **[docs/WiFi-Tester-FSD.md](docs/WiFi-Tester-FSD.md)**

## Project Structure

```
wifi-tester/
├── pyproject.toml                 # Python package configuration
├── src/wifi_tester/               # pip-installable Python driver
│   ├── __init__.py                # Exports WiFiTesterDriver, WiFiTesterError, Response
│   ├── driver.py                  # Serial driver class
│   └── cli.py                     # Interactive CLI (wifi-tester command)
├── tests/                         # Instrument self-tests (pytest)
│   ├── conftest.py                # Generic fixtures (wifi_tester, wifi_network)
│   └── test_instrument.py         # WT-xxx self-tests
├── main/                          # ESP-IDF firmware (C)
│   ├── main.c                     # Entry point and command handlers
│   ├── serial_protocol.c/h        # USB Serial/JTAG I/O, CMD dispatch, RSP formatting
│   ├── wifi_controller.c/h        # softAP and STA management, event callbacks
│   ├── http_relay.c/h             # esp_http_client relay with base64 encoding
│   └── version.h                  # Firmware version
└── docs/
    └── WiFi-Tester-FSD.md         # Functional specification document
```

DUT-specific test scripts belong in the DUT's own repository, not here.

## Configuration

| Environment Variable | Default | Description |
|---------------------|---------|-------------|
| `WIFI_TESTER_PORT` | `/dev/ttyACM0` | Serial port for the WiFi Tester |

## Documentation

- **[Functional Specification](docs/WiFi-Tester-FSD.md)** -- Full protocol spec, firmware architecture, 35 instrument self-test cases, and captive portal testing example

## Status

Under development. Firmware is implemented and flashed. Python test driver and instrument self-tests are implemented.

## License

MIT
