"""What the IP side of the network looks like: border routers and Matter nodes via mDNS."""

from __future__ import annotations

import time

from zeroconf import ServiceBrowser, ServiceListener, Zeroconf

MESHCOP = "_meshcop._udp.local."  # Thread border routers
MATTER = "_matter._tcp.local."  # commissioned Matter nodes
MATTERC = "_matterc._udp.local."  # Matter devices waiting to be commissioned

# MeshCoP TXT values that are raw bytes rather than text.
BINARY_KEYS = {"xp", "xa", "sb", "at", "pt", "dd", "omr", "id"}


class _Collector(ServiceListener):
    def __init__(self) -> None:
        self.seen: set[tuple[str, str]] = set()

    def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        self.seen.add((type_, name))

    def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        pass

    def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
        pass


def _txt(properties: dict) -> dict[str, str]:
    out = {}
    for key, value in properties.items():
        k = key.decode(errors="replace")
        if value is None:
            out[k] = ""
        elif k in BINARY_KEYS:
            out[k] = value.hex()
        else:
            try:
                out[k] = value.decode()
            except UnicodeDecodeError:
                out[k] = value.hex()
    return out


def scan(seconds: float = 6.0) -> dict:
    """Browse mDNS for `seconds` and return border routers and Matter nodes."""
    zc = Zeroconf()
    try:
        collector = _Collector()
        ServiceBrowser(zc, [MESHCOP, MATTER, MATTERC], collector)
        time.sleep(seconds)

        result: dict = {"border_routers": [], "matter_nodes": [], "commissionable": []}
        for type_, name in sorted(collector.seen):
            info = zc.get_service_info(type_, name, timeout=1500)
            instance = name[: -len(type_) - 1]
            addresses = info.parsed_scoped_addresses() if info else []
            txt = _txt(info.properties) if info else {}
            if type_ == MESHCOP:
                result["border_routers"].append({
                    "name": instance,
                    "host": info.server if info else None,
                    "addresses": addresses,
                    "network_name": txt.get("nn"),
                    "ext_pan_id": txt.get("xp"),
                    "vendor": txt.get("vn"),
                    "model": txt.get("mn"),
                    "thread_version": txt.get("tv"),
                })
            elif type_ == MATTER:
                fabric, _, node = instance.partition("-")
                result["matter_nodes"].append({
                    "name": instance, "fabric": fabric, "node": node,
                    "host": info.server if info else None, "addresses": addresses,
                })
            else:
                result["commissionable"].append({
                    "name": instance, "addresses": addresses,
                    "discriminator": txt.get("D"), "vendor_product": txt.get("VP"),
                    "device_name": txt.get("DN"),
                })
        return result
    finally:
        zc.close()
