# `healthypi` — host library and CLI for HealthyPi 6

One package instead of nineteen scripts, on the same rule the firmware uses:
a capability is implemented once, and the CLI is a thin adapter over it.

Distribution name **`protocentral-healthypi`**; the import package and both
console scripts stay `healthypi` (and the short alias `hpi`).

```bash
pip install protocentral-healthypi            # library only, stdlib deps
pip install "protocentral-healthypi[device]"  # + smpclient/pyserial/cryptography

pip install -e tools/healthypi                # from a checkout
pip install -e "tools/healthypi[device]"
```

## What works today

| Module | State |
|---|---|
| `healthypi.hp6` | ✅ decode/verify/export/repair `.HP6`, files and live streams |
| `healthypi.smp.catalog` | ✅ the group-64 single source of truth, drift-checked against the firmware |
| `healthypi.smp.group64` | ✅ wire classes, **generated** from the catalog |
| `healthypi.transport` | ✅ CDC 1 connect + autodetect |
| `healthypi.openview` | ✅ Wi-Fi (OpenView v2) decode + monitor |
| `healthypi.cli` | ✅ the `healthypi` / `hpi` command |
| `healthypi.fw` | ✅ `.hpifw` bundles, M7+M4 update, MCUboot serial recovery |
| `healthypi.hw` | ✅ HealthyLink module EEPROM images |
| `healthypi.testing` | ✅ the group-64 acceptance suite, importable |

**171 tests, no hardware required.**

`healthypi.fw`'s offline half (bundle create, verify, and refusing a tampered or
wrongly-signed one) is covered by tests. **Its device half — upload, commit,
reset and reconnect — has never run against a board since the fold**; that is the
bench gate, not a green suite.

`healthypi.hp6` and `healthypi.smp.catalog` are **stdlib-only on purpose**:
decoding a recording, and checking the protocol catalog against the firmware,
must never require a device stack to be installed.

## `hp6` — the container

```python
from healthypi import hp6

header, blocks = hp6.read_file("REC0001.HP6")
for blk in blocks:                       # lazy: 2 GiB files stream
    for s in blk.samples():
        ...

report = hp6.verify("REC0001.HP6")       # CRCs, seq gaps, sync chain, counts
hp6.export_csv("REC0001.HP6", "out/")    # one CSV per channel, streaming
hp6.repair("yanked.HP6")                 # → yanked.HP6.repaired

hp6.wrap_capture(cdc0_bytes, "capture.HP6")   # a raw stream is not a file
```

The same `DBLK` frame carries the live CDC0 stream and the recorded file; only
the container differs (the file adds the 256-byte header, sync markers and
sidecars). Layout: [`docs/HP6_DATA_FORMAT.md`](../../docs/HP6_DATA_FORMAT.md).

The reader **never aborts on bad data** — a corrupt block resyncs to the next
`DBLK` and the loss is counted in `ReadStats`. That is required, not defensive:
the device drops frames on a full consumer ring rather than back-pressuring
acquisition, so gaps are normal.

## `smp.catalog` — the protocol

```python
from healthypi.smp import catalog

catalog.BY_NAME["telemetry"].response      # every key the device sends
catalog.live()                             # 27 commands that really work
catalog.unlock_gated()                     # 9 that need the security unlock
catalog.err_hint(267)                      # 'no SD card present'
```

Transcribed from `hpi_mgmt_group.h`, the dispatch table in `hpi_mgmt_group.c`,
and the handler sources. `tests/test_catalog_drift.py` re-parses all three via
[`smpgroup.drift`](../smpgroup/) and fails on any disagreement — command ids,
dispatch status, error codes, and every request/response key. Prose version with
reference: [`docs/MCUMGR_COMMANDS.md`](../../docs/MCUMGR_COMMANDS.md).

## `smp.group64` — the wire

```python
from healthypi.smp.group64 import g, fmt_error, is_error

async with SMPClient(SMPSerialTransport(), port) as client:
    info = await client.request(g.device_info())
    await client.request(g.stream_start(ch=0x03, ann=0))
    await client.request(g.transfer_mode_write(on=True))
```

**Generated** from the catalog by [`smpgroup`](../smpgroup/) — there are no
hand-written request/response classes, and there must never be. The generated
requests are asserted byte-identical to the hand-written set they replaced,
frozen in `tests/legacy_g64_reference.py` as it stood when the update path was
validated on v5 hardware.

## CLI

```bash
hpi catalog                     # every group-64 command, and its real status
hpi hp6 verify REC0001.HP6      # CRCs, gaps, counters
hpi hp6 to-csv REC0001.HP6 out/
hpi device info                 # port autodetected
hpi telemetry --json
hpi stream start --ch 0x03      # ECG + PPG
hpi record start --name walk
hpi transfer arm                # SD card as a USB disk (drops the connection)
hpi wifi-stream monitor --udp   # Wi-Fi packet rate and loss

# firmware
hpi fw info --bundle hpi6-1.0.0.hpifw --pubkey release.pem   # offline
hpi fw update --bundle hpi6-1.0.0.hpifw                      # all processors
hpi fw update --bundle hpi6-1.0.0.hpifw --only m4 --force
hpi fw enter-recovery                                        # into MCUboot
hpi fw recover --port <recovery-port> --bundle hpi6-1.0.0.hpifw

# acceptance suite (the bench gate)
hpi test list                        # the cases and what each needs
hpi test run                         # read-only by default
hpi test run --destructive --group fw
hpi test soak --iterations 60000     # 0 errors, p99 < 50 ms

# HealthyLink module EEPROMs
hpi hl eeprom generate -m EEG-8CH -n "EEG-8CH" -s 10001 -o eeprom.bin
hpi hl eeprom read eeprom.bin        # non-zero exit on a bad CRC
hpi hl eeprom stack --modules EEG-8CH TRIGGER-IO AI-ACCELERATOR
```

Update order is `esp32c6 → m4 → m7`; the M7 goes last because it is what applies
the other two. The C6 is skipped over USB — it updates itself over Wi-Fi.

The EEPROM verbs build the 256-byte image. Writing it to a real 24AA02 needs an
external programmer (FT232H, Raspberry Pi I2C, CH341A, Bus Pirate) and that
programmer's own tool; nothing here drives one.

`--port` is optional: CDC 1 is found by asking each serial port for an `os
echo`, which only the control port answers. Every read verb takes `--json`.

## Tests

```bash
pytest -q                                    # 171 tests, no hardware
PYTHONPATH=src python3 -m pytest -m hw --port /dev/tty.usbmodem…3   # (P3+)
```

The drift tests skip cleanly outside the firmware tree, so the package stays
testable standalone.
