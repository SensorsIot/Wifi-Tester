"""Interactive CLI for the WiFi Tester instrument."""

import argparse
import json
import logging
import shlex
import sys
import threading

from wifi_tester.driver import WiFiTesterDriver, WiFiTesterError


def _event_printer(driver: WiFiTesterDriver) -> None:
    """Background thread that prints async events."""
    while driver._running:
        try:
            event = driver._event_queue.get(timeout=0.5)
            print(f"\n  << EVT {event.get('type', '?')} {json.dumps(event)} >>")
            print("wifi-tester> ", end="", flush=True)
        except Exception:
            continue


def _run_command(driver: WiFiTesterDriver, parts: list[str]) -> None:
    """Execute a single CLI command."""
    cmd = parts[0].lower()
    args = parts[1:]

    if cmd == "ping":
        result = driver.ping()
        print(json.dumps(result, indent=2))

    elif cmd == "ap_start":
        ssid = args[0] if args else "WiFiTester"
        password = args[1] if len(args) > 1 else ""
        channel = int(args[2]) if len(args) > 2 else 6
        result = driver.ap_start(ssid, password, channel)
        print(json.dumps(result, indent=2))

    elif cmd == "ap_stop":
        driver.ap_stop()
        print("OK")

    elif cmd == "ap_status":
        result = driver.ap_status()
        print(json.dumps(result, indent=2))

    elif cmd == "sta_join":
        if not args:
            print("Usage: sta_join <ssid> [password] [timeout]")
            return
        ssid = args[0]
        password = args[1] if len(args) > 1 else ""
        timeout = int(args[2]) if len(args) > 2 else 15
        result = driver.sta_join(ssid, password, timeout)
        print(json.dumps(result, indent=2))

    elif cmd == "sta_leave":
        driver.sta_leave()
        print("OK")

    elif cmd == "scan":
        networks = driver.scan()
        for net in networks:
            print(f"  {net['ssid']:32s}  {net['rssi']:4d} dBm  {net['auth']}")
        if not networks:
            print("  (no networks found)")

    elif cmd == "http":
        if len(args) < 2:
            print("Usage: http <method> <url>")
            return
        method = args[0].upper()
        url = args[1]
        resp = driver.http_request(method, url)
        print(f"  Status: {resp.status_code}")
        print(f"  Body:   {resp.text[:200]}")

    elif cmd == "reset":
        driver.reset()
        print("Device reset. Reconnecting...")
        driver.open()
        print("OK")

    elif cmd in ("help", "?"):
        print("Commands:")
        print("  ping                          - Check connection")
        print("  ap_start [ssid] [pass] [ch]   - Start WiFi AP")
        print("  ap_stop                       - Stop WiFi AP")
        print("  ap_status                     - Query AP state")
        print("  sta_join <ssid> [pass] [tout]  - Join WiFi network")
        print("  sta_leave                     - Leave WiFi network")
        print("  scan                          - Scan for networks")
        print("  http <method> <url>           - HTTP request via device")
        print("  reset                         - Restart device")
        print("  quit                          - Exit")

    elif cmd in ("quit", "exit"):
        raise SystemExit(0)

    else:
        print(f"Unknown command: {cmd}. Type 'help' for usage.")


def main() -> None:
    parser = argparse.ArgumentParser(description="WiFi Tester CLI")
    parser.add_argument("--port", default="/dev/ttyACM0",
                        help="Serial port (default: /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("--command", "-c",
                        help="Run a single command and exit")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Enable debug logging")
    args = parser.parse_args()

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)

    driver = WiFiTesterDriver(args.port, args.baud)
    try:
        driver.open()
    except Exception as e:
        print(f"Failed to open {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        if args.command:
            parts = shlex.split(args.command)
            _run_command(driver, parts)
            return

        # Start event printer for interactive mode
        evt_thread = threading.Thread(target=_event_printer, args=(driver,), daemon=True)
        evt_thread.start()

        print(f"WiFi Tester CLI on {args.port}. Type 'help' for commands.")
        while True:
            try:
                line = input("wifi-tester> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not line:
                continue
            try:
                parts = shlex.split(line)
                _run_command(driver, parts)
            except WiFiTesterError as e:
                print(f"Device error: {e}")
            except TimeoutError as e:
                print(f"Timeout: {e}")
            except SystemExit:
                break
    finally:
        driver.close()


if __name__ == "__main__":
    main()
