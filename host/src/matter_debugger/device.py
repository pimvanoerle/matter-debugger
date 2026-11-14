"""Serial client for the matter-probe firmware (one JSON object per line)."""

from __future__ import annotations

import itertools
import json
import time

import serial
from serial.tools import list_ports

ESPRESSIF_VID = 0x303A

# Generous per-command timeouts in seconds; scans block until they finish.
TIMEOUTS = {"wifi_scan": 20, "ble_scan": 15, "thread_discover": 35, "thread_energy": 15}


class ProbeError(RuntimeError):
    pass


def find_ports() -> list[str]:
    """Serial ports that look like an Espressif native-USB device."""
    return [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_VID]


class Probe:
    def __init__(self, port: str | None = None, *, transport=None):
        """Open the probe on `port`, or auto-detect it. `transport` is for tests."""
        if transport is None:
            if port is None:
                ports = find_ports()
                if not ports:
                    raise ProbeError("no probe found; plug it in or pass --port")
                if len(ports) > 1:
                    raise ProbeError(f"several probes found ({', '.join(ports)}); pass --port")
                port = ports[0]
            transport = serial.Serial(port, 115200, timeout=0.2)
        self._io = transport
        self._ids = itertools.count(1)

    def close(self) -> None:
        self._io.close()

    def __enter__(self) -> "Probe":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def call(self, cmd: str, timeout: float | None = None, **args):
        """Run one command and return its `data`; raises ProbeError on failure."""
        req_id = next(self._ids)
        request = {"id": req_id, "cmd": cmd}
        if args:
            request["args"] = args
            if cmd == "ble_scan" and "duration_ms" in args and timeout is None:
                timeout = args["duration_ms"] / 1000 + 10
        self._io.reset_input_buffer()
        self._io.write((json.dumps(request) + "\n").encode())
        self._io.flush()

        deadline = time.monotonic() + (timeout or TIMEOUTS.get(cmd, 5))
        buf = b""
        while time.monotonic() < deadline:
            buf += self._io.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.strip()
                if not line.startswith(b"{"):
                    continue  # firmware log output
                try:
                    msg = json.loads(line)
                except ValueError:
                    continue
                if msg.get("id") != req_id:
                    continue
                if not msg.get("ok"):
                    raise ProbeError(f"{cmd}: {msg.get('error', 'failed')}")
                return msg.get("data")
        raise ProbeError(f"{cmd}: no reply from probe within {timeout or TIMEOUTS.get(cmd, 5)}s")

    def survey(self, ble_ms: int = 5000) -> dict:
        """Run every scan the probe supports and return them in one dict."""
        info = self.call("info")
        out = {"time": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "info": info}
        if "thread" in info.get("capabilities", []):
            out["thread"] = self.call("thread_discover")
            out["energy"] = self.call("thread_energy")
        out["wifi"] = self.call("wifi_scan")
        out["ble"] = self.call("ble_scan", duration_ms=ble_ms)
        return out
