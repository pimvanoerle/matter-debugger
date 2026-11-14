"""Command line for the matter-probe and LAN checks."""

from __future__ import annotations

import json
import time

import click

from . import __version__, analysis, lan as lan_mod
from .device import Probe, ProbeError, find_ports

MARKS = {"problem": ("✗", "red"), "warning": ("!", "yellow"), "ok": ("✓", "green"), "info": ("·", "cyan")}


def print_findings(findings: list[dict]) -> None:
    for f in findings:
        mark, colour = MARKS[f["level"]]
        click.echo(click.style(f" {mark} {f['title']}", fg=colour, bold=True))
        if f["detail"]:
            click.echo(f"   {f['detail']}")


@click.group()
@click.version_option(__version__)
@click.option("--port", help="Serial port of the probe (auto-detected by default).")
@click.pass_context
def main(ctx: click.Context, port: str | None) -> None:
    """Debug Matter and Thread: a walk-around radio probe plus LAN checks."""
    ctx.obj = port


def open_probe(port: str | None) -> Probe:
    try:
        return Probe(port)
    except (ProbeError, OSError) as e:
        raise click.ClickException(str(e))


@main.command()
def ports() -> None:
    """List serial ports that look like a probe."""
    found = find_ports()
    click.echo("\n".join(found) if found else "No probe found.")


@main.command()
@click.pass_obj
def info(port: str | None) -> None:
    """Show the probe's firmware, chip and radios."""
    with open_probe(port) as probe:
        try:
            click.echo(json.dumps(probe.call("info"), indent=2))
        except ProbeError as e:
            raise click.ClickException(str(e))


@main.command()
@click.argument("cmd")
@click.argument("args", required=False)
@click.pass_obj
def raw(port: str | None, cmd: str, args: str | None) -> None:
    """Send one command to the probe, e.g. `raw ble_scan '{"duration_ms": 2000}'`."""
    with open_probe(port) as probe:
        try:
            click.echo(json.dumps(probe.call(cmd, **(json.loads(args) if args else {})), indent=2))
        except ProbeError as e:
            raise click.ClickException(str(e))


@main.command()
@click.option("--label", help="Where you are standing, e.g. 'kitchen'.")
@click.option("--log", type=click.Path(dir_okay=False), help="Append the survey to this JSONL file.")
@click.option("--json", "as_json", is_flag=True, help="Print raw scans and findings as JSON.")
@click.pass_obj
def survey(port: str | None, label: str | None, log: str | None, as_json: bool) -> None:
    """Scan Thread, Wi-Fi and BLE at this spot and say what looks wrong."""
    with open_probe(port) as probe:
        try:
            result = probe.survey()
        except ProbeError as e:
            raise click.ClickException(str(e))
    if label:
        result["label"] = label
    result["findings"] = analysis.analyse_survey(result)
    if log:
        with open(log, "a") as f:
            f.write(json.dumps(result) + "\n")
    if as_json:
        click.echo(json.dumps(result, indent=2))
        return

    click.echo(f"Survey{f' at {label}' if label else ''} ({result['info']['chip']})")
    for xp, net in analysis.thread_networks(result.get("thread", [])).items():
        click.echo(f"  Thread '{net['name']}' ch {net['channel']} xpan {xp}: "
                   f"{len(net['routers'])} router(s), best {net['best_rssi']} dBm")
        for r in net["routers"]:
            click.echo(f"    …{r['ext_addr'][-4:]} {analysis.signal_bar(r['rssi'], 20)} {r['rssi']} dBm"
                       + (f"  (answered {r['heard']} of 3 scans)" if r.get("heard", 3) < 3 else ""))
    click.echo(f"  Wi-Fi: {len(result['wifi'])} access point(s); "
               f"BLE: {result['ble']['advertisers']} advertiser(s)")
    print_findings(result["findings"])


@main.command()
@click.argument("target", required=False)
@click.option("--log", type=click.Path(dir_okay=False), help="Append every reading to this JSONL file.")
@click.pass_obj
def track(port: str | None, target: str | None, log: str | None) -> None:
    """Live signal meter for Thread routers; walk around to find where one is.

    TARGET is the end of a router address (e.g. 715a) or part of a network name.
    Without it, lists what is in range. Stop with Ctrl-C.
    """
    with open_probe(port) as probe:
        try:
            if "thread" not in probe.call("info").get("capabilities", []):
                raise click.ClickException("this probe has no 802.15.4 radio")
            click.echo("Looking for Thread routers...")
            found = probe.discover(rounds=3)
            def matches(r: dict) -> bool:
                return bool(target) and (r["ext_addr"].endswith(target.lower())
                                         or target.lower() in r["network_name"].lower())

            wanted = [r for r in found if matches(r)]
            if not wanted:
                if target:
                    click.echo(f"Nothing matching '{target}'. In range:")
                for r in found:
                    click.echo(f"  {r['ext_addr'][-4:]}  {r['network_name']}  ch {r['channel']}  {r['rssi']} dBm")
                return
            channels = sorted({r["channel"] for r in wanted})
            rows = {r["ext_addr"]: {"name": r["network_name"], "rssi": None, "peak": r["rssi"], "age": 0}
                    for r in wanted}
            click.echo("Stop with Ctrl-C.\n" + "\n" * (len(rows) - 1), nl=False)
            while True:
                heard = {}
                for ch in channels:
                    for r in probe.call("thread_discover", channel=ch):
                        heard[r["ext_addr"]] = r["rssi"]
                        if matches(r) and r["ext_addr"] not in rows:
                            # a router that was out of range (or off) when tracking started
                            rows[r["ext_addr"]] = {"name": r["network_name"], "rssi": None,
                                                   "peak": r["rssi"], "age": 0}
                            click.echo()
                if log:
                    with open(log, "a") as f:
                        f.write(json.dumps({"time": time.time(), "rssi": {a: heard[a] for a in rows if a in heard}}) + "\n")
                lines = []
                for addr, row in rows.items():
                    if addr in heard:
                        row.update(rssi=heard[addr], age=0, peak=max(row["peak"], heard[addr]))
                    else:
                        row["age"] += 1
                        if row["age"] >= 3:  # three misses in a row: treat as out of range
                            row["rssi"] = None
                    reading = f"{row['rssi']:>4} dBm" if row["rssi"] is not None else " no reply"
                    lines.append(f"…{addr[-4:]} {row['name']:<16} {analysis.signal_bar(row['rssi'])} "
                                 f"{reading}  peak {row['peak']}")
                click.echo(f"\033[{len(lines)}A" + "".join(f"\r\033[K{line}\n" for line in lines), nl=False)
        except ProbeError as e:
            raise click.ClickException(str(e))
        except KeyboardInterrupt:
            click.echo()


@main.command()
@click.option("--seconds", default=6.0, show_default=True, help="How long to listen.")
@click.option("--json", "as_json", is_flag=True, help="Print raw results and findings as JSON.")
def lan(seconds: float, as_json: bool) -> None:
    """Check the IP side from this computer: border routers and Matter nodes (no probe needed)."""
    result = lan_mod.scan(seconds)
    result["findings"] = analysis.analyse_lan(result)
    if as_json:
        click.echo(json.dumps(result, indent=2))
        return
    for r in result["border_routers"]:
        click.echo(f"  Border router {r['name']} ({r['vendor'] or '?'} {r['model'] or ''}): "
                   f"network '{r['network_name']}' xpan {r['ext_pan_id']}")
    print_findings(result["findings"])


@main.command()
@click.pass_obj
def mcp(port: str | None) -> None:
    """Run an MCP server (stdio) so AI agents can use the probe."""
    try:
        from .mcp_server import serve
    except ImportError:
        raise click.ClickException("MCP support not installed; run: pip install 'matter-debugger[mcp]'")
    serve(port)
