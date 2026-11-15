from matter_debugger import watch


def test_short_id():
    assert watch.short_id("000000000000002B") == "2B"


def test_update_reports_drops_only_after_two_misses_and_recoveries():
    state, pings = {}, {"2B", "2E"}
    up = {"addresses": ["fd00::1"], "ping_ms": 3400.0}
    assert watch.update(state, {"2E": up, "15": {"addresses": []}}, pings) == [
        "15 is advertising", "2B is NOT advertising", "2E is advertising"]
    assert watch.update(state, {"15": {"addresses": []}}, pings) == []  # one miss is not a drop
    assert watch.update(state, {"15": {"addresses": []}}, pings) == ["2E stopped advertising"]
    assert watch.update(state, {"2E": up, "15": {"addresses": []}}, pings) == ["2E is advertising again"]


def test_update_reports_ping_changes():
    state, pings = {}, {"2E"}
    quiet = {"addresses": ["fd00::1"], "ping_ms": None}
    assert watch.update(state, {"2E": quiet}, pings) == [
        "2E is advertising", "2E advertises but does not answer pings"]
    assert watch.update(state, {"2E": {"addresses": ["fd00::1"], "ping_ms": 3000.0}}, pings) == [
        "2E answers pings again"]
