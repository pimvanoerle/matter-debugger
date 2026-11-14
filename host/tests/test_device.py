import json

import pytest

from matter_debugger.device import Probe, ProbeError


class FakeSerial:
    """Replies to each written request with canned lines."""

    def __init__(self, replies):
        self.replies = replies  # cmd -> list of lines; "{id}" is filled in
        self.pending = b""

    def reset_input_buffer(self):
        self.pending = b""

    def write(self, data):
        req = json.loads(data)
        for line in self.replies.get(req["cmd"], []):
            self.pending += line.replace("{id}", str(req["id"])).encode() + b"\r\n"

    def flush(self):
        pass

    def read(self, n):
        out, self.pending = self.pending[:n], self.pending[n:]
        return out

    def close(self):
        pass


def test_call_skips_logs_and_stale_replies():
    io = FakeSerial({"info": [
        "W (123) wifi: some log line",
        '{"id": 999, "ok": true, "data": "stale"}',
        '{"id": {id}, "ok": true, "data": {"chip": "esp32c6"}}',
    ]})
    assert Probe(transport=io).call("info") == {"chip": "esp32c6"}


def test_call_raises_on_error_and_timeout():
    io = FakeSerial({"thread_discover": ['{"id": {id}, "ok": false, "error": "no radio"}']})
    probe = Probe(transport=io)
    with pytest.raises(ProbeError, match="no radio"):
        probe.call("thread_discover")
    with pytest.raises(ProbeError, match="no reply"):
        probe.call("info", timeout=0.3)


def test_survey_skips_thread_without_radio():
    io = FakeSerial({
        "info": ['{"id": {id}, "ok": true, "data": {"chip": "esp32s3", "capabilities": ["wifi", "ble"]}}'],
        "wifi_scan": ['{"id": {id}, "ok": true, "data": []}'],
        "ble_scan": ['{"id": {id}, "ok": true, "data": {"advertisers": 0, "matter": []}}'],
    })
    result = Probe(transport=io).survey()
    assert "thread" not in result and result["wifi"] == []
