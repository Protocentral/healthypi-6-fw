# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Check a spec against the Zephyr firmware that implements it.

A host-side protocol definition is a transcription of what the firmware does,
and transcriptions rot. This module re-derives the facts from the C sources and
reports where the spec disagrees: command ids, which commands are actually
routed, the error-code table, and whether every declared CBOR key appears in a
handler.

It parses rather than compiles, so it is deliberately shallow -- it proves a key
*exists somewhere in the handlers*, not that it belongs to that command. Shallow
checks still catch the failure that actually happens: a renamed or added field
that nobody propagated to the host.

Stdlib only. Point it at a Zephyr tree with :class:`Sources` and run
:func:`check`; wire the result into pytest or CI.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .spec import Field, Group, Status


@dataclass(frozen=True, slots=True)
class Sources:
    """Where the firmware's truth lives.

    ``header``    -- declares ``<id_prefix><NAME> = 0x…`` and the error enum
    ``dispatch``  -- the handler table, entries like ``[<id_prefix><NAME>] = {…}``
    ``handlers``  -- the .c files that encode/decode CBOR keys
    """

    header: Path
    dispatch: Path
    handlers: tuple[Path, ...]
    id_prefix: str
    err_prefix: str
    #: name of the C enum holding the group's error codes
    err_enum: str | None = None

    @classmethod
    def zephyr(
        cls,
        directory: str | Path,
        *,
        id_prefix: str,
        err_prefix: str,
        header: str | None = None,
        dispatch: str | None = None,
        err_enum: str | None = None,
    ) -> "Sources":
        """Convention: one directory with a header, a dispatch .c, and handlers."""
        d = Path(directory)
        hdrs = sorted(d.glob("*.h"))
        srcs = sorted(d.glob("*.c"))
        hdr = d / header if header else (hdrs[0] if hdrs else d / "missing.h")
        dsp = d / dispatch if dispatch else Path(str(hdr).replace(".h", ".c"))
        return cls(
            header=hdr,
            dispatch=dsp,
            handlers=tuple(p for p in srcs if p != dsp),
            id_prefix=id_prefix,
            err_prefix=err_prefix,
            err_enum=err_enum,
        )

    def exists(self) -> bool:
        return self.header.is_file() and self.dispatch.is_file()


@dataclass(slots=True)
class Report:
    """What the firmware says, and where the spec disagrees with it."""

    declared: dict[str, int] = field(default_factory=dict)
    routed: set[str] = field(default_factory=set)
    errors: dict[str, int] = field(default_factory=dict)
    problems: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.problems

    def __str__(self) -> str:
        if self.ok:
            return (
                f"spec matches firmware: {len(self.declared)} ids declared, "
                f"{len(self.routed)} routed, {len(self.errors)} error codes"
            )
        return "spec/firmware drift:\n" + "\n".join(f"  - {p}" for p in self.problems)


def _declared_ids(src: Sources) -> dict[str, int]:
    text = src.header.read_text()
    pat = re.compile(re.escape(src.id_prefix) + r"([A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)")
    return {m.group(1).lower(): int(m.group(2), 16) for m in pat.finditer(text)}


def _routed(src: Sources) -> set[str]:
    text = src.dispatch.read_text()
    pat = re.compile(r"\[" + re.escape(src.id_prefix) + r"([A-Z0-9_]+)\]\s*=")
    return {m.group(1).lower() for m in pat.finditer(text)}


def _error_codes(src: Sources) -> dict[str, int]:
    text = src.header.read_text()
    if src.err_enum and src.err_enum in text:
        text = text[text.index(src.err_enum) :]
        if "}" in text:
            text = text[: text.index("}")]
    pat = re.compile(re.escape(src.err_prefix) + r"([A-Z0-9_]+)\s*=\s*(\d+)")
    return {m.group(1): int(m.group(2)) for m in pat.finditer(text)}


def _handler_text(src: Sources) -> str:
    return "\n".join(p.read_text() for p in src.handlers if p.is_file())


def _all_fields(fields: tuple[Field, ...]) -> list[Field]:
    out: list[Field] = []
    for f in fields:
        out.append(f)
        out.extend(_all_fields(f.nested))
    return out


def check(group: Group, src: Sources, *, check_keys: bool = True) -> Report:
    """Compare ``group`` against the firmware in ``src``."""
    rep = Report()
    if not src.exists():
        rep.problems.append(f"firmware sources not found at {src.header.parent}")
        return rep

    rep.declared = _declared_ids(src)
    rep.routed = _routed(src)
    rep.errors = _error_codes(src)
    handlers = _handler_text(src) if check_keys else ""

    for cmd in group.commands:
        fw_id = rep.declared.get(cmd.name)
        if fw_id is None:
            rep.problems.append(f"{cmd.name}: in the spec, not declared by the firmware")
            continue
        if fw_id != cmd.cmd_id:
            rep.problems.append(
                f"{cmd.name}: spec id {cmd.cmd_id:#06x} != firmware {fw_id:#06x}"
            )
        is_routed = cmd.status is not Status.UNREACHABLE
        if (cmd.name in rep.routed) != is_routed:
            rep.problems.append(
                f"{cmd.name}: spec says {cmd.status.value}, firmware "
                f"{'routes' if cmd.name in rep.routed else 'does not route'} it"
            )
        if check_keys and cmd.status is Status.LIVE:
            seen = set()
            for op in cmd.ops:
                for group_fields in cmd.schema(op):
                    for f in _all_fields(group_fields):
                        if f.name in seen:
                            continue
                        seen.add(f.name)
                        if f'"{f.name}"' not in handlers:
                            rep.problems.append(
                                f"{cmd.name}: key {f.name!r} appears in no handler"
                            )

    spec_ids = {c.name for c in group.commands}
    for name, cid in sorted(rep.declared.items(), key=lambda kv: kv[1]):
        if name not in spec_ids:
            rep.problems.append(
                f"{name} ({cid:#06x}): declared by the firmware, missing from the spec"
            )
    for name in sorted(rep.routed - spec_ids):
        rep.problems.append(f"{name}: routed by the firmware, missing from the spec")

    for code, (name, _hint) in sorted(group.errors.items()):
        if name not in rep.errors:
            rep.problems.append(f"error {name} ({code}): not in the firmware enum")
        elif rep.errors[name] != code:
            rep.problems.append(
                f"error {name}: spec {code} != firmware {rep.errors[name]}"
            )
    spec_err = {n for n, _ in group.errors.values()}
    for name, code in sorted(rep.errors.items(), key=lambda kv: kv[1]):
        if name not in spec_err:
            rep.problems.append(f"error {name} ({code}): in the firmware, not the spec")

    return rep
