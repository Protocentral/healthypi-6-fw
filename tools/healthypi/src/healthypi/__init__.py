# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Host library and CLI for HealthyPi 6.

Layering mirrors the firmware's own: ``hp6`` is the wire/file format, ``smp`` is
the control protocol, and everything above them is a thin adapter.

``healthypi.hp6`` and ``healthypi.smp.catalog`` are **stdlib-only** -- decoding a
recording and checking the protocol catalog against the firmware must never
require a device stack to be installed. Modules that talk to hardware live
behind the ``device`` extra.
"""

# The version lives in pyproject.toml only. Read it back from the installed
# distribution rather than restating it here, where it drifted: a second copy is
# a second thing to forget when bumping for a release tag.
try:
    from importlib.metadata import PackageNotFoundError, version as _version

    __version__ = _version("protocentral-healthypi")
except PackageNotFoundError:  # running from a source tree that was never installed
    __version__ = "0+unknown"

from . import hp6  # noqa: F401  (stdlib-only, always safe to import)

__all__ = ["hp6", "__version__"]
