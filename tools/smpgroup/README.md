# smpgroup

**Declarative custom MCUmgr management groups for Zephyr devices.**

Describe your device's custom SMP group once, as data. Then generate the
host-side request/response classes from it — and check that the description
still matches the firmware that implements it.

```bash
pip install smpgroup           # spec + drift checker, stdlib only
pip install 'smpgroup[smp]'    # + class generation (pulls in smpclient)
```

Apache-2.0. Works with [`smpclient`](https://pypi.org/project/smpclient/).

---

## The problem

Zephyr lets you add a custom MCUmgr management group (id ≥ 64) for
device-specific commands. On the host side you then hand-write a pydantic
request/response pair per command, because that is what `smpclient` consumes.

That works until you have more than one host tool. A test harness, a CLI, an
updater and a provisioning script each end up with their own copy of the same
schemas. And because `smp`'s response models are pydantic with
`extra="forbid"`, **a response class must declare every key the device sends** —
so the day someone adds a field to a reply in firmware, every copy that wasn't
updated starts failing with an opaque validation error, at runtime, usually on
someone else's bench.

The schemas are data. They should be written once, as data.

## Usage

### 1. Describe the group

```python
from smpgroup import Group, Command, Field, Op, Status, T

R, W = Op.READ, Op.WRITE

MY_GROUP = Group(
    group_id=64,
    name="acme",
    schema_version=1,
    commands=(
        Command(0x0001, "device_info", (R,), response=(
            Field("sn", T.TSTR, "serial number"),
            Field("up", T.UINT, "uptime, seconds"),
        )),
        Command(0x0002, "set_led", (W,),
                meta={"privileged": True},
                request=(Field("on", T.BOOL),),
                response=(Field("ok", T.BOOL),)),
        # both ops, different schemas
        Command(0x0003, "mode", (R, W),
                response=(Field("armed", T.BOOL),),
                write_request=(Field("on", T.BOOL),),
                write_response=(Field("armed", T.BOOL),)),
        # declared in the firmware header but never dispatched:
        Command(0x0004, "reserved", (), status=Status.UNREACHABLE),
    ),
    errors={
        256: ("NOT_READY", "a precondition is missing"),
        257: ("HW_FAULT", "hardware failure"),
    },
)
```

`T.MAP` fields take `nested=(...)` for maps whose keys you know; nested models
forbid unknown keys too.

### 2. Generate the wire classes

```python
from smpgroup.build import build

g = build(MY_GROUP)

async with SMPClient(SMPSerialTransport(), port) as client:
    info = await client.request(g.device_info())
    print(info.sn, info.up)

    await client.request(g.set_led(on=True))

    await client.request(g.mode())            # the read op
    await client.request(g.mode_write(on=1))  # the write op
```

A command declaring both ops is exposed as `g.<name>` (read) and
`g.<name>_write`. Commands marked `UNREACHABLE` generate nothing — you cannot
accidentally ship a verb the device will only ever answer with `ENOTSUP`.

### 3. Check the spec against the firmware

This is the part you will not find elsewhere. Point it at the Zephyr sources
that implement the group; it re-derives the facts and reports disagreement:

```python
from smpgroup.drift import Sources, check

SRC = Sources.zephyr(
    "firmware/src/mgmt",
    header="acme_mgmt.h",        # declares ACME_CMD_* and the error enum
    dispatch="acme_mgmt.c",      # the [ACME_CMD_*] = {...} handler table
    id_prefix="ACME_CMD_",
    err_prefix="ACME_ERR_",
    err_enum="enum acme_err",
)

report = check(MY_GROUP, SRC)
assert report.ok, str(report)
```

It catches:

- a command id that changed
- a command the firmware declares but your spec doesn't have (or vice versa)
- a command your spec calls live that the firmware never dispatches
- an error code that moved, was renamed, or is missing from either side
- **a request or response key that appears in no handler** — the renamed-field
  case that breaks `extra="forbid"` clients

Drop it in a pytest and your CI now fails on protocol drift instead of your
users discovering it:

```python
def test_no_drift():
    report = check(MY_GROUP, SRC)
    assert report.ok, str(report)
```

The checker parses rather than compiles, so it is deliberately shallow: it
proves a key exists *somewhere in the handlers*, not that it belongs to that
specific command. Shallow still catches the failure that actually happens.

## Design notes

**Stdlib-only where it counts.** `smpgroup.spec` and `smpgroup.drift` import
nothing outside the standard library, so a protocol definition and a firmware
consistency check run in any CI job without a device stack. Only
`smpgroup.build` needs `smpclient`, and `import smpgroup` never pulls it in.

**The spec is the artifact.** It is plain frozen dataclasses — diff it in review,
generate documentation from it, drive a CLI from it, or hand it to another team
as the contract. `Group.describe()` prints a readable summary.

**`meta` is yours.** Commands carry a free-form `meta` mapping for
project-specific facts the library shouldn't know about — access gates, build
flavors, feature flags — with `Group.tagged("key")` to query it.

**Validation at construction.** Duplicate command ids, a group id below the
user range, an unreachable command that declares ops, a write schema on a
read-only command: all rejected when the `Group` is built, not at runtime.

## Status

0.1.0, alpha. Extracted from the HealthyPi 6 firmware project, where it
describes a 40-id management group across four host tools and is checked against
the firmware on every CI run.

The API may change before 1.0. Issues and PRs welcome.
