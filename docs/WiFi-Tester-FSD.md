# WiFi Tester - Functional Specification Document

**Version:** 1.1
**Date:** February 2026
**Platform:** ESP32-C3 (ESP-IDF)
**Repository:** https://github.com/SensorsIot/Wifi-Tester
**Author:** Andreas Spiess / Claude Code

---

## 1. System Overview

### 1.1 Purpose

The WiFi Tester is a **generic WiFi test instrument** — an ESP32-C3 that provides programmable WiFi access point and station capabilities over a USB serial interface. It has no knowledge of any device under test (DUT). It simply provides WiFi primitives that a test runner can orchestrate.

Think of it as a bench instrument: it creates WiFi networks, tears them down, joins existing networks, relays HTTP requests, and reports WiFi events. What you do with those capabilities is up to the test scripts.

### 1.2 Problem Statement

WiFi integration tests for embedded devices are typically manual: a human checks whether a device connects, recovers from dropouts, and handles captive portal flows. This is slow, unrepeatable, and error-prone.

A programmable WiFi AP controlled over serial enables full automation of:
- Connection and reconnection behavior
- Credential handling (valid, invalid, changed)
- Captive portal flows (any device's portal, not just one specific implementation)
- Network service verification through HTTP relay

### 1.3 Design Principle: Dumb Instrument

The firmware is deliberately **dumb** — it knows nothing about:
- What DUT is being tested
- What HTTP endpoints the DUT exposes
- What captive portal SSID the DUT broadcasts
- What credentials the DUT expects
- What "success" or "failure" looks like

All DUT-specific intelligence lives in the **test scripts** (pytest). The firmware just provides building blocks:

| Firmware provides | Test scripts decide |
|-------------------|-------------------|
| Start/stop a WiFi AP | What SSID/password to use |
| Join a WiFi network as STA | Which network to join (e.g., a DUT's captive portal) |
| Relay HTTP requests over WiFi | What URL to hit, what payload to send, what response means |
| Report station connect/disconnect | Whether the DUT connected in time, what IP it got |
| Scan for visible networks | Whether a specific SSID appeared (e.g., a portal AP) |

This makes the WiFi Tester reusable across any project with WiFi-connected devices.

---

## 2. Hardware Configuration

### 2.1 Test AP Hardware

- **MCU**: ESP32-C3 SuperMini
- **Connection**: USB serial to dev machine (115200 baud)
- **No other connections required** (no GPIO wiring to DUT)

### 2.2 Bench Setup

```
                           USB Serial
    Dev Machine  <========================>  WiFi Tester (ESP32-C3)
    (pytest)          Commands + HTTP relay       |
                                               WiFi softAP / STA
                                                  |
                                             WiFi STA client
                                                  |
                                              DUT (any device)
```

- The dev machine connects to the WiFi Tester **only** via USB serial
- The WiFi Tester creates or joins WiFi networks as instructed
- The dev machine stays on its own network (internet access preserved)
- All DUT communication goes through serial-relayed HTTP

### 2.3 Network Addressing

| Device | Interface | IP Address |
|--------|-----------|------------|
| WiFi Tester | softAP | 192.168.4.1 (ESP-IDF default) |
| DUT | STA (DHCP) | 192.168.4.x (reported via event) |
| Dev machine | LAN/WiFi | Own network (not on test WiFi) |

The WiFi Tester runs the ESP32's built-in DHCP server on its softAP interface. The actual IP assigned to any connecting station is reported via the `STA_CONNECT` event — the test scripts should use that reported IP, never hardcode it.

---

## 3. Firmware Architecture

### 3.1 Overview

The firmware is a single-purpose ESP-IDF application with three components:

```
┌────────────────────────────────────┐
│           Main Task                │
│  ┌──────────────────────────────┐  │
│  │   Serial Command Parser      │  │
│  │   - Read line from UART      │  │
│  │   - Parse CMD + JSON args    │  │
│  │   - Dispatch to handler      │  │
│  │   - Send RSP line            │  │
│  └──────────────────────────────┘  │
│  ┌──────────────────────────────┐  │
│  │   WiFi Controller            │  │
│  │   - softAP start/stop        │  │
│  │   - STA join/leave           │  │
│  │   - Event callbacks → EVT    │  │
│  │   - Station IP tracking      │  │
│  └──────────────────────────────┘  │
│  ┌──────────────────────────────┐  │
│  │   HTTP Relay Client          │  │
│  │   - esp_http_client          │  │
│  │   - Request from serial args │  │
│  │   - Base64-encode response   │  │
│  │   - Return via serial        │  │
│  └──────────────────────────────┘  │
└────────────────────────────────────┘
```

### 3.2 Dependencies

All built into ESP-IDF — no external libraries:

| Component | ESP-IDF Module |
|-----------|---------------|
| WiFi softAP/STA | `esp_wifi` |
| DHCP server | `esp_netif` (built into softAP) |
| HTTP client | `esp_http_client` |
| JSON parsing | `cJSON` (bundled) |
| Base64 encoding | `mbedtls/base64.h` (bundled) |
| UART serial | `driver/uart.h` |

---

## 4. Serial Command Protocol

### 4.1 Transport

- **Baud rate**: 115200
- **Line termination**: `\n` (LF only)
- **Encoding**: ASCII text, one message per line
- **Max line length**: 4096 bytes (accommodates base64-encoded HTTP responses)
- **Flow control**: None

### 4.2 Message Types

| Direction | Prefix | Format |
|-----------|--------|--------|
| Host → Device | `CMD` | `CMD <command> [JSON arguments]` |
| Device → Host | `RSP` | `RSP <command> <OK\|ERR> [JSON payload]` |
| Device → Host (async) | `EVT` | `EVT <event_type> [JSON payload]` |

Commands are synchronous: the host sends a `CMD`, the device processes it, and sends back exactly one `RSP` for that command. Asynchronous `EVT` messages may arrive at any time, interleaved between command/response pairs.

### 4.3 Commands

#### `PING` — Heartbeat

```
CMD PING
RSP PING OK {"fw_version":"1.0.0","uptime":12345}
```

Returns firmware version and uptime in milliseconds. Used to verify the serial link is alive.

#### `AP_START` — Start softAP

```
CMD AP_START {"ssid":"MY-TEST-NET","pass":"secret123","channel":6}
RSP AP_START OK {"ip":"192.168.4.1"}
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `ssid` | yes | - | AP SSID (1-32 characters) |
| `pass` | no | `""` | Password. Empty = open network. Min 8 chars for WPA2. |
| `channel` | no | `6` | WiFi channel (1-13) |

If the AP is already running, it is stopped first and restarted with the new configuration. Returns the AP's IP address.

#### `AP_STOP` — Stop softAP

```
CMD AP_STOP
RSP AP_STOP OK
```

Disconnects all stations and stops the AP. Idempotent — returns OK even if no AP is running.

#### `AP_STATUS` — Query AP state

```
CMD AP_STATUS
RSP AP_STATUS OK {"active":true,"ssid":"MY-TEST-NET","channel":6,"stations":[{"mac":"AA:BB:CC:DD:EE:FF","ip":"192.168.4.2"}]}
```

Returns current AP state and list of connected stations with MAC addresses and DHCP-assigned IPs.

#### `STA_JOIN` — Join a WiFi network as station

```
CMD STA_JOIN {"ssid":"SomeNetwork","pass":"password","timeout":15}
RSP STA_JOIN OK {"ip":"192.168.4.2","gateway":"192.168.4.1"}
RSP STA_JOIN ERR {"error":"Connection timeout"}
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `ssid` | yes | - | Network SSID to join |
| `pass` | no | `""` | Password |
| `timeout` | no | `15` | Connection timeout in seconds |

The softAP is stopped while in STA mode (ESP32-C3 single radio — STA+AP shares one channel, which would constrain testing). When in STA mode, the HTTP relay routes through the STA connection.

#### `STA_LEAVE` — Disconnect from WiFi network

```
CMD STA_LEAVE
RSP STA_LEAVE OK
```

Disconnects from the STA network. Does not automatically restart the softAP — use `AP_START` explicitly.

#### `HTTP` — Relay an HTTP request

```
CMD HTTP {"method":"GET","url":"http://192.168.4.2/some/endpoint"}
RSP HTTP OK {"status":200,"headers":{"content-type":"application/json"},"body":"eyJrZXkiOiJ2YWx1ZSJ9"}
```

```
CMD HTTP {"method":"POST","url":"http://192.168.4.2/some/endpoint","headers":{"Content-Type":"application/json"},"body":"eyJmb28iOiJiYXIifQ=="}
RSP HTTP OK {"status":200,"headers":{},"body":"eyJvayI6dHJ1ZX0="}
```

| Argument | Required | Default | Description |
|----------|----------|---------|-------------|
| `method` | yes | - | HTTP method: GET, POST, PUT, DELETE |
| `url` | yes | - | Full URL including `http://` and IP |
| `headers` | no | `{}` | Request headers as key-value object |
| `body` | no | `""` | Request body, **base64-encoded** |
| `timeout` | no | `10` | Request timeout in seconds |

The response body is always **base64-encoded** to keep the serial protocol line-safe (HTTP responses may contain newlines, quotes, or binary data).

On connection failure:
```
RSP HTTP ERR {"error":"Connection refused","code":-1}
```

The firmware does not interpret the HTTP request or response in any way. It is a transparent relay.

#### `SCAN` — Scan for visible WiFi networks

```
CMD SCAN
RSP SCAN OK {"networks":[{"ssid":"Network1","rssi":-45,"auth":"WPA2"},{"ssid":"OpenNet","rssi":-72,"auth":"OPEN"}]}
```

Performs an active WiFi scan and returns all visible networks with SSID, RSSI, and auth type. The firmware does not filter or interpret the results.

Auth types: `OPEN`, `WEP`, `WPA`, `WPA2`, `WPA3`, `WPA2_ENTERPRISE`, `UNKNOWN`.

#### `RESET` — Restart the WiFi Tester

```
CMD RESET
RSP RESET OK
```

The device restarts. USB serial re-enumerates.

### 4.4 Asynchronous Events

Events are pushed from the device at any time.

#### `STA_CONNECT` — A station connected to our softAP

```
EVT STA_CONNECT {"mac":"AA:BB:CC:DD:EE:FF","ip":"192.168.4.2"}
```

Fired when any WiFi station connects and receives a DHCP lease. The `ip` field is the DHCP-assigned address.

#### `STA_DISCONNECT` — A station disconnected from our softAP

```
EVT STA_DISCONNECT {"mac":"AA:BB:CC:DD:EE:FF"}
```

#### `LOG` — Diagnostic log message

```
EVT LOG {"level":"info","msg":"AP started on channel 6"}
```

Optional diagnostic output for debugging the WiFi Tester itself. Levels: `error`, `warn`, `info`, `debug`.

---

## 5. Python Test Driver

### 5.1 Serial Driver Class

The `WiFiTesterDriver` class wraps serial communication and provides high-level Python methods. It is **DUT-agnostic** — it knows nothing about what device is being tested.

```python
class WiFiTesterDriver:
    """Serial driver for the WiFi Tester instrument."""

    def __init__(self, port: str, baudrate: int = 115200)
    def open(self) -> None
    def close(self) -> None

    # AP management
    def ap_start(self, ssid: str, password: str = "", channel: int = 6) -> dict
    def ap_stop(self) -> None
    def ap_status(self) -> dict

    # STA management
    def sta_join(self, ssid: str, password: str = "", timeout: int = 15) -> dict
    def sta_leave(self) -> None

    # HTTP relay (transparent — any URL, any method)
    def http_request(self, method: str, url: str, headers: dict = None,
                     body: bytes = None, timeout: int = 10) -> Response
    def http_get(self, url: str, **kwargs) -> Response
    def http_post(self, url: str, json: dict = None, **kwargs) -> Response

    # WiFi scanning
    def scan(self) -> list[dict]

    # Events
    def wait_for_event(self, event_type: str, timeout: float = 30) -> dict
    def wait_for_station(self, timeout: float = 30) -> dict  # shortcut for STA_CONNECT
    def drain_events(self) -> list[dict]  # return and clear queued events

    # Utility
    def ping(self) -> dict
    def reset(self) -> None
```

The `Response` object mimics `requests.Response`:
- `response.status_code` — HTTP status code
- `response.json()` — parse body as JSON
- `response.text` — body as string
- `response.headers` — response headers dict

### 5.2 Event Handling

The driver runs a background thread that reads serial lines continuously:
- Lines starting with `EVT` are pushed to a thread-safe event queue
- Lines starting with `RSP` are matched to the pending command via a threading event
- Unrecognized lines are logged as diagnostics

This allows asynchronous events (station connect/disconnect) to arrive between commands without being lost.

### 5.3 Pytest Fixtures (Generic)

These fixtures provide reusable WiFi test building blocks. They are DUT-agnostic.

```python
@pytest.fixture(scope="session")
def wifi_tester():
    """Session-scoped connection to the WiFi Tester instrument."""
    port = os.environ.get("WIFI_TESTER_PORT", "/dev/ttyACM0")
    driver = WiFiTesterDriver(port)
    driver.open()
    driver.ping()
    yield driver
    driver.ap_stop()
    driver.close()

@pytest.fixture
def wifi_network(wifi_tester):
    """Start a fresh AP for this test, stop on teardown."""
    ssid = f"TEST-{uuid.uuid4().hex[:6].upper()}"
    password = "testpass123"
    wifi_tester.ap_start(ssid, password)
    yield {"ssid": ssid, "password": password, "ap_ip": "192.168.4.1"}
    wifi_tester.ap_stop()
```

DUT-specific fixtures (provisioning, credential restore, captive portal flows) are written per-project in the DUT's test suite, not in the WiFi Tester repo.

---

## 6. Test Cases for WiFi Tester Firmware

These tests verify the **WiFi Tester itself** works correctly. They do NOT require any DUT — they test the instrument.

### 6.1 Serial Protocol (WT-1xx)

| ID | Test Name | Steps | Expected Result |
|----|-----------|-------|-----------------|
| WT-100 | Ping response | Send `CMD PING` | `RSP PING OK` with `fw_version` and `uptime` fields |
| WT-101 | Unknown command | Send `CMD FOOBAR` | `RSP FOOBAR ERR {"error":"Unknown command"}` |
| WT-102 | Malformed JSON | Send `CMD AP_START {bad json}` | `RSP AP_START ERR` with parse error |
| WT-103 | Missing required arg | Send `CMD AP_START {}` (no ssid) | `RSP AP_START ERR` with missing field error |
| WT-104 | Command while busy | Send two commands without waiting for RSP | Second command queued or error (not crash) |

### 6.2 SoftAP Management (WT-2xx)

| ID | Test Name | Steps | Expected Result |
|----|-----------|-------|-----------------|
| WT-200 | Start AP | `AP_START` with valid SSID/pass | `RSP OK`; `SCAN` from another device sees the SSID |
| WT-201 | Start open AP | `AP_START` with empty password | `RSP OK`; AP is open (no auth) |
| WT-202 | Stop AP | `AP_START` then `AP_STOP` | `RSP OK`; SSID no longer visible |
| WT-203 | Stop when not running | `AP_STOP` without prior start | `RSP OK` (idempotent) |
| WT-204 | Restart AP with new config | `AP_START` SSID-A, then `AP_START` SSID-B | Second start succeeds; only SSID-B visible |
| WT-205 | AP status when running | `AP_START` then `AP_STATUS` | Reports `active: true`, correct SSID and channel |
| WT-206 | AP status when stopped | `AP_STATUS` without AP | Reports `active: false` |
| WT-207 | Max SSID length (32 chars) | `AP_START` with 32-char SSID | `RSP OK` |
| WT-208 | Channel selection | `AP_START` with `channel: 11` | `AP_STATUS` shows channel 11 |

### 6.3 Station Connect/Disconnect Events (WT-3xx)

These tests require a DUT or any WiFi device to connect.

| ID | Test Name | Steps | Expected Result |
|----|-----------|-------|-----------------|
| WT-300 | Station connect event | Start AP, DUT connects | `EVT STA_CONNECT` with MAC and IP |
| WT-301 | Station disconnect event | DUT disconnects | `EVT STA_DISCONNECT` with MAC |
| WT-302 | Station in AP_STATUS | DUT connected, query `AP_STATUS` | Station listed with MAC and IP |
| WT-303 | IP matches event | Compare `STA_CONNECT` IP with `AP_STATUS` | IPs match |

### 6.4 STA Mode (WT-4xx)

| ID | Test Name | Steps | Expected Result |
|----|-----------|-------|-----------------|
| WT-400 | Join open network | Start AP on another device, `STA_JOIN` | `RSP OK` with assigned IP |
| WT-401 | Join WPA2 network | `STA_JOIN` with correct password | `RSP OK` |
| WT-402 | Join with wrong password | `STA_JOIN` with wrong password | `RSP ERR` with timeout/auth error |
| WT-403 | Join nonexistent network | `STA_JOIN` with fake SSID | `RSP ERR` with timeout |
| WT-404 | Leave STA | `STA_JOIN` then `STA_LEAVE` | `RSP OK`; disconnected |
| WT-405 | SoftAP stops during STA mode | `AP_START`, then `STA_JOIN` | AP is stopped; `AP_STATUS` shows `active: false` |

### 6.5 HTTP Relay (WT-5xx)

| ID | Test Name | Steps | Expected Result |
|----|-----------|-------|-----------------|
| WT-500 | GET request | Start AP, DUT connected (with HTTP server), relay `GET /` | `RSP HTTP OK` with status 200 and base64 body |
| WT-501 | POST request with body | Relay POST with base64-encoded JSON body | `RSP HTTP OK` with response |
| WT-502 | Custom headers | Relay GET with `Authorization` header | Header forwarded; DUT sees it |
| WT-503 | Connection refused | Relay HTTP to non-existent IP | `RSP HTTP ERR` |
| WT-504 | Request timeout | Relay HTTP to a device that doesn't respond | `RSP HTTP ERR` after timeout |
| WT-505 | Large response | DUT returns large HTML page | Full response base64-encoded in RSP (up to 4KB) |
| WT-506 | HTTP via STA mode | `STA_JOIN` network, relay HTTP to device on that network | `RSP HTTP OK` |

### 6.6 WiFi Scan (WT-6xx)

| ID | Test Name | Steps | Expected Result |
|----|-----------|-------|-----------------|
| WT-600 | Scan finds networks | `SCAN` in environment with WiFi networks | `RSP SCAN OK` with non-empty `networks` array |
| WT-601 | Scan returns SSID/RSSI/auth | `SCAN` | Each entry has `ssid`, `rssi` (negative int), `auth` fields |
| WT-602 | Scan finds our own AP | `AP_START`, then `SCAN` | Our SSID NOT in results (ESP32 doesn't see its own AP in scan) |
| WT-603 | Scan while AP running | `AP_START`, then `SCAN` | Scan completes without stopping AP |

---

## 7. Example: Testing a Captive Portal (Any Device)

This section shows how test scripts use the WiFi Tester to test a DUT's captive portal. The firmware doesn't know any of this — it's all pytest logic.

### 7.1 Scenario

A DUT enters captive portal mode when WiFi fails 3 times. It broadcasts an AP called "MyDevice-Setup". A user connects to it, opens a web page, and enters WiFi credentials.

### 7.2 Test Flow

```python
def test_captive_portal_provisioning(wifi_tester):
    """Test the DUT's full captive portal provisioning flow."""
    target_ssid = "TEST-TARGET"
    target_pass = "secret123"
    portal_ssid = "MyDevice-Setup"  # DUT-specific knowledge

    # 1. Provision DUT with our SSID, but don't start AP yet
    #    (DUT is currently on production network)
    requests.post(f"http://{DUT_IP}/api/wifi",
                  json={"ssid": target_ssid, "password": target_pass})

    # 2. DUT reboots and fails WiFi 3 times (~90s)
    #    (because our AP isn't running)
    time.sleep(100)

    # 3. Verify DUT's portal AP appeared
    result = wifi_tester.scan()
    assert any(n["ssid"] == portal_ssid for n in result["networks"])

    # 4. Join the DUT's portal AP
    wifi_tester.sta_join(portal_ssid)

    # 5. Verify portal page is served
    resp = wifi_tester.http_get("http://192.168.4.1/")
    assert resp.status_code == 200
    assert "WiFi" in resp.text  # some portal content

    # 6. Submit credentials through the portal
    wifi_tester.http_post("http://192.168.4.1/api/wifi",
                          json={"ssid": target_ssid, "password": target_pass})

    # 7. Leave portal, start our AP
    wifi_tester.sta_leave()
    wifi_tester.ap_start(target_ssid, target_pass)

    # 8. Wait for DUT to connect with new credentials
    station = wifi_tester.wait_for_station(timeout=45)
    assert station["ip"].startswith("192.168.4.")

    # 9. Verify DUT is operational
    resp = wifi_tester.http_get(f"http://{station['ip']}/api/status")
    assert resp.status_code == 200
```

The same WiFi Tester hardware + firmware works for any device — only the test script changes (portal SSID, endpoints, payload format).

---

## 8. Timing Reference

| Operation | Duration | Notes |
|-----------|----------|-------|
| WiFi Tester starts softAP | < 1s | `esp_wifi_start()` is fast |
| WiFi Tester joins STA network | 1-10s | Depends on AP, auth type |
| DHCP lease assignment | < 2s | Built-in ESP32 DHCP |
| Serial HTTP relay round-trip | 100-500ms | Small payloads at 115200 baud |
| WiFi scan | 2-3s | Active scan, all channels |
| USB serial reconnect after reset | 1-3s | CDC re-enumeration |

---

## 9. File Structure

```
wifi-tester/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                  # App entry, main loop
│   ├── serial_protocol.c/h     # UART line parsing, CMD dispatch, RSP formatting
│   ├── wifi_controller.c/h     # softAP/STA management, event callbacks
│   ├── http_relay.c/h          # esp_http_client relay, base64 encode/decode
│   └── version.h               # FW_VERSION define
├── pytest/
│   ├── wifi_tester_driver.py   # WiFiTesterDriver serial class
│   ├── conftest.py             # Generic fixtures: wifi_tester, wifi_network
│   └── test_instrument.py      # WT-xxx self-tests for the instrument itself
└── docs/
    └── WiFi-Tester-FSD.md      # This document
```

DUT-specific test scripts live in the **DUT's repository**, not here. For example, the Modbus Proxy WiFi tests would live in `Modbus_Proxy/test/wifi/` and import `wifi_tester_driver` as a dependency.

---

## 10. Configuration

Environment variables for pytest:

| Variable | Default | Description |
|----------|---------|-------------|
| `WIFI_TESTER_PORT` | `/dev/ttyACM0` | Serial port for the WiFi Tester |

That's it. No DUT configuration here — that belongs in the DUT's test suite.

---

## 11. Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-02-06 | Initial specification (DUT-coupled design) |
| 1.1 | 2026-02-06 | Redesigned as generic "dumb instrument"; all DUT logic moved to pytest |
