"""Self-tests for the WiFi Tester instrument (WT-xxx).

These tests verify the WiFi Tester itself works correctly.
They do NOT require any DUT -- they test the instrument.
"""

import time

import pytest

from wifi_tester_driver import WiFiTesterError


# ============================================================
# WT-1xx: Serial Protocol
# ============================================================

class TestSerialProtocol:
    """WT-1xx: Serial protocol tests."""

    def test_wt100_ping_response(self, wifi_tester):
        """WT-100: PING returns fw_version and uptime."""
        result = wifi_tester.ping()
        assert "fw_version" in result
        assert "uptime" in result
        assert isinstance(result["uptime"], (int, float))

    def test_wt101_unknown_command(self, wifi_tester):
        """WT-101: Unknown command returns ERR."""
        with pytest.raises(WiFiTesterError, match="Unknown command"):
            wifi_tester._send_command("FOOBAR")

    def test_wt102_malformed_json(self, wifi_tester):
        """WT-102: Malformed JSON returns parse error."""
        with pytest.raises(WiFiTesterError):
            wifi_tester._send_command("AP_START", None)
            # Send raw malformed JSON
            wifi_tester._serial.write(b"CMD AP_START {bad json}\n")
            wifi_tester._serial.flush()
            time.sleep(1)

    def test_wt103_missing_required_arg(self, wifi_tester):
        """WT-103: Missing required field returns error."""
        with pytest.raises(WiFiTesterError, match="ssid"):
            wifi_tester._send_command("AP_START", {})


# ============================================================
# WT-2xx: SoftAP Management
# ============================================================

class TestSoftAPManagement:
    """WT-2xx: SoftAP management tests."""

    def test_wt200_start_ap(self, wifi_tester):
        """WT-200: Start AP with valid SSID/pass."""
        result = wifi_tester.ap_start("WT-TEST-200", "password123")
        assert "ip" in result
        assert result["ip"] == "192.168.4.1"
        wifi_tester.ap_stop()

    def test_wt201_start_open_ap(self, wifi_tester):
        """WT-201: Start open AP (no password)."""
        result = wifi_tester.ap_start("WT-TEST-201")
        assert "ip" in result
        wifi_tester.ap_stop()

    def test_wt202_stop_ap(self, wifi_tester):
        """WT-202: Stop AP after starting."""
        wifi_tester.ap_start("WT-TEST-202", "password123")
        wifi_tester.ap_stop()  # should not raise

    def test_wt203_stop_when_not_running(self, wifi_tester):
        """WT-203: Stop when no AP running (idempotent)."""
        wifi_tester.ap_stop()  # should not raise

    def test_wt204_restart_ap_new_config(self, wifi_tester):
        """WT-204: Restart AP with new SSID replaces old one."""
        wifi_tester.ap_start("WT-TEST-204A", "password123")
        result = wifi_tester.ap_start("WT-TEST-204B", "password456")
        assert "ip" in result

        status = wifi_tester.ap_status()
        assert status["ssid"] == "WT-TEST-204B"
        wifi_tester.ap_stop()

    def test_wt205_ap_status_when_running(self, wifi_tester):
        """WT-205: AP_STATUS when running reports correct state."""
        wifi_tester.ap_start("WT-TEST-205", "password123", channel=6)
        status = wifi_tester.ap_status()
        assert status["active"] is True
        assert status["ssid"] == "WT-TEST-205"
        assert status["channel"] == 6
        wifi_tester.ap_stop()

    def test_wt206_ap_status_when_stopped(self, wifi_tester):
        """WT-206: AP_STATUS when stopped reports inactive."""
        wifi_tester.ap_stop()
        status = wifi_tester.ap_status()
        assert status["active"] is False

    def test_wt207_max_ssid_length(self, wifi_tester):
        """WT-207: 32-character SSID works."""
        ssid = "A" * 32
        result = wifi_tester.ap_start(ssid)
        assert "ip" in result
        wifi_tester.ap_stop()

    def test_wt208_channel_selection(self, wifi_tester):
        """WT-208: Channel selection is respected."""
        wifi_tester.ap_start("WT-TEST-208", "password123", channel=11)
        status = wifi_tester.ap_status()
        assert status["channel"] == 11
        wifi_tester.ap_stop()


# ============================================================
# WT-4xx: STA Mode (subset that doesn't need external AP)
# ============================================================

class TestSTAMode:
    """WT-4xx: STA mode tests."""

    def test_wt403_join_nonexistent_network(self, wifi_tester):
        """WT-403: Joining a nonexistent SSID times out."""
        with pytest.raises(WiFiTesterError):
            wifi_tester.sta_join("NONEXISTENT-NETWORK-12345", timeout=5)

    def test_wt405_softap_stops_during_sta(self, wifi_tester):
        """WT-405: SoftAP stops when entering STA mode."""
        wifi_tester.ap_start("WT-TEST-405", "password123")
        try:
            wifi_tester.sta_join("NONEXISTENT-405", timeout=5)
        except WiFiTesterError:
            pass  # expected - network doesn't exist
        status = wifi_tester.ap_status()
        assert status["active"] is False


# ============================================================
# WT-5xx: HTTP Relay (error cases only - no DUT needed)
# ============================================================

class TestHTTPRelay:
    """WT-5xx: HTTP relay tests (error cases)."""

    def test_wt503_connection_refused(self, wifi_tester):
        """WT-503: HTTP to unreachable IP returns error."""
        wifi_tester.ap_start("WT-TEST-503", "password123")
        with pytest.raises(WiFiTesterError):
            wifi_tester.http_get("http://192.168.4.99/nonexistent", timeout=5)
        wifi_tester.ap_stop()


# ============================================================
# WT-6xx: WiFi Scan
# ============================================================

class TestWiFiScan:
    """WT-6xx: WiFi scan tests."""

    def test_wt600_scan_finds_networks(self, wifi_tester):
        """WT-600: Scan returns results (assumes WiFi environment)."""
        networks = wifi_tester.scan()
        assert isinstance(networks, list)
        # In most environments there will be at least one network
        # but we don't assert non-empty since lab environments may differ

    def test_wt601_scan_returns_fields(self, wifi_tester):
        """WT-601: Each scan result has ssid, rssi, auth fields."""
        networks = wifi_tester.scan()
        for net in networks:
            assert "ssid" in net
            assert "rssi" in net
            assert "auth" in net
            assert isinstance(net["rssi"], (int, float))

    def test_wt603_scan_while_ap_running(self, wifi_tester):
        """WT-603: Scan works while AP is running."""
        wifi_tester.ap_start("WT-TEST-603", "password123")
        networks = wifi_tester.scan()
        assert isinstance(networks, list)
        wifi_tester.ap_stop()
