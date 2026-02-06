"""Serial driver for the WiFi Tester instrument."""

import base64
import json
import logging
import queue
import threading
import time

import serial

logger = logging.getLogger(__name__)


class Response:
    """HTTP response object mimicking requests.Response."""

    def __init__(self, status_code: int, headers: dict, body: bytes):
        self.status_code = status_code
        self.headers = headers
        self._body = body

    @property
    def text(self) -> str:
        return self._body.decode("utf-8", errors="replace")

    def json(self) -> dict:
        return json.loads(self._body)

    @property
    def content(self) -> bytes:
        return self._body


class WiFiTesterError(Exception):
    """Error returned by the WiFi Tester device."""
    pass


class WiFiTesterDriver:
    """Serial driver for the WiFi Tester instrument."""

    def __init__(self, port: str, baudrate: int = 115200):
        self._port = port
        self._baudrate = baudrate
        self._serial: serial.Serial | None = None
        self._event_queue: queue.Queue = queue.Queue()
        self._response_event = threading.Event()
        self._response_data: str | None = None
        self._reader_thread: threading.Thread | None = None
        self._running = False
        self._lock = threading.Lock()

    def open(self) -> None:
        self._serial = serial.serial_for_url(self._port, baudrate=self._baudrate, timeout=0.1)
        self._serial.reset_input_buffer()
        self._running = True
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()
        time.sleep(0.5)  # let device settle after connection

    def close(self) -> None:
        self._running = False
        if self._reader_thread:
            self._reader_thread.join(timeout=2)
            self._reader_thread = None
        if self._serial and self._serial.is_open:
            self._serial.close()
            self._serial = None

    def _reader_loop(self) -> None:
        while self._running:
            try:
                if not self._serial or not self._serial.is_open:
                    break
                line = self._serial.readline()
                if not line:
                    continue
                line = line.decode("utf-8", errors="replace").strip()
                if not line:
                    continue

                logger.debug("RX: %s", line)

                if line.startswith("EVT "):
                    self._handle_event(line)
                elif line.startswith("RSP "):
                    self._response_data = line
                    self._response_event.set()
                else:
                    logger.debug("Unrecognized: %s", line)
            except serial.SerialException:
                if self._running:
                    logger.error("Serial read error")
                break
            except Exception as e:
                logger.error("Reader error: %s", e)

    def _handle_event(self, line: str) -> None:
        parts = line[4:].split(" ", 1)
        event_type = parts[0]
        payload = json.loads(parts[1]) if len(parts) > 1 else {}
        self._event_queue.put({"type": event_type, **payload})

    def _send_command(self, cmd: str, args: dict | None = None, timeout: float = 30) -> dict:
        with self._lock:
            self._response_event.clear()
            self._response_data = None

            if args:
                line = f"CMD {cmd} {json.dumps(args)}\n"
            else:
                line = f"CMD {cmd}\n"

            logger.debug("TX: %s", line.strip())
            self._serial.write(line.encode())
            self._serial.flush()

            if not self._response_event.wait(timeout=timeout):
                raise TimeoutError(f"No response for {cmd} within {timeout}s")

            return self._parse_response(cmd, self._response_data)

    def _parse_response(self, expected_cmd: str, line: str) -> dict:
        # RSP <cmd> <OK|ERR> [json]
        parts = line[4:].split(" ", 2)
        cmd = parts[0]
        status = parts[1] if len(parts) > 1 else "ERR"
        payload_str = parts[2] if len(parts) > 2 else "{}"

        if cmd != expected_cmd:
            logger.warning("Response mismatch: expected %s, got %s", expected_cmd, cmd)

        payload = json.loads(payload_str) if payload_str else {}

        if status == "ERR":
            error_msg = payload.get("error", "Unknown error")
            raise WiFiTesterError(f"{cmd}: {error_msg}")

        return payload

    # --- AP management ---

    def ap_start(self, ssid: str, password: str = "", channel: int = 6) -> dict:
        args = {"ssid": ssid, "channel": channel}
        if password:
            args["pass"] = password
        return self._send_command("AP_START", args)

    def ap_stop(self) -> None:
        self._send_command("AP_STOP")

    def ap_status(self) -> dict:
        return self._send_command("AP_STATUS")

    # --- STA management ---

    def sta_join(self, ssid: str, password: str = "", timeout: int = 15) -> dict:
        args = {"ssid": ssid, "timeout": timeout}
        if password:
            args["pass"] = password
        return self._send_command("STA_JOIN", args, timeout=timeout + 5)

    def sta_leave(self) -> None:
        self._send_command("STA_LEAVE")

    # --- HTTP relay ---

    def http_request(self, method: str, url: str, headers: dict = None,
                     body: bytes = None, timeout: int = 10) -> Response:
        args = {"method": method, "url": url, "timeout": timeout}
        if headers:
            args["headers"] = headers
        if body:
            args["body"] = base64.b64encode(body).decode()

        result = self._send_command("HTTP", args, timeout=timeout + 5)

        resp_body = b""
        if result.get("body"):
            resp_body = base64.b64decode(result["body"])

        resp_headers = result.get("headers", {})
        return Response(result.get("status", 0), resp_headers, resp_body)

    def http_get(self, url: str, **kwargs) -> Response:
        return self.http_request("GET", url, **kwargs)

    def http_post(self, url: str, json_data: dict = None, **kwargs) -> Response:
        body = None
        headers = kwargs.pop("headers", None) or {}
        if json_data is not None:
            body = json.dumps(json_data).encode()
            headers["Content-Type"] = "application/json"
        return self.http_request("POST", url, headers=headers, body=body, **kwargs)

    # --- WiFi scanning ---

    def scan(self) -> list[dict]:
        result = self._send_command("SCAN", timeout=10)
        return result.get("networks", [])

    # --- Events ---

    def wait_for_event(self, event_type: str, timeout: float = 30) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event = self._event_queue.get(timeout=0.5)
                if event.get("type") == event_type:
                    return event
                # Put non-matching events back
                self._event_queue.put(event)
            except queue.Empty:
                continue
        raise TimeoutError(f"No {event_type} event within {timeout}s")

    def wait_for_station(self, timeout: float = 30) -> dict:
        return self.wait_for_event("STA_CONNECT", timeout)

    def drain_events(self) -> list[dict]:
        events = []
        while True:
            try:
                events.append(self._event_queue.get_nowait())
            except queue.Empty:
                break
        return events

    # --- Utility ---

    def ping(self) -> dict:
        return self._send_command("PING")

    def reset(self) -> None:
        try:
            self._send_command("RESET", timeout=2)
        except TimeoutError:
            pass  # device resets before sending full response
        time.sleep(3)  # wait for USB re-enumeration
