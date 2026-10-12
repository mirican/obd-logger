# -*- coding: utf-8 -*-
"""Test połączenia z adapterem vLinker MS po BLE (z PC).

Użycie (na PC z Bluetooth):
    py -m pip install bleak
    py pc\\bleak_vlinker_test.py                    # skan + połączenie + ATZ/0100
    py pc\\bleak_vlinker_test.py --scan-only        # tylko skan (szuka "vlinker")
    py pc\\bleak_vlinker_test.py --addr AA:BB:...   # bez skanu (adres z nRF Connect)
    py pc\\bleak_vlinker_test.py --cmd ATH1 --cmd 010C

Co robi: znajduje urządzenie po nazwie, wypisuje usługi/charakterystyki GATT,
wybiera char. zapisu i notyfikacji, wysyła sekwencję ELM327 i drukuje odpowiedzi.

WAŻNE: vLinker MS musi być przełączony w tryb „BT + BLE" (VgateFwUpdater),
a telefon nie może być z nim połączony (BLE = jeden klient naraz).
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("Brak pakietu `bleak`. Zainstaluj:  py -m pip install bleak")
    sys.exit(1)

DEFAULT_SEQ = ["ATZ", "ATE0", "ATL0", "ATH1", "ATSP0", "0100"]


async def do_scan(pattern: str, timeout: float):
    print(f"# skan BLE ({timeout:.0f} s), szukam: *{pattern}* ...")
    devices = await BleakScanner.discover(timeout=timeout)
    hits = []
    for d in devices:
        name = d.name or "?"
        mark = ""
        if pattern.lower() in name.lower():
            hits.append(d)
            mark = "   <-- kandydat"
        print(f"  {d.address}  {name}{mark}")
    return hits


async def find_chars(client: BleakClient):
    write_ch = notify_ch = None
    for service in client.services:
        print(f"# usługa {service.uuid}  {service.description or ''}")
        for ch in service.characteristics:
            print(f"    char {ch.uuid}  [{','.join(ch.properties)}]")
            if write_ch is None and any(p.startswith("write") for p in ch.properties):
                write_ch = ch
            if notify_ch is None and any(p in ("notify", "indicate") for p in ch.properties):
                notify_ch = ch
    return write_ch, notify_ch


async def run(args) -> int:
    addr = args.addr
    if not addr:
        hits = await do_scan(args.pattern, args.scan_time)
        if not hits:
            print("# Nie znaleziono urządzenia.")
            print("#  - czy MS jest w trybie BT+BLE (VgateFwUpdater)?")
            print("#  - czy telefon nie jest z nim połączony (BLE = 1 klient)?")
            return 1
        addr = hits[0].address
        print(f"# wybrano: {addr} ({hits[0].name})")

    print(f"# łączę z {addr} ...")
    buf: list = []

    def on_notify(_sender, data: bytearray) -> None:
        buf.append(bytes(data))

    async with BleakClient(addr) as client:
        write_ch, notify_ch = await find_chars(client)
        if write_ch is None or notify_ch is None:
            print("# Brak charakterystyki write i/lub notify — patrz dump GATT powyżej.")
            return 1
        resp = ("write" in write_ch.properties
                and "write-without-response" not in write_ch.properties)
        await client.start_notify(notify_ch, on_notify)
        print(f"# TX -> {write_ch.uuid}   RX <- {notify_ch.uuid}")
        ok = 0
        for cmd in args.cmd:
            buf.clear()
            print(f">> {cmd}")
            await client.write_gatt_char(write_ch, (cmd + "\r").encode("ascii"),
                                         response=resp)
            t0 = time.monotonic()
            text = ""
            while time.monotonic() - t0 < args.timeout:
                await asyncio.sleep(0.05)
                text = b"".join(buf).decode("ascii", "replace")
                if ">" in text:
                    break
            clean = text.replace("\r", "\n").strip()
            print(clean or "(brak odpowiedzi)")
            if clean and ">" in text:
                ok += 1
        await client.stop_notify(notify_ch)
        print(f"# zakończono: {ok}/{len(args.cmd)} komend z odpowiedzią")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Test BLE: vLinker MS (ELM327) z PC")
    parser.add_argument("--scan-only", action="store_true", help="tylko skan BLE")
    parser.add_argument("--pattern", default="vlinker",
                        help="fragment nazwy (domyślnie 'vlinker')")
    parser.add_argument("--scan-time", type=float, default=8.0, help="czas skanu [s]")
    parser.add_argument("--addr", help="adres BLE (pomiń skan)")
    parser.add_argument("--cmd", action="append", help="komenda ELM (można wielokrotnie)")
    parser.add_argument("--timeout", type=float, default=3.0,
                        help="timeout na komendę [s]")
    args = parser.parse_args()
    if not args.cmd:
        args.cmd = list(DEFAULT_SEQ)

    if args.scan_only:
        hits = asyncio.run(do_scan(args.pattern, args.scan_time))
        print(f"# kandydatów: {len(hits)}")
        return 0 if hits else 1
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
