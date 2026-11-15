"""Watch commissioned Matter nodes over time: are they still advertising, do they answer?

A Thread device that stops advertising (while its neighbours carry on) has
dropped off the network; one that advertises but does not answer a ping is
registered but unreachable. Logging both over days shows when devices drop.
"""

from __future__ import annotations

import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor

from . import lan

MISSES_BEFORE_GONE = 2  # one mDNS scan can miss a node; need two in a row


def short_id(node: str) -> str:
    """'000000000000002B' -> '2B'."""
    return node.lstrip("0").upper() or "0"


def ping_ms(address: str) -> float | None:
    """Round-trip time in ms, or None. Sleepy battery devices take 3-7 s to answer."""
    try:
        out = subprocess.run(["ping6", "-c", "3", "-i", "4", address],
                             capture_output=True, text=True, timeout=25).stdout
    except (subprocess.TimeoutExpired, OSError):
        return None
    times = re.findall(r"time=([\d.]+)", out)
    return float(times[0]) if times else None


def observe(fabric: str | None, ping_ids: set[str], seconds: float = 10.0) -> dict[str, dict]:
    """One look at the network: {node id: {"addresses": [...], "ping_ms": ...}}."""
    seen: dict[str, dict] = {}
    for n in lan.scan(seconds)["matter_nodes"]:
        if fabric and n["fabric"].upper() != fabric.upper():
            continue
        routable = [a for a in n["addresses"] if ":" in a and not a.startswith("fe80")]
        seen[short_id(n["node"])] = {"addresses": routable}
    to_ping = [i for i in seen if i in ping_ids and seen[i]["addresses"]]
    with ThreadPoolExecutor(max_workers=8) as pool:
        for node_id, ms in zip(to_ping, pool.map(lambda i: ping_ms(seen[i]["addresses"][0]), to_ping)):
            seen[node_id]["ping_ms"] = ms
    return seen


def update(state: dict[str, dict], seen: dict[str, dict], ping_ids: set[str]) -> list[str]:
    """Fold one observation into `state` and return the changes worth reporting."""
    events = []
    for node_id in sorted((set(state) | set(seen) | ping_ids) - {"_"}):
        s = state.setdefault(node_id, {"advertising": None, "misses": 0, "answers": None})
        if node_id in seen:
            if s["advertising"] is not True:
                events.append(f"{node_id} is advertising" if s["advertising"] is None
                              else f"{node_id} is advertising again")
            s.update(advertising=True, misses=0)
            if node_id in ping_ids:
                answers = seen[node_id].get("ping_ms") is not None
                if s["answers"] is not None and answers != s["answers"]:
                    events.append(f"{node_id} answers pings again" if answers
                                  else f"{node_id} advertises but stopped answering pings")
                elif s["answers"] is None and not answers:
                    events.append(f"{node_id} advertises but does not answer pings")
                s["answers"] = answers
        else:
            s["misses"] += 1
            if s["advertising"] is None and node_id in ping_ids:
                events.append(f"{node_id} is NOT advertising")
                s["advertising"] = False
            elif s["advertising"] and s["misses"] >= MISSES_BEFORE_GONE:
                events.append(f"{node_id} stopped advertising")
                s["advertising"] = False
    return events


def stamp() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S")
