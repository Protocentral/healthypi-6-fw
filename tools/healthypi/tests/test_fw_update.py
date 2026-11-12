# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""`healthypi fw update` against a fake device -- the ESP32-C6 step above all.

The fake sits at the SMPClient level, *under* the real
:class:`~healthypi.transport.serial_smp.Connection`, so every request in these
tests passes through the code that resolves the catalog's per-request timeout.
That is deliberate: the 5 s ``espfw_*`` budget silently not reaching the client
is the failure this project has hit twice, and a fake that replaced Connection
would hide it.

What the fake models is the device contract in docs/MCUMGR_COMMANDS.md §5.12 --
the states, the error codes, the busy rule, the C6 being held in reset at boot
and by every M7 reset -- not the firmware's code. It has not been checked
against hardware; nor has the updater.
"""

from __future__ import annotations

import asyncio
import hashlib
from types import SimpleNamespace

import pytest

pytest.importorskip("smpclient", reason="the device stack is not installed")
pytest.importorskip("cryptography", reason="signing needs cryptography")

import cbor2  # noqa: E402
from smp.header import OP, Flag, Header, Version  # noqa: E402

from healthypi import fw  # noqa: E402
from healthypi.fw import esp32c6 as C6  # noqa: E402
from healthypi.fw import update as U  # noqa: E402
from healthypi.fw.errors import UpdateError  # noqa: E402
from healthypi.smp import catalog  # noqa: E402
from healthypi.smp.group64 import g  # noqa: E402
from healthypi.transport.serial_smp import Connection  # noqa: E402

from test_fw import fake_c6_image  # noqa: E402


# ----------------------------------------------------------------- replies ---


def _ok(name: str, **payload):
    """A real generated success reply (some carry an `err` field, so a stand-in
    object would read as an error)."""
    bound = g[name]
    op = OP.READ_RSP if bound.op == catalog.Op.READ else OP.WRITE_RSP
    body = cbor2.dumps(payload, canonical=True)
    hdr = Header(op=op, version=Version.V2, flags=Flag(0), length=len(body),
                 group_id=64, sequence=0, command_id=bound.cmd_id)
    return bound.Response.loads(bytes(hdr) + body)


def _gerr(code: int):
    return SimpleNamespace(err=SimpleNamespace(group=64, rc=code))


def _rc(code: int):
    return SimpleNamespace(rc=code, rsn=None)


ENOTSUP = 8
RESET_TYPE = {"ResetWrite", "enter_recovery_write", "conn_disable"}


# -------------------------------------------------------------- the device ---


class FakeDevice:
    """A HealthyPi 6 as group 64 and the stock img/os groups describe it."""

    def __init__(self, pub, *, m7="1.0.0", m4="1.0.0", c6="1.0.0", espfw=True,
                 c6_on=False, outcome="done", prepare_polls=2, no_ota=False,
                 version_polls=1):
        self.pub = pub
        self.m7, self.m4, self.c6 = m7, m4, c6
        self.espfw = espfw              # this M7 firmware has espfw_*
        self.link = 2 if c6_on else 0   # conn_status.link
        self.c6_ver_known = c6_on
        self.version_polls = version_polls
        self.outcome = outcome          # done | rolled_back | timeout
        self.prepare_polls = prepare_polls
        self.no_ota = no_ota
        self.m4_bound = True
        self.slot0 = b"\x00" * 32
        self.pending_m7 = None          # (version, hash, espfw) after ImageStatesWrite
        self.m7_upload = b""
        self.next_m7 = None             # what the uploaded M7 image "is"
        self.st = dict(st=0, len=0, rx=0, err=0, c6_err=0, c6_st=255, img=255,
                       other=255, slot=255)
        self.slot = 0
        self.buf = bytearray()
        self.sha = b""
        self.last = (None, None)
        self.polls = 0
        self.chunk_faults: dict[int, list] = {}
        self.commit_reply_lost = False
        self.log: list[tuple[str, float | None]] = []
        self.violations: list[str] = []
        self.commits = 0
        self.resets = 0

    # -- state ------------------------------------------------------------
    @property
    def busy(self) -> bool:
        return 1 <= self.st["st"] <= 5 or self.st["img"] == 1

    def _c6_fields(self):
        if self.link != 2:
            return dict(c6_st=255, img=255, other=255, slot=255, ver="")
        return dict(c6_st=self.st["c6_st"] if self.st["c6_st"] != 255 else 0,
                    img=self.st["img"] if self.st["img"] != 255 else 0,
                    other=self.st["other"], slot=self.slot,
                    ver=self.c6 if self.c6_ver_known else "")

    def status(self):
        self.polls += 1
        s = self.st
        if self.link == 2 and not self.c6_ver_known:
            self.version_polls -= 1
            self.c6_ver_known = self.version_polls <= 0
        if s["st"] == 1:
            self.prepare_polls -= 1
            if self.prepare_polls <= 0:
                if self.no_ota:
                    s.update(st=8, err=3)
                    self.link = 2
                else:
                    s.update(st=2, c6_st=1)
        elif s["st"] in (3, 4, 5, 9):
            self._advance()
        c6 = self._c6_fields()
        img = c6["img"]
        return _ok("espfw_status", st=s["st"], len=s["len"], rx=s["rx"], err=s["err"],
                   c6_err=s["c6_err"], c6_st=c6["c6_st"], img=img, other=c6["other"],
                   slot=c6["slot"], ver=c6["ver"], valid=img == 0, pend=img == 1,
                   busy=1 <= s["st"] <= 5 or img == 1)

    def _advance(self):
        s = self.st
        self.after_commit += 1
        n = self.after_commit
        if s["st"] == 3 and n >= 1:
            s.update(st=4, c6_st=2)
        elif s["st"] == 4 and n >= 3:
            # the C6 restarted into the new slot, pending verification
            self.slot ^= 1
            self.c6 = self.new_c6
            s.update(st=5, img=1, other=0, c6_st=0)
        elif s["st"] in (5, 9) and n >= 6:
            if self.outcome == "done":
                s.update(st=6, img=0, other=0)
            elif self.outcome == "rolled_back":
                self.slot ^= 1
                self.c6 = self.old_c6
                s.update(st=7, err=14, img=0, other=4)
            elif self.outcome == "timeout" and n >= 25:
                s.update(st=9, err=13)          # stays pending: busy

    # -- dispatch ---------------------------------------------------------
    def handle(self, req, timeout_s):
        cls = type(req).__name__
        if getattr(type(req), "_GROUP_ID", None) == 64:
            cmd = catalog.BY_ID[type(req)._COMMAND_ID]
            write = req.header.op == OP.WRITE
            name = cmd.name + ("_write" if write and cmd.ops == (catalog.Op.READ, catalog.Op.WRITE) else "")
            kw = req.model_dump(exclude={"header", "version", "sequence", "smp_data"})
        else:
            name, kw = cls, {}
        self.log.append((name, timeout_s))
        if name in RESET_TYPE and self.busy:
            self.violations.append(name)
            return _gerr(274)
        return getattr(self, "h_" + name)(req, **kw)

    # stock groups
    def h_ImageStatesRead(self, req):
        return SimpleNamespace(images=[SimpleNamespace(slot=0, hash=self.slot0)])

    def h_ImageStatesWrite(self, req):
        self.pending_m7 = (self.next_m7, req.hash)
        return SimpleNamespace()

    def h_ResetWrite(self, req):
        self.resets += 1
        if self.pending_m7:
            ver, h = self.pending_m7
            self.m7, self.slot0 = ver["m7"], h
            self.espfw = ver["espfw"]
            self.pending_m7 = None
            self.m4_bound = False          # MCUboot's copy delays the M7 past the M4 bind
        else:
            self.m4_bound = True
        # every M7 reset holds the C6 in reset
        self.link = 0
        self.c6_ver_known = False
        self.st = dict(st=0, len=0, rx=0, err=0, c6_err=0, c6_st=255, img=255,
                       other=255, slot=255)
        return SimpleNamespace()

    # group 64
    def h_fw_versions(self, req):
        return _ok("fw_versions", m7fw=self.m7, m4fw=self.m4 if self.m4_bound else "",
                   espfw=self.c6 if (self.link == 2 and self.c6_ver_known) else "",
                   mod_a_fw="", mod_b_fw="")

    def h_stream_stop(self, req):
        return _ok("stream_stop")

    def h_m4fw_status(self, req):
        return _ok("m4fw_status", st=0, len=0, rx=0, err=0, rst=False, sig=True)

    def h_m4fw_begin(self, req, **kw):
        self.m4_len = kw["len"]
        return _ok("m4fw_begin")

    def h_m4fw_chunk(self, req, off, data):
        return _ok("m4fw_chunk", off=off + len(data))

    def h_m4fw_commit(self, req):
        self.m4 = self.next_m4
        return _ok("m4fw_commit", rst=True)

    def h_conn_status(self, req):
        return _ok("conn_status", link=self.link, radios=0, state=0, rssi=0, ssid="",
                   ip="", ble_adv=False, ble_conn=False)

    def h_conn_enable(self, req, radios):
        self.link = 2
        return _ok("conn_enable", ok=True)

    def h_conn_disable(self, req):
        self.link = 0
        self.c6_ver_known = False
        return _ok("conn_disable", ok=True)

    def h_espfw_status(self, req):
        if not self.espfw:
            return _rc(ENOTSUP)
        return self.status()

    def h_espfw_begin(self, req, len, sha, sig):  # noqa: A002 -- the wire name
        if not self.espfw:
            return _rc(ENOTSUP)
        if self.busy:
            return _gerr(270)
        from cryptography.hazmat.primitives import hashes
        from cryptography.hazmat.primitives.asymmetric import ec
        from cryptography.hazmat.primitives.asymmetric.utils import (
            Prehashed,
            encode_dss_signature,
        )

        try:
            der = encode_dss_signature(int.from_bytes(sig[:32], "big"),
                                       int.from_bytes(sig[32:], "big"))
            self.pub.verify(der, sha, ec.ECDSA(Prehashed(hashes.SHA256())))
        except Exception:  # noqa: BLE001
            self.st.update(st=8, err=1, len=len, rx=0)
            return _gerr(268)
        self.st.update(st=1, len=len, rx=0, err=0, c6_err=0)
        self.buf = bytearray(len)
        self.sha = bytes(sha)
        self.last = (None, None)
        self.link = 2                       # the service powers the C6 itself
        self.prepare_polls = max(self.prepare_polls, 1)
        return _ok("espfw_begin")

    def h_espfw_chunk(self, req, off, data):
        s = self.st
        faults = self.chunk_faults.get(off)
        fault = faults.pop(0) if faults else None
        if s["st"] != 2:
            return _gerr(256)
        if fault == 272:
            return _gerr(272)
        if fault == "timeout":
            raise TimeoutError("no reply")
        if fault == 273:
            s.update(st=8, err=4, c6_err=0x04)
            return _gerr(273)
        if fault == "rewind":
            # the device's idea of `next` is one chunk behind the host's
            lo, ln = self.last
            s["rx"] = lo
            self.last = (None, None)
            return _gerr(260)
        if self.last == (off, len(data)) and off + len(data) == s["rx"]:
            return _ok("espfw_chunk", off=s["rx"])          # idempotent resend
        if off != s["rx"] or off + len(data) > s["len"]:
            return _gerr(260)
        self.buf[off:off + len(data)] = data
        s["rx"] = off + len(data)
        self.last = (off, len(data))
        if fault == "lost":
            raise TimeoutError("reply lost")
        return _ok("espfw_chunk", off=s["rx"])

    def h_espfw_commit(self, req):
        self.commits += 1
        s = self.st
        if s["st"] != 2:
            return _gerr(256)
        if s["rx"] != s["len"]:
            s.update(st=8, err=8)
            return _gerr(275)
        if hashlib.sha256(bytes(self.buf)).digest() != self.sha:
            s.update(st=8, err=7)
            return _gerr(268)
        s.update(st=3, c6_st=2)
        self.after_commit = 0
        self.old_c6, self.new_c6 = self.c6, self.image_version
        if self.commit_reply_lost:
            raise TimeoutError("reply lost")
        return _ok("espfw_commit")

    def h_espfw_abort(self, req):
        s = self.st
        if 3 <= s["st"] <= 5:
            return _gerr(270)
        s.update(st=0, err=9 if s["st"] in (1, 2) else 0)
        return _ok("espfw_abort")

    # helpers for assertions
    def names(self):
        return [n for n, _ in self.log]


class FakeClient:
    def __init__(self, dev):
        self.dev = dev

    async def request(self, req, timeout_s=None):
        return self.dev.handle(req, timeout_s)

    async def upload(self, image, slot=0, upgrade=False):
        self.dev.log.append(("img_upload", None))
        self.dev.m7_upload = bytes(image)
        yield len(image)

    async def __aexit__(self, *exc):
        return None


class FakeTarget(U.Target):
    def __init__(self, dev):
        super().__init__(port="/dev/fake")
        self.dev = dev

    async def connect(self):
        return Connection(client=FakeClient(self.dev), port="/dev/fake", transport=None)


# ---------------------------------------------------------------- fixtures ---


@pytest.fixture
def key(tmp_path_factory):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec

    path = tmp_path_factory.mktemp("k") / "k.pem"
    k = ec.generate_private_key(ec.SECP256R1())
    path.write_bytes(k.private_bytes(serialization.Encoding.PEM,
                                     serialization.PrivateFormat.PKCS8,
                                     serialization.NoEncryption()))
    return path


@pytest.fixture
def pub(key):
    from cryptography.hazmat.primitives import serialization

    return serialization.load_pem_private_key(key.read_bytes(), password=None).public_key()


@pytest.fixture(autouse=True)
def fast(monkeypatch):
    """Virtual clock for the 1 Hz polling; no reset settle time; the M7 image
    hash is the file digest (the fake images carry no MCUboot TLV)."""
    clock = [0.0]

    async def sleep(s):
        clock[0] += s

    monkeypatch.setattr(C6, "_now", lambda: clock[0])
    monkeypatch.setattr(C6, "_sleep", sleep)
    monkeypatch.setattr(U, "_RESET_SETTLE_S", 0)
    monkeypatch.setattr(U, "mcuboot_image_hash", lambda img: hashlib.sha256(img).digest())
    return clock


def make_bundle(tmp_path, key, *, m7=None, m4=None, c6="1.1.0", c6_size=3000):
    specs = []
    if m7:
        p = tmp_path / "m7.bin"
        p.write_bytes(b"\xa5" * 1500 + m7.encode())
        specs.append(fw.ImageSpec("m7", p, m7, "mcumgr-img"))
    if m4:
        p = tmp_path / "m4.bin"
        p.write_bytes(b"\x5a" * 700 + m4.encode())
        specs.append(fw.ImageSpec("m4", p, m4, "hpi-g64", sign=True))
    if c6:
        p = tmp_path / "c6.bin"
        p.write_bytes(fake_c6_image(c6, c6_size))
        specs.append(fw.ImageSpec("esp32c6", p, "", "hpi-g64-esp", sign=True))
    return fw.Bundle(fw.create(tmp_path / "b.zip", specs, release="1.0.1", hw_rev=["v5"],
                               key_path=key, created="2026-11-12T00:00:00Z"))


def run(bundle, dev, key, **kw):
    lines: list[str] = []
    if bundle.images().get("esp32c6"):
        dev.image_version = bundle.images()["esp32c6"]["version"]
    if bundle.images().get("m7"):
        dev.next_m7 = {"m7": bundle.images()["m7"]["version"],
                       "espfw": getattr(dev, "espfw_after_m7", True)}
    if bundle.images().get("m4"):
        dev.next_m4 = bundle.images()["m4"]["version"]
    res = asyncio.run(U.apply_bundle(bundle, FakeTarget(dev), pubkey=key,
                                     log=lines.append, progress=lambda o, t: None, **kw))
    return res, "\n".join(lines)


def run_err(bundle, dev, key, **kw):
    with pytest.raises(UpdateError) as exc:
        run(bundle, dev, key, **kw)
    return exc.value


def chunks(dev):
    return [n for n in dev.names() if n == "espfw_chunk"]


# --------------------------------------------------------------- the tests ---


def test_happy_path_c6_only(tmp_path, key, pub):
    dev = FakeDevice(pub, c6="1.0.0", c6_on=True)
    res, out = run(make_bundle(tmp_path, key), dev, key)
    assert res.exit_code == 0 and res.ok and res.applied == ["esp32c6"]
    assert dev.c6 == "1.1.0" and dev.commits == 1
    assert bytes(dev.buf) == make_bundle(tmp_path, key).read_image("esp32c6")
    n = dev.names()
    assert n.count("espfw_begin") == 1
    assert n.index("espfw_begin") < n.index("espfw_chunk") < n.index("espfw_commit")
    assert "OK — running from slot 1 and VALID" in out
    assert dev.violations == []


def test_the_catalog_timeout_reaches_the_transport_for_every_espfw_request(tmp_path, key, pub):
    """The 5 s budget is the catalog's, applied by Connection -- the updater
    passes none. A 2.5 s default reaching the client is the bug this guards."""
    dev = FakeDevice(pub, c6="1.0.0", c6_on=True)
    run(make_bundle(tmp_path, key), dev, key)
    espfw = [(n, t) for n, t in dev.log if n.startswith("espfw_")]
    assert {n for n, _ in espfw} >= {"espfw_status", "espfw_begin", "espfw_chunk",
                                     "espfw_commit"}
    assert all(t == catalog.ESPFW_TIMEOUT_S == 5.0 for _, t in espfw), espfw
    # and nothing else picked up a timeout it did not ask for
    assert all(t is None for n, t in dev.log if n in ("fw_versions", "conn_status"))


def test_connection_resolves_the_timeout_and_an_explicit_one_wins():
    seen = []

    class Client:
        async def request(self, req, timeout_s=None):
            seen.append((type(req).__name__, timeout_s))

    conn = Connection(client=Client(), port="/dev/null")
    asyncio.run(conn.request(g.espfw_chunk(off=0, data=b"x")))
    asyncio.run(conn.request(g.device_info()))
    asyncio.run(conn.request(g.espfw_status(), timeout_s=9.0))
    assert seen == [("EspfwChunkRequest", 5.0), ("DeviceInfoRequest", None),
                    ("EspfwStatusRequest", 9.0)]


def test_272_resends_the_same_chunk(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True)
    b = make_bundle(tmp_path, key)
    step = C6.chunk_size(Connection(client=None, port="x"))
    dev.chunk_faults = {step: [272, 272]}
    res, _ = run(b, dev, key)
    assert res.exit_code == 0
    sent = [kw for kw in dev.log if kw[0] == "espfw_chunk"]
    assert len(sent) == -(-3000 // step) + 2
    assert bytes(dev.buf) == b.read_image("esp32c6")


def test_a_lost_chunk_reply_resends_harmlessly(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True)
    b = make_bundle(tmp_path, key)
    step = C6.chunk_size(Connection(client=None, port="x"))
    dev.chunk_faults = {0: ["lost"], 2 * step: ["timeout"]}
    res, _ = run(b, dev, key)
    assert res.exit_code == 0 and bytes(dev.buf) == b.read_image("esp32c6")


def test_260_reads_rx_and_resumes_there(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True)
    b = make_bundle(tmp_path, key)
    step = C6.chunk_size(Connection(client=None, port="x"))
    dev.chunk_faults = {3 * step: ["rewind"]}
    res, out = run(b, dev, key)
    assert res.exit_code == 0 and bytes(dev.buf) == b.read_image("esp32c6")
    assert f"device expects +{2 * step}" in out


def test_273_starts_again_from_begin(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True)
    b = make_bundle(tmp_path, key)
    step = C6.chunk_size(Connection(client=None, port="x"))
    dev.chunk_faults = {2 * step: [273]}
    res, out = run(b, dev, key)
    assert res.exit_code == 0
    assert dev.names().count("espfw_begin") == 2 and dev.commits == 1
    assert "starting again from begin" in out


def test_commit_is_never_resent_when_its_reply_is_lost(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True)
    dev.commit_reply_lost = True
    res, out = run(make_bundle(tmp_path, key), dev, key)
    assert dev.commits == 1 and dev.names().count("espfw_commit") == 1
    assert res.exit_code == 0 and dev.c6 == "1.1.0"
    assert "not resending" in out


def test_rolled_back(tmp_path, key, pub):
    dev = FakeDevice(pub, c6="1.0.0", c6_on=True, outcome="rolled_back")
    res, out = run(make_bundle(tmp_path, key), dev, key)
    assert res.exit_code == 5 and not res.ok and "esp32c6" not in res.applied
    assert "ROLLED BACK to 1.0.0" in out and "fine on its old firmware" in out


def test_timeout_leaves_the_c6_powered_and_says_how_to_recheck(tmp_path, key, pub, fast):
    dev = FakeDevice(pub, c6_on=False, outcome="timeout")
    res, out = run(make_bundle(tmp_path, key), dev, key, force=True)
    assert res.exit_code == 6
    assert "left POWERED" in out and "healthypi fw esp32c6-status" in out
    assert dev.violations == [] and "conn_disable" not in dev.names()
    assert dev.link == 2 and dev.busy


def test_enotsup_then_m7_update_then_c6_in_one_run(tmp_path, key, pub):
    """A unit whose M7 predates espfw_*: one run installs the M7, resets (twice,
    to pair the cores), and only then updates the C6 -- on the new M7."""
    dev = FakeDevice(pub, m7="1.0.0", m4="1.0.0", c6="1.0.0", espfw=False)
    b = make_bundle(tmp_path, key, m7="1.0.1", m4="1.0.1")
    res, out = run(b, dev, key)
    n = dev.names()
    assert res.exit_code == 0, out
    assert res.applied == ["m4", "m7", "esp32c6"]
    assert dev.m7 == "1.0.1" and dev.m4 == "1.0.1" and dev.c6 == "1.1.0"
    assert n.count("ResetWrite") == 2
    last_reset = max(i for i, x in enumerate(n) if x == "ResetWrite")
    assert n.index("espfw_begin") > last_reset
    assert n.index("m4fw_commit") < n.index("img_upload") < last_reset
    assert "runs on the M7 this bundle installs" in out
    assert dev.violations == []


def test_enotsup_without_a_newer_m7_is_refused_up_front(tmp_path, key, pub):
    dev = FakeDevice(pub, espfw=False)
    e = run_err(make_bundle(tmp_path, key), dev, key)
    assert "no ESP32-C6 update commands" in str(e) and "M7 has to be updated first" in str(e)
    assert "espfw_begin" not in dev.names() and "conn_enable" not in dev.names()


def test_no_ota_needs_a_usb_flash(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True, no_ota=True)
    e = run_err(make_bundle(tmp_path, key), dev, key)
    assert e.code == 7
    assert "predates the USB update path" in str(e) and "flash_hp6.sh" in str(e)
    assert "healthypi conn enable --radios none" in str(e)


def test_identical_version_is_skipped_and_force_reapplies(tmp_path, key, pub):
    b = make_bundle(tmp_path, key, c6="1.1.0")
    dev = FakeDevice(pub, c6="1.1.0", c6_on=True)
    res, out = run(b, dev, key)
    assert res.skipped == ["esp32c6"] and "espfw_begin" not in dev.names()
    assert "nothing to do" in out

    dev = FakeDevice(pub, c6="1.1.0", c6_on=True)
    res, _ = run(b, dev, key, force=True)
    assert res.applied == ["esp32c6"] and dev.commits == 1


def test_version_unknown_is_learned_with_radios_off_and_the_c6_put_back(tmp_path, key, pub):
    """The C6 is held in reset at boot, so its version is unknown. It is powered
    with no radio to ask, and put back into reset when the answer is "same"."""
    b = make_bundle(tmp_path, key, c6="1.1.0")
    dev = FakeDevice(pub, c6="1.1.0", c6_on=False, version_polls=2)
    res, out = run(b, dev, key)
    n = dev.names()
    assert res.skipped == ["esp32c6"] and "espfw_begin" not in n
    assert ("conn_enable", None) in dev.log
    assert n.index("conn_enable") < n.index("conn_disable")
    assert dev.link == 0
    assert "radios off" in out and "back in reset" in out


def test_version_unknown_then_update_puts_the_c6_back_afterwards(tmp_path, key, pub):
    dev = FakeDevice(pub, c6="1.0.0", c6_on=False)
    res, _ = run(make_bundle(tmp_path, key), dev, key)
    n = dev.names()
    assert res.exit_code == 0 and dev.c6 == "1.1.0"
    assert n.index("conn_enable") < n.index("espfw_begin") < n.index("conn_disable")
    assert dev.link == 0 and dev.violations == []


def test_downgrade_is_refused_unless_forced(tmp_path, key, pub):
    b = make_bundle(tmp_path, key, c6="1.0.0")
    dev = FakeDevice(pub, c6="1.1.0", c6_on=True)
    e = run_err(b, dev, key)
    assert "OLDER than the installed 1.1.0" in str(e) and "--force" in str(e)
    assert "no downgrade protection" in str(e)
    assert "espfw_begin" not in dev.names()

    dev = FakeDevice(pub, c6="1.1.0", c6_on=True)
    res, _ = run(b, dev, key, force=True)
    assert res.applied == ["esp32c6"] and dev.c6 == "1.0.0"


def test_downgrade_learned_late_is_refused_too(tmp_path, key, pub):
    b = make_bundle(tmp_path, key, c6="1.0.0")
    dev = FakeDevice(pub, c6="1.1.0", c6_on=False)
    e = run_err(b, dev, key)
    assert "OLDER" in str(e) and "espfw_begin" not in dev.names()
    assert dev.link == 0     # powered only to ask, then put back


def test_full_bundle_order_and_no_reset_while_busy(tmp_path, key, pub):
    dev = FakeDevice(pub, m7="1.0.0", m4="1.0.0", c6="1.0.0")
    b = make_bundle(tmp_path, key, m7="1.0.1", m4="1.0.1")
    res, out = run(b, dev, key)
    n = dev.names()
    assert res.exit_code == 0
    assert "plan: m4 → m7 → reset ×2" in out and out.index("reset ×2") < out.index("→ esp32c6")
    first_c6 = min(i for i, x in enumerate(n) if x.startswith("espfw_") and x != "espfw_status")
    assert "ResetWrite" not in n[first_c6:] and "enter_recovery_write" not in n
    # the C6 was off after the reset, so it was powered to read its version and
    # put back afterwards -- once the device said busy: false, never before
    assert n[first_c6:].count("conn_disable") == 1 and n[-1] == "conn_disable"
    assert dev.violations == [] and not dev.busy


def test_busy_at_start_refuses_everything(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True)
    dev.st.update(st=2, len=100, rx=10)
    b = make_bundle(tmp_path, key, m7="1.0.1", m4="1.0.1")
    e = run_err(b, dev, key)
    assert "update is in progress" in str(e) and "busy: no" in str(e)
    assert not {"m4fw_begin", "img_upload", "ResetWrite", "espfw_begin"} & set(dev.names())


def test_a_pending_c6_image_also_blocks(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=True)
    dev.st.update(img=1)
    e = run_err(make_bundle(tmp_path, key, m4="1.0.1", c6=None), dev, key)
    assert "pending verification" in str(e)


def test_bad_signature_is_explained(tmp_path, key, pub, monkeypatch):
    from cryptography.hazmat.primitives.asymmetric import ec

    dev = FakeDevice(ec.generate_private_key(ec.SECP256R1()).public_key(), c6_on=True)
    e = run_err(make_bundle(tmp_path, key), dev, key)
    assert "REFUSED the image's signature" in str(e) and "Nothing was sent" in str(e)
    assert "espfw_chunk" not in dev.names()


def test_c6_failure_after_m7_says_what_was_done(tmp_path, key, pub):
    dev = FakeDevice(pub, m4="1.0.0", c6_on=True, no_ota=True)
    e = run_err(make_bundle(tmp_path, key, m4="1.0.1"), dev, key)
    assert e.code == 7 and str(e).startswith("M4 updated above; the ESP32-C6 step")


def test_old_bundle_c6_entry_is_skipped_never_applied(tmp_path, key, pub):
    import json
    import zipfile

    src = make_bundle(tmp_path, key, m4="1.0.1")
    zf = zipfile.ZipFile(src.path)
    members = {n: zf.read(n) for n in zf.namelist()}
    d = json.loads(members["manifest.json"])
    e = d["images"]["esp32c6"]
    e["transport"] = "esp-ota-http"
    del e["sig"]
    blob = json.dumps(d, indent=2, sort_keys=True).encode()
    members["manifest.json"] = blob
    members["manifest.sig"] = fw.sign_digest_raw(hashlib.sha256(blob).digest(), key)
    old = tmp_path / "old.zip"
    with zipfile.ZipFile(old, "w") as out:
        for k, v in members.items():
            out.writestr(k, v)

    dev = FakeDevice(pub, m4="1.0.0", c6_on=True)
    res, out = run(fw.Bundle(old), dev, key)
    assert res.applied == ["m4"] and "esp32c6" in res.skipped
    assert "predates the USB update path" in out
    assert not [n for n in dev.names() if n.startswith("espfw_") and n != "espfw_status"]

    dev = FakeDevice(pub, c6_on=True)
    e = run_err(fw.Bundle(old), dev, key, only="esp32c6")
    assert "predates the USB update path" in str(e)


def test_a_bundle_without_a_c6_leaves_it_alone(tmp_path, key, pub):
    dev = FakeDevice(pub, m4="1.0.0", c6_on=True)
    res, out = run(make_bundle(tmp_path, key, m4="1.0.1", c6=None), dev, key)
    assert res.applied == ["m4"] and "carries no C6 image" in out
    e = run_err(make_bundle(tmp_path, key, m4="1.0.1", c6=None), FakeDevice(pub), key,
                only="esp32c6")
    assert "no esp32c6 image" in str(e)


def test_only_each_processor(tmp_path, key, pub):
    b = make_bundle(tmp_path, key, m7="1.0.1", m4="1.0.1")
    for only, applied in (("m4", ["m4"]), ("m7", ["m7"]), ("esp32c6", ["esp32c6"])):
        dev = FakeDevice(pub, c6_on=True)
        res, _ = run(b, dev, key, only=only)
        assert res.applied == applied, only
        if only == "esp32c6":
            assert "ResetWrite" not in dev.names()


def test_dry_run_writes_nothing_and_powers_nothing(tmp_path, key, pub):
    dev = FakeDevice(pub, c6_on=False)
    res, out = run(make_bundle(tmp_path, key, m4="1.0.1"), dev, key, dry_run=True)
    assert "dry run" in out and "plan: m4 → reset → esp32c6" in out
    assert "running version unknown" in out and "chunks of" in out
    assert not {"m4fw_begin", "conn_enable", "espfw_begin", "ResetWrite"} & set(dev.names())


def test_the_m7_reset_is_not_sent_while_busy(tmp_path, key, pub):
    """Defence in depth: if a C6 update was started elsewhere mid-run, the
    reset that installs the M7 is not even asked for."""
    dev = FakeDevice(pub, m4="1.0.0", c6_on=True)
    real = dev.h_m4fw_commit

    def commit_then_busy(req):
        dev.st.update(img=1)             # someone else's C6 image now pending
        return real(req)

    dev.h_m4fw_commit = commit_then_busy
    e = run_err(make_bundle(tmp_path, key, m4="1.0.1", c6=None), dev, key)
    assert "cannot reset" in str(e) and "staged" in str(e)
    assert "ResetWrite" not in dev.names() and dev.violations == []


# ------------------------------------------------- acceptance-suite cases ---


def _suite_conn(dev):
    return Connection(client=FakeClient(dev), port="/dev/fake")


def test_suite_skips_espfw_cases_when_the_commands_are_absent(pub):
    from healthypi.testing import suite as S

    dev = FakeDevice(pub, espfw=False)
    dev.h_lock_state = lambda req: _ok("lock_state", state=1)
    dev.h_diag_run_selftest = lambda req: _rc(ENOTSUP)
    caps = asyncio.run(S.probe(_suite_conn(dev), g, U_is_error()))
    assert caps.signed_build and not caps.espfw
    assert "ENOTSUP" in caps.unmet((S.REQ_ESPFW,))


def U_is_error():
    from healthypi.smp.group64 import is_error

    return is_error


def _case(name):
    from healthypi import testing

    return {c.name: c for c in testing.registry()}[name].fn


def _run_case(name, dev):
    from healthypi.smp.group64 import fmt_error, is_error

    return asyncio.run(_case(name)(_suite_conn(dev), g, is_error, fmt_error))


def test_espfw_status_case_passes_and_catches_an_unsafe_busy(pub):
    from healthypi.testing.suite import Outcome

    dev = FakeDevice(pub, c6_on=True)
    assert _run_case("espfw_status", dev).outcome is Outcome.PASS

    class Lying(FakeDevice):
        def status(self):
            r = super().status()
            return _ok("espfw_status", **{**r.model_dump(include={
                "st", "len", "rx", "err", "c6_err", "c6_st", "img", "other", "slot",
                "ver", "valid", "pend"}), "busy": False})

    liar = Lying(pub, c6_on=True)
    liar.st.update(img=1)
    res = _run_case("espfw_status", liar)
    assert res.outcome is Outcome.FAIL and "roll it back" in res.detail


def test_bad_signature_case(pub):
    from healthypi.testing.suite import Outcome

    dev = FakeDevice(pub, c6_on=False)
    res = _run_case("espfw rejects a bad signature", dev)
    assert res.outcome is Outcome.PASS, res.detail
    assert "268" in res.detail and dev.st["st"] == 0 and dev.link == 0  # cleared, C6 off

    class Accepts(FakeDevice):
        def h_espfw_begin(self, req, **kw):
            self.st.update(st=1)
            return _ok("espfw_begin")

    res = _run_case("espfw rejects a bad signature", Accepts(pub))
    assert res.outcome is Outcome.FAIL and "ACCEPTED" in res.detail

    busy = FakeDevice(pub, c6_on=True)
    busy.st.update(st=2)
    assert _run_case("espfw rejects a bad signature", busy).outcome is Outcome.SKIP


# ----------------------------------------- reset-type verbs check `busy` ---


@pytest.fixture
def busy_dev(pub, monkeypatch):
    """A device mid-C6-update, reachable through the CLI's own connect paths."""
    from contextlib import asynccontextmanager

    from healthypi.transport import serial_smp

    dev = FakeDevice(pub, c6_on=True)
    dev.st.update(st=2, len=1000, rx=300)

    @asynccontextmanager
    async def session(port=None, **kw):
        yield _suite_conn(dev)

    async def connect(port=None, **kw):
        return _suite_conn(dev)

    monkeypatch.setattr(serial_smp, "session", session)
    monkeypatch.setattr(serial_smp, "connect", connect)
    dev.h_enter_recovery = lambda req: _ok("enter_recovery", av=True, armed=False)
    dev.h_enter_recovery_write = lambda req, **kw: _ok("enter_recovery_write", armed=True, rst=True)
    return dev


@pytest.mark.parametrize("argv", [
    ["device", "reset"],
    ["conn", "disable"],
    ["fw", "enter-recovery"],
])
def test_reset_type_verbs_explain_instead_of_sending(busy_dev, capsys, argv):
    from healthypi.cli.main import main

    rc = main(argv + ["--port", "/dev/fake"])
    err = capsys.readouterr().err
    assert rc == 2
    assert "update is in progress" in err and "busy: no" in err
    assert not RESET_TYPE & set(busy_dev.names())     # never asked
    assert busy_dev.violations == []


def test_a_274_from_the_device_is_explained(busy_dev, capsys, monkeypatch):
    """If the check races (or the C6 turns pending between the two requests),
    the device's 274 is still explained, not printed as a raw code."""
    from healthypi.cli.main import main
    from healthypi.fw import esp32c6

    async def not_busy(*a):
        return None

    monkeypatch.setattr(esp32c6, "busy_reason", not_busy)
    rc = main(["conn", "disable", "--port", "/dev/fake"])
    err = capsys.readouterr().err
    assert rc == 2 and "274 ESP_UPDATE_ACTIVE" in err and "roll it back" in err


def test_esp32c6_status_verb(pub, monkeypatch, capsys):
    from contextlib import asynccontextmanager

    from healthypi.cli.main import main
    from healthypi.transport import serial_smp

    dev = FakeDevice(pub, c6_on=True)

    @asynccontextmanager
    async def session(port=None, **kw):
        yield _suite_conn(dev)

    monkeypatch.setattr(serial_smp, "session", session)
    assert main(["fw", "esp32c6-status", "--port", "x"]) == 0
    out = capsys.readouterr().out
    assert "session   IDLE" in out and "version 1.0.0" in out and "busy      no" in out


def test_update_exit_code_reaches_the_shell(tmp_path, key, pub, monkeypatch, capsys):
    from healthypi.cli.main import main

    dev = FakeDevice(pub, c6_on=True, outcome="rolled_back")
    b = make_bundle(tmp_path, key)
    dev.image_version = "1.1.0"
    monkeypatch.setattr(U, "Target", lambda **kw: FakeTarget(dev))
    rc = main(["fw", "update", "--bundle", str(b.path), "--pubkey", str(key),
               "--port", "/dev/fake"])
    assert rc == 5
