"""Turn raw scans into findings a person (or agent) can act on.

A finding is {"level": "problem" | "warning" | "ok" | "info", "title", "detail"}.
"""

from __future__ import annotations

WEAK_RSSI = -80  # dBm: below this a Thread link is unreliable
MARGINAL_RSSI = -70
LOUD_WIFI_RSSI = -75  # an overlapping AP at least this strong can hurt Thread


def finding(level: str, title: str, detail: str = "") -> dict:
    return {"level": level, "title": title, "detail": detail}


def thread_freq(channel: int) -> int:
    """Centre frequency in MHz of an 802.15.4 channel (11-26)."""
    return 2405 + 5 * (channel - 11)


def wifi_span(channel: int, secondary: str = "none") -> tuple[int, int]:
    """Occupied band (low, high) in MHz of a 2.4 GHz Wi-Fi channel."""
    centre = 2484 if channel == 14 else 2407 + 5 * channel
    low, high = centre - 11, centre + 11
    if secondary == "above":
        high += 20
    elif secondary == "below":
        low -= 20
    return low, high


def overlaps(thread_channel: int, wifi_channel: int, secondary: str = "none") -> bool:
    low, high = wifi_span(wifi_channel, secondary)
    f = thread_freq(thread_channel)
    return f + 1 >= low and f - 1 <= high


def merge_routers(rounds: list[list[dict]]) -> list[dict]:
    """Combine several discovery scans: one entry per router with its best RSSI
    and "heard", the number of scans it answered. Single scans miss routers."""
    merged: dict[str, dict] = {}
    for scan in rounds:
        for ext_addr in {r["ext_addr"] for r in scan}:
            best = max((r for r in scan if r["ext_addr"] == ext_addr), key=lambda r: r["rssi"])
            entry = merged.setdefault(ext_addr, {**best, "heard": 0})
            entry["heard"] += 1
            if best["rssi"] > entry["rssi"]:
                entry.update(rssi=best["rssi"], lqi=best.get("lqi"))
    return sorted(merged.values(), key=lambda r: (r["network_name"], -r["rssi"]))


def signal_bar(rssi: int | None, width: int = 30, low: int = -95, high: int = -35) -> str:
    """A text meter for an RSSI in dBm; empty when there is no reading."""
    filled = 0 if rssi is None else round(width * (min(max(rssi, low), high) - low) / (high - low))
    return "█" * filled + "░" * (width - filled)


def thread_networks(routers: list[dict]) -> dict[str, dict]:
    """Group discovered routers by extended PAN ID."""
    nets: dict[str, dict] = {}
    for r in routers:
        net = nets.setdefault(
            r["ext_pan_id"],
            {"name": r.get("network_name", ""), "channel": r["channel"], "routers": []},
        )
        net["routers"].append(r)
    for net in nets.values():
        net["best_rssi"] = max(r["rssi"] for r in net["routers"])
    return nets


def analyse_survey(survey: dict) -> list[dict]:
    """Findings for one radio survey taken at one spot."""
    out: list[dict] = []
    wifi = survey.get("wifi", [])

    if "thread" not in survey:
        out.append(finding(
            "info", "No Thread radio on this probe",
            "This chip only has Wi-Fi and BLE. Use an ESP32-C6 to see Thread itself."))
    else:
        nets = thread_networks(survey["thread"])
        if not nets:
            out.append(finding(
                "problem", "No Thread network in range here",
                "No Thread router answered. A Thread device at this spot cannot join or stay "
                "connected. Add a mains-powered Thread device (plug, bulb) or border router nearby."))
        if len(nets) > 1:
            names = ", ".join(f"'{n['name']}' (ch {n['channel']})" for n in nets.values())
            out.append(finding(
                "warning", f"{len(nets)} separate Thread networks in range",
                f"Found {names}. Border routers from different ecosystems each make their own "
                "network unless they share credentials; devices on one cannot use routers on the "
                "other. Harmless if the extra one is a neighbour's, or a private link such as a Nest "
                "thermostat and its Heat Link, which run their own small Thread network."))
        for net in nets.values():
            label = f"Thread network '{net['name']}' (ch {net['channel']})"
            n, best = len(net["routers"]), net["best_rssi"]
            if best < WEAK_RSSI:
                out.append(finding(
                    "problem", f"{label}: signal too weak here",
                    f"Best of {n} router(s) is {best} dBm. Devices here will drop. "
                    "Add a mains-powered Thread router between here and the rest of the mesh."))
            elif best < MARGINAL_RSSI:
                out.append(finding(
                    "warning", f"{label}: marginal signal",
                    f"Best of {n} router(s) is {best} dBm; expect occasional dropouts."))
            else:
                out.append(finding("ok", f"{label}: good signal",
                                   f"{n} router(s) in range, best {best} dBm."))

            loud = [ap for ap in wifi
                    if ap["rssi"] >= LOUD_WIFI_RSSI
                    and overlaps(net["channel"], ap["channel"], ap.get("secondary", "none"))]
            if loud:
                aps = ", ".join(f"'{ap['ssid'] or '(hidden)'}' ch {ap['channel']} {ap['rssi']} dBm"
                                for ap in sorted(loud, key=lambda a: -a["rssi"]))
                out.append(finding(
                    "warning", f"{label}: shares spectrum with strong Wi-Fi",
                    f"Overlapping access points: {aps}. Move your own Wi-Fi to a channel that "
                    f"avoids {thread_freq(net['channel'])} MHz, or move Thread to channel 25/26."))

    ble = survey.get("ble", {})
    pairing = [d for d in ble.get("matter", []) if d.get("commissionable")]
    if pairing:
        devs = ", ".join(f"discriminator {d['discriminator']} at {d['rssi']} dBm" for d in pairing)
        out.append(finding("info", f"{len(pairing)} Matter device(s) in pairing mode", devs))
    return out


def analyse_lan(lan: dict) -> list[dict]:
    """Findings for an mDNS view of the LAN (see lan.scan)."""
    out: list[dict] = []
    routers = lan.get("border_routers", [])
    if not routers:
        out.append(finding(
            "problem", "No Thread border router visible on this network",
            "Nothing advertises _meshcop._udp. Either there is no border router, or mDNS "
            "(multicast) is blocked between this computer and it, e.g. by a VLAN or AP isolation."))
    nets = {r.get("ext_pan_id"): r.get("network_name") for r in routers if r.get("ext_pan_id")}
    if len(nets) > 1:
        detail = "; ".join(
            f"'{name}': " + ", ".join(r["name"] for r in routers if r.get("ext_pan_id") == xp)
            for xp, name in nets.items())
        out.append(finding(
            "problem", f"Border routers are split across {len(nets)} Thread networks",
            f"{detail}. Each group runs its own mesh. Sync Thread credentials so all border "
            "routers join one network."))
    elif routers:
        out.append(finding("ok", f"{len(routers)} border router(s) on one Thread network",
                           ", ".join(r["name"] for r in routers)))
    for r in routers:
        if not any(":" in a for a in r.get("addresses", [])):
            out.append(finding(
                "warning", f"Border router '{r['name']}' has no IPv6 address",
                "Matter over Thread needs IPv6 on the LAN."))

    nodes = lan.get("matter_nodes", [])
    fabrics = {n["fabric"] for n in nodes}
    out.append(finding("info", f"{len(nodes)} commissioned Matter node(s) on {len(fabrics)} fabric(s)"))
    unreachable = [n["name"] for n in nodes if not n.get("addresses")]
    if unreachable:
        out.append(finding("warning", f"{len(unreachable)} Matter node(s) advertise no address",
                           ", ".join(unreachable)))
    return out
