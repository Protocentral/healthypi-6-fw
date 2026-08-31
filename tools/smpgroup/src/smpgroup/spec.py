# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Declarative description of a custom MCUmgr management group.

A spec is plain data: command ids, CBOR keys and their types. From one spec you
can generate SMP request/response classes, check the spec against the Zephyr
firmware that implements it, and drive a CLI -- instead of hand-writing each of
those and keeping them in sync by hand.

Stdlib only. Importing a spec never requires an SMP stack, so specs can be
shared, diffed and checked in environments that have no device tooling.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from types import MappingProxyType
from typing import Any, Iterator, Mapping

#: MCUmgr reserves group ids from 64 upward for user-defined groups.
USER_GROUP_ID_START = 64


class T(str, Enum):
    """CBOR types, as a device encodes them."""

    UINT = "uint"
    INT = "int"
    BOOL = "bool"
    TSTR = "tstr"
    BSTR = "bstr"
    MAP = "map"

    @property
    def py(self) -> type:
        return {
            T.UINT: int,
            T.INT: int,
            T.BOOL: bool,
            T.TSTR: str,
            T.BSTR: bytes,
            T.MAP: dict,
        }[self]


class Op(str, Enum):
    """The SMP operation a command answers."""

    READ = "read"
    WRITE = "write"


class Status(str, Enum):
    """How real a command is on the device.

    The distinction matters because a management group's header usually
    declares more ids than the firmware routes. Publishing all of them as API
    is how host tools end up with verbs that can only ever return ENOTSUP.
    """

    #: routed, implemented, returns real data
    LIVE = "live"
    #: routed, but the handler is a stub returning ENOTSUP
    STUB = "stub"
    #: declared in the header with no dispatch entry -- not API
    UNREACHABLE = "unreachable"


@dataclass(frozen=True, slots=True)
class Field:
    """One CBOR key in a request or response map."""

    name: str
    type: T
    doc: str = ""
    optional: bool = False
    #: for ``T.MAP``, the keys of the nested map (empty = opaque map)
    nested: tuple["Field", ...] = ()


@dataclass(frozen=True, slots=True)
class Command:
    """One command within a group."""

    cmd_id: int
    name: str
    ops: tuple[Op, ...] = ()
    status: Status = Status.LIVE
    request: tuple[Field, ...] = ()
    response: tuple[Field, ...] = ()
    #: write-op schemas, when a command implements both ops with different maps
    write_request: tuple[Field, ...] = ()
    write_response: tuple[Field, ...] = ()
    #: group-specific error codes this command can return
    errors: tuple[int, ...] = ()
    doc: str = ""
    #: free-form, project-specific annotations (access gates, build flavors, …)
    meta: Mapping[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if self.status is Status.UNREACHABLE and self.ops:
            raise ValueError(f"{self.name}: unreachable commands must declare no ops")
        if self.status is not Status.UNREACHABLE and not self.ops:
            raise ValueError(f"{self.name}: a routed command must declare an op")
        if Op.WRITE not in self.ops and (self.write_request or self.write_response):
            raise ValueError(f"{self.name}: write schema on a command with no write op")

    def schema(self, op: Op) -> tuple[tuple[Field, ...], tuple[Field, ...]]:
        """``(request_fields, response_fields)`` for one operation."""
        if op is Op.WRITE and (self.write_request or self.write_response):
            return self.write_request, self.write_response
        return self.request, self.response


@dataclass(frozen=True, slots=True)
class Group:
    """A management group: an id, its commands, and its error codes."""

    group_id: int
    name: str
    commands: tuple[Command, ...]
    #: code -> (NAME, human explanation); by convention these start at 256
    errors: Mapping[int, tuple[str, str]] = field(default_factory=dict)
    #: the group's own schema version, if it reports one
    schema_version: int | None = None
    doc: str = ""

    def __post_init__(self) -> None:
        if self.group_id < USER_GROUP_ID_START:
            raise ValueError(
                f"group id {self.group_id} is below the user range "
                f"({USER_GROUP_ID_START}); those are reserved by MCUmgr"
            )
        seen: dict[int, str] = {}
        for c in self.commands:
            if c.cmd_id in seen:
                raise ValueError(
                    f"duplicate command id {c.cmd_id:#06x}: "
                    f"{seen[c.cmd_id]} and {c.name}"
                )
            seen[c.cmd_id] = c.name
        object.__setattr__(self, "errors", MappingProxyType(dict(self.errors)))

    def __iter__(self) -> Iterator[Command]:
        return iter(self.commands)

    def by_id(self, cmd_id: int) -> Command:
        for c in self.commands:
            if c.cmd_id == cmd_id:
                return c
        raise KeyError(f"no command {cmd_id:#06x} in group {self.name}")

    def by_name(self, name: str) -> Command:
        for c in self.commands:
            if c.name == name:
                return c
        raise KeyError(f"no command {name!r} in group {self.name}")

    def routed(self) -> tuple[Command, ...]:
        """Commands the firmware dispatches (live + stub)."""
        return tuple(c for c in self.commands if c.status is not Status.UNREACHABLE)

    def live(self) -> tuple[Command, ...]:
        return tuple(c for c in self.commands if c.status is Status.LIVE)

    def unreachable(self) -> tuple[Command, ...]:
        return tuple(c for c in self.commands if c.status is Status.UNREACHABLE)

    def tagged(self, key: str, value: Any = True) -> tuple[Command, ...]:
        """Commands whose ``meta[key]`` equals ``value``."""
        return tuple(c for c in self.commands if c.meta.get(key) == value)

    def err_name(self, code: int) -> str:
        return self.errors.get(code, ("UNKNOWN", ""))[0]

    def err_hint(self, code: int) -> str:
        return self.errors.get(code, ("UNKNOWN", "unrecognised error code"))[1]

    def describe(self) -> str:
        """A readable summary -- handy in a CLI or a bug report."""
        lines = [f"group {self.group_id} ({self.name})"]
        if self.schema_version is not None:
            lines[0] += f", schema {self.schema_version:#06x}"
        for c in sorted(self.commands, key=lambda x: x.cmd_id):
            ops = "/".join(o.value for o in c.ops) or "-"
            mark = {Status.LIVE: " ", Status.STUB: "~", Status.UNREACHABLE: "x"}[c.status]
            lines.append(f" {mark} {c.cmd_id:#06x} {c.name:<24} {ops}")
        return "\n".join(lines)
