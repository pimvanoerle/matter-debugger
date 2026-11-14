# matter-debugger

A pocket-sized radio probe and a set of network checks for finding out **why
your Matter / Thread smart home is flaky**: devices that refuse to pair, rooms
that drop out, switches that go "unavailable" for no visible reason.

Matter gives you almost no feedback when it fails. This project gives you
something to walk around the house with that says, per spot: is there a Thread
network here, how strong is it, is Wi-Fi sitting on top of it, and is that
device you are trying to pair even advertising.

It is built so that AI agents can use it too: the probe speaks JSON over USB,
and the host tool includes an MCP server.

> **Status: early.** The host tool and LAN checks work. The firmware compiles
> for ESP32-C6 and ESP32-S3 but has not yet been run on hardware.

## How it fits together

```
 ┌──────────────┐  USB serial, JSON lines  ┌──────────────────┐  MCP   ┌───────┐
 │ probe (ESP32)│ ───────────────────────► │ matter-debugger  │ ─────► │ agent │
 │ 802.15.4/BLE │                          │ CLI on a laptop  │        └───────┘
 │ /Wi-Fi scans │                          │ + mDNS LAN checks│
 └──────────────┘                          └──────────────────┘
```

- **`firmware/`**: ESP-IDF firmware for the probe.
- **`host/`**: Python CLI, analysis and MCP server.

## What it checks

| Check | Needs | Finds |
| --- | --- | --- |
| Thread discovery | probe with 802.15.4 | Thread routers in earshot, their network, channel and signal strength; dead spots; several separate Thread networks |
| Channel energy | probe with 802.15.4 | Noise on each 802.15.4 channel |
| Wi-Fi scan | any probe | Strong access points overlapping the Thread channel |
| BLE scan | any probe | Matter devices in pairing mode (discriminator, vendor/product ID, signal) |
| LAN scan | just a laptop | Border routers and which Thread network each is on, IPv6 presence, commissioned Matter nodes and fabrics |

## Hardware

Thread runs on IEEE 802.15.4 at 2.4 GHz, so the chip matters more than its speed:

| Chip | Wi-Fi | BLE | Thread | Use |
| --- | --- | --- | --- | --- |
| **ESP32-C6** | yes | yes | yes | Recommended: everything on one chip |
| ESP32-S3 / classic ESP32 | yes | yes | no | Works for the Wi-Fi and BLE checks only |
| ESP32-H2 | no | yes | yes | Possible companion radio for an S3 (not supported yet) |

Sub-GHz radios such as the CC1101 cannot receive Thread.

## Quick start

Laptop only, no probe needed:

```sh
cd host
python3 -m venv .venv && . .venv/bin/activate
pip install -e '.[mcp]'
matter-debugger lan
```

Flash a probe (ESP-IDF 5.4, board connected by its native USB port):

```sh
cd firmware
idf.py set-target esp32c6      # or esp32s3
idf.py flash
```

Then walk around:

```sh
matter-debugger survey --label kitchen --log survey.jsonl
```

### For agents

```sh
matter-debugger mcp
```

runs an MCP server over stdio with the tools `survey`, `lan_scan`,
`thread_discover`, `thread_energy`, `wifi_scan`, `ble_scan` and `probe_info`.
`survey` and `lan_scan` return raw data plus a list of findings.

## Probe protocol

One JSON object per line at any baud rate over the USB serial port. Lines that
do not start with `{` are log output.

```
→ {"id": 1, "cmd": "thread_discover"}
← {"id": 1, "ok": true, "data": [{"network_name": "...", "ext_pan_id": "...", "channel": 15, "rssi": -61, ...}]}
```

Commands: `info`, `thread_discover`, `thread_energy`, `wifi_scan`, `ble_scan`.
Scans take an optional `"args": {"duration_ms": N}`.

## Roadmap

- Run and tune the firmware on real hardware
- Join the Thread network with its credentials: mesh topology, per-router link
  quality, parent/child tables, reachability of border routers
- Standalone mode: display, button, battery, on-device log
- Long-running monitor that records when devices drop and what changed
- 802.15.4 sniffer mode (pcap for Wireshark)
- 3D-printable case
- Home Assistant add-on

## Development

```sh
cd host && pip install -e '.[mcp,dev]' && pytest
```

## License

MIT
