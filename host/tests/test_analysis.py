from matter_debugger import analysis as a


def router(name="home", xp="aa" * 8, channel=15, rssi=-60):
    return {"network_name": name, "ext_pan_id": xp, "channel": channel, "rssi": rssi, "lqi": 200}


def titles(findings, level):
    return [f["title"] for f in findings if f["level"] == level]


def test_channel_overlap():
    assert a.thread_freq(11) == 2405 and a.thread_freq(26) == 2480
    assert a.overlaps(15, 1) is False  # 2425 vs 2401-2423
    assert a.overlaps(15, 3)
    assert a.overlaps(25, 11) is False  # the classic "safe" Thread channel
    assert a.overlaps(18, 6)
    assert a.overlaps(18, 1, "above")  # 40 MHz channel reaches up to 2443
    assert a.overlaps(18, 1) is False


def test_good_single_network():
    f = a.analyse_survey({"thread": [router(), router(rssi=-75)], "wifi": [], "ble": {}})
    assert [x["level"] for x in f] == ["ok"]


def test_no_thread_in_range_is_a_problem():
    f = a.analyse_survey({"thread": [], "wifi": [], "ble": {}})
    assert titles(f, "problem") == ["No Thread network in range here"]


def test_weak_and_split_networks():
    survey = {"thread": [router(rssi=-85), router(name="google", xp="bb" * 8, channel=25)],
              "wifi": [], "ble": {}}
    f = a.analyse_survey(survey)
    assert any("2 separate Thread networks" in t for t in titles(f, "warning"))
    assert any("'home'" in t and "too weak" in t for t in titles(f, "problem"))


def test_wifi_interference_only_counts_loud_overlapping_aps():
    wifi = [{"ssid": "loud", "channel": 3, "rssi": -50, "secondary": "none"},
            {"ssid": "quiet", "channel": 3, "rssi": -90, "secondary": "none"},
            {"ssid": "elsewhere", "channel": 11, "rssi": -40, "secondary": "none"}]
    f = a.analyse_survey({"thread": [router()], "wifi": wifi, "ble": {}})
    (w,) = [x for x in f if x["level"] == "warning"]
    assert "'loud'" in w["detail"] and "quiet" not in w["detail"] and "elsewhere" not in w["detail"]


def test_probe_without_thread_radio_and_pairing_device():
    ble = {"advertisers": 3, "matter": [{"commissionable": True, "discriminator": 3840, "rssi": -55}]}
    f = a.analyse_survey({"wifi": [], "ble": ble})
    assert titles(f, "info") == ["No Thread radio on this probe", "1 Matter device(s) in pairing mode"]


def test_lan_split_border_routers():
    lan = {"border_routers": [
        {"name": "HA", "network_name": "ha-thread", "ext_pan_id": "11" * 8, "addresses": ["fd00::1"]},
        {"name": "Nest", "network_name": "NEST-PAN", "ext_pan_id": "22" * 8, "addresses": ["192.168.1.5"]},
    ], "matter_nodes": []}
    f = a.analyse_lan(lan)
    assert any("split across 2" in t for t in titles(f, "problem"))
    assert titles(f, "warning") == ["Border router 'Nest' has no IPv6 address"]


def test_lan_no_border_router():
    f = a.analyse_lan({"border_routers": [], "matter_nodes": []})
    assert titles(f, "problem") == ["No Thread border router visible on this network"]
