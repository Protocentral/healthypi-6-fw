# Copyright (c) 2026 ProtoCentral Electronics
# SPDX-License-Identifier: MIT

"""Firmware bundles and device update.

``bundle`` is stdlib-only apart from ``cryptography`` for signing, so a bundle
can be inspected and its digests checked on a machine with no device stack.
``update`` and ``recovery`` need ``smpclient`` and are imported lazily by the
CLI, which is why they are not re-exported here.
"""

from __future__ import annotations

from .bundle import (
    APPLY_ORDER,
    Bundle,
    BundleError,
    ImageSpec,
    create,
    sha256_file,
)
from .keys import sign_digest_raw, verify_digest_raw

__all__ = [
    "APPLY_ORDER",
    "Bundle",
    "BundleError",
    "ImageSpec",
    "create",
    "sha256_file",
    "sign_digest_raw",
    "verify_digest_raw",
]
