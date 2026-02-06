"""Generic pytest fixtures for the WiFi Tester instrument."""

import os
import uuid

import pytest

from wifi_tester import WiFiTesterDriver


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
