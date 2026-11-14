"""MCP server exposing the probe and LAN checks as tools for AI agents."""

from __future__ import annotations

from mcp.server.mcpserver import MCPServer

from . import analysis, lan
from .device import Probe


def serve(port: str | None = None) -> None:
    server = MCPServer("matter-debugger")

    def run(cmd: str, **args):
        # Open per call so the probe can be unplugged and moved between calls.
        with Probe(port) as probe:
            return probe.call(cmd, **args)

    @server.tool()
    def probe_info() -> dict:
        """Firmware version, chip and radios ("capabilities") of the connected probe."""
        return run("info")

    @server.tool()
    def survey(label: str = "") -> dict:
        """Scan Thread, Wi-Fi and BLE where the probe is now. Returns raw scans plus
        "findings" (level/title/detail). Takes about 20 s. Ask the person to move the
        probe to the room in question first; `label` records where it was taken."""
        with Probe(port) as probe:
            result = probe.survey()
        if label:
            result["label"] = label
        result["findings"] = analysis.analyse_survey(result)
        return result

    @server.tool()
    def thread_discover() -> list:
        """Thread routers in radio range: network name, extended PAN ID, channel, RSSI, LQI."""
        return run("thread_discover")

    @server.tool()
    def thread_energy(duration_ms: int = 200) -> list:
        """Peak RF energy (dBm) on each 802.15.4 channel 11-26; shows interference."""
        return run("thread_energy", duration_ms=duration_ms)

    @server.tool()
    def wifi_scan() -> list:
        """2.4 GHz Wi-Fi access points in range with channel and RSSI."""
        return run("wifi_scan")

    @server.tool()
    def ble_scan(duration_ms: int = 5000) -> dict:
        """Matter devices advertising over BLE (pairing mode) with discriminator,
        vendor/product ID and RSSI, plus a count of all BLE advertisers."""
        return run("ble_scan", duration_ms=duration_ms)

    @server.tool()
    def lan_scan(seconds: float = 6.0) -> dict:
        """mDNS view from this computer (no probe needed): Thread border routers and
        which Thread network each is on, commissioned Matter nodes, plus "findings"."""
        result = lan.scan(seconds)
        result["findings"] = analysis.analyse_lan(result)
        return result

    server.run()
