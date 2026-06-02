#!/usr/bin/env python3
"""Quick end-to-end test of the macOS connected-peripheral path.

Discovers the HID-held 'Claude Controller', connects without scanning, writes
one full two-provider payload to the RX characteristic, and — crucially —
subscribes to the TX characteristic to confirm the firmware ACKs the write.
The firmware notifies {"ack":true} on a successful parse and {"err":true} on a
parse failure; that ack is what actually proves the device received the full
payload without truncation, not merely that the write call returned.

Run from Terminal.app (which has Bluetooth permission):

    cd daemon && ./.venv/bin/python ./test_macos_connect.py

The payload is a full 14-key snapshot exercising every wire-contract key
(≈142 bytes — the exact length is logged at runtime). The ATT MTU must be
>= payload length + 3; negotiated values are typically Linux ~517, macOS ~185,
both comfortably above ≈142.
"""
import asyncio

from bleak import BleakClient

import claude_usage_daemon as d

# Firmware ack/nack channel. The daemon never subscribes to it in normal
# operation, so it isn't defined in the daemon module — declare it here.
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"


async def main() -> None:
    d.log("Discovering target via macOS connected-peripheral path...")
    target = await d.discover_target()
    if not target:
        d.log("FAIL: no target found (device powered on and showing splash?)")
        return

    display = target if isinstance(target, str) else f"{target.name} [{target.address}]"
    d.log(f"Target: {display}")

    client = BleakClient(target)
    d.log("Connecting (should NOT scan)...")
    await client.connect()
    if not client.is_connected:
        d.log("FAIL: connected=False")
        return
    d.log("Connected. Enumerating services...")

    found_rx = any(
        ch.uuid.lower() == d.RX_CHAR_UUID.lower()
        for service in client.services
        for ch in service.characteristics
    )
    d.log(f"RX characteristic present: {found_rx}")
    if not found_rx:
        d.log("FAIL: custom RX characteristic not found on the peripheral.")
        await client.disconnect()
        return

    # Subscribe to TX *before* writing so the firmware's ack/nack can confirm
    # the device actually parsed the payload (this is the real truncation test).
    ack = asyncio.Event()
    result = {"ok": None, "raw": b""}

    def on_tx(_char, data: bytearray) -> None:
        result["raw"] = bytes(data)
        text = result["raw"].decode("utf-8", errors="replace")
        result["ok"] = '"ack"' in text
        ack.set()

    try:
        await client.start_notify(TX_CHAR_UUID, on_tx)
    except Exception as e:  # noqa: BLE001 - diagnostic tool, log and continue
        d.log(f"WARN: could not subscribe to TX ({e}); the write will be unverified")

    payload = (
        '{"s":42,"sr":120,"w":17,"wr":4320,"st":"allowed","ok":true,'
        '"sp":true,"cp":true,'
        '"cs":31,"csr":146,"cw":6,"cwr":8241,"cst":"allowed","cok":true}'
    )
    d.log(f"Writing test payload ({len(payload)} bytes): {payload}")
    await client.write_gatt_char(d.RX_CHAR_UUID, payload.encode(), response=False)

    try:
        await asyncio.wait_for(ack.wait(), timeout=5.0)
        if result["ok"]:
            d.log(f"PASS: device ACKed the full {len(payload)}-byte payload (no truncation).")
        else:
            d.log(f"FAIL: device returned NACK/err: {result['raw']!r}")
    except asyncio.TimeoutError:
        d.log("WARN: no ack within 5s — payload written but parse is unconfirmed; check the screen.")

    try:
        await client.stop_notify(TX_CHAR_UUID)
    except Exception:
        pass
    await client.disconnect()
    d.log("Disconnected. Done.")


if __name__ == "__main__":
    asyncio.run(main())
