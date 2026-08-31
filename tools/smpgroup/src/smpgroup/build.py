# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Generate SMP request/response classes from a :class:`~smpgroup.spec.Group`.

Why generate rather than hand-write: ``smp``'s response models are pydantic with
``extra="forbid"``, so a response class must declare **every** key the device
sends. Hand-written classes get copied between a test harness, a CLI and an
updater, and then a firmware change breaks whichever copies nobody remembered.
Generating them from one spec makes that failure mode structurally impossible.

Requires ``smpclient``. Import this module only where you actually talk to a
device; :mod:`smpgroup.spec` and :mod:`smpgroup.drift` stay stdlib-only.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping

try:
    from pydantic import BaseModel, ConfigDict, create_model
    from smp import error as smperror
    from smp.message import ReadRequest, ReadResponse, WriteRequest, WriteResponse
except ImportError as exc:  # pragma: no cover
    raise ImportError(
        "smpgroup.build needs the SMP stack: pip install 'smpgroup[smp]'"
    ) from exc

from .spec import Command, Field, Group, Op, Status

_BASES = {
    (Op.READ, "req"): ReadRequest,
    (Op.READ, "resp"): ReadResponse,
    (Op.WRITE, "req"): WriteRequest,
    (Op.WRITE, "resp"): WriteResponse,
}


def _nested_model(name: str, fields: tuple[Field, ...]) -> type[BaseModel]:
    """A pydantic model for a nested CBOR map, forbidding unknown keys too."""
    return create_model(  # type: ignore[call-overload,no-any-return]
        name,
        __config__=ConfigDict(extra="forbid", frozen=True),
        **{f.name: _annotation(f) for f in fields},
    )


def _annotation(f: Field, owner: str = "") -> tuple[Any, Any]:
    if f.nested:
        py: Any = _nested_model(f"{owner}{f.name.title()}", f.nested)
    else:
        py = f.type.py
    return (py | None, None) if f.optional else (py, ...)


def _camel(name: str) -> str:
    return "".join(p.title() for p in name.split("_"))


def _make(
    group_id: int, cmd: Command, op: Op, kind: str, fields: tuple[Field, ...]
) -> type:
    base = _BASES[(op, kind)]
    suffix = "Request" if kind == "req" else "Response"
    cls_name = f"{_camel(cmd.name)}{suffix}"
    model = create_model(
        cls_name,
        __base__=base,
        **{f.name: _annotation(f, _camel(cmd.name)) for f in fields},
    )
    model._GROUP_ID = group_id  # type: ignore[attr-defined]
    model._COMMAND_ID = cmd.cmd_id  # type: ignore[attr-defined]
    return model


@dataclass(frozen=True, slots=True)
class BoundCommand:
    """One command's generated classes."""

    command: Command
    op: Op
    Request: type
    Response: type
    ErrorV1: type
    ErrorV2: type

    @property
    def name(self) -> str:
        return self.command.name

    @property
    def cmd_id(self) -> int:
        return self.command.cmd_id

    def __call__(self, **kwargs: Any) -> Any:
        """Build a request: ``bound.telemetry()`` / ``bound.wifi_set(ssid=…)``."""
        return self.Request(**kwargs)


class BoundGroup:
    """Generated classes for a whole group, addressable by command name.

        g = build(MY_GROUP)
        await client.request(g.telemetry())              # read
        await client.request(g.wifi_set(ssid="x", pw="y"))  # write

    A command implementing both ops appears as ``g.transfer_mode`` (its read)
    and ``g.transfer_mode_write``.
    """

    def __init__(self, group: Group):
        self.group = group
        self._by_name: dict[str, BoundCommand] = {}

        err_v1 = type(
            f"{_camel(group.name)}ErrorV1", (smperror.ErrorV1,), {"_GROUP_ID": group.group_id}
        )

        for cmd in group.commands:
            if cmd.status is Status.UNREACHABLE:
                continue  # never generate a class the device cannot answer
            for op in cmd.ops:
                req_fields, resp_fields = cmd.schema(op)
                err_v2 = type(
                    f"{_camel(cmd.name)}ErrorV2",
                    (smperror.ErrorV2[int],),
                    {"_GROUP_ID": group.group_id, "_COMMAND_ID": cmd.cmd_id},
                )
                Response = _make(group.group_id, cmd, op, "resp", resp_fields)
                Request = _make(group.group_id, cmd, op, "req", req_fields)
                Request._Response = Response  # type: ignore[attr-defined]
                Request._ErrorV1 = err_v1  # type: ignore[attr-defined]
                Request._ErrorV2 = err_v2  # type: ignore[attr-defined]

                key = cmd.name
                if len(cmd.ops) > 1 and op is Op.WRITE:
                    key = f"{cmd.name}_write"
                bound = BoundCommand(cmd, op, Request, Response, err_v1, err_v2)
                self._by_name[key] = bound
                setattr(self, key, bound)

    def __getitem__(self, name: str) -> BoundCommand:
        return self._by_name[name]

    def __contains__(self, name: str) -> bool:
        return name in self._by_name

    def __iter__(self):
        return iter(self._by_name.values())

    def names(self) -> tuple[str, ...]:
        return tuple(self._by_name)

    def __repr__(self) -> str:
        return (
            f"<BoundGroup {self.group.name} id={self.group.group_id} "
            f"commands={len(self._by_name)}>"
        )


def build(group: Group) -> BoundGroup:
    """Generate SMP classes for every routed command in ``group``."""
    return BoundGroup(group)


def format_error(
    group: Group,
    response: Any,
    *,
    foreign: Mapping[int, Mapping[int, tuple[str, str]]] | None = None,
) -> str:
    """Human-readable text for an SMP error response.

    Two shapes reach here, and they use different namespaces:

    * ``{"rc": n}`` -- the protocol-wide ``MGMT_ERR`` set (EINVAL, ENOTSUP …),
      which a handler returns by returning non-zero. ``smp`` types it, so it
      names itself.
    * ``{"err": {"group": g, "rc": n}}`` -- ``n`` belongs to **group g's** own
      namespace, and a group is free to define 4 however it likes. That group is
      not always ours: a client issuing stock commands (``os`` echo, datetime,
      reset) over the same connection gets os-group errors back. Naming one of
      those from this group's table yields a confident lie, or -- when the
      number is simply unused here -- "UNKNOWN (4)" for what the device meant as
      os/RTC_NOT_SET. So this group's table is consulted only when the ids match.

    ``foreign`` optionally supplies tables for other groups, keyed by group id,
    which adds the hint text; without it a foreign code is still named whenever
    ``smp`` typed the rc as that group's enum.
    """
    err = getattr(response, "err", None)
    if err is not None:
        code = int(err.rc)
        raw_group = getattr(err, "group", None)
        gid = group.group_id if raw_group is None else int(raw_group)

        if gid != group.group_id:
            name, hint = (foreign or {}).get(gid, {}).get(code, ("", ""))
            # ``smp`` types an error's rc as its own group's IntEnum, so the
            # name is usually there for free even with no table supplied.
            name = name or getattr(err.rc, "name", None) or "UNKNOWN"
            return f"{name} (group {gid}, rc {code}){f' -- {hint}' if hint else ''}"

        name = group.err_name(code)
        hint = group.err_hint(code)
        return f"{name} ({code}){f' -- {hint}' if hint else ''}"
    rc = getattr(response, "rc", None)
    if rc is not None:
        rsn = getattr(response, "rsn", None)
        return f"{getattr(rc, 'name', rc)} ({int(rc)}){f' -- {rsn}' if rsn else ''}"
    return repr(response)


def is_error(response: Any) -> bool:
    return hasattr(response, "err") or hasattr(response, "rc")
