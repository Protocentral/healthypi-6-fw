# Contributing to HealthyPi 6 Firmware

Thank you for your interest in contributing to HealthyPi 6! This document provides guidelines for contributing to the project.

## Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [Making Changes](#making-changes)
- [Coding Standards](#coding-standards)
- [Submitting Changes](#submitting-changes)
- [Reporting Issues](#reporting-issues)

## Code of Conduct

This project adheres to a [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code.

## Getting Started

1. Fork the repository on GitHub
2. Clone your fork locally
3. Set up the development environment (see below)
4. Create a branch for your changes
5. Make your changes
6. Test your changes
7. Submit a pull request

## Development Setup

### Prerequisites

- Zephyr RTOS v4.4 with the Zephyr SDK
- ESP-IDF v6.0.1 (for ESP32-C6 builds, in the separate healthybridge-esp32 repo)
- ST-Link or J-Link programmer
- HealthyPi 6 hardware

### Environment Setup

```bash
# Clone the repository
west init -m https://github.com/Protocentral/healthypi-6-fw --mr main hpi6-workspace
cd hpi6-workspace
west update

# Activate Zephyr environment
source ~/zephyrproject/.venv/bin/activate

# Build and test
scripts/build.sh m4 && scripts/build.sh m7
scripts/flash.sh all
```

### Build Commands

There are four scripts, and they take a target:

| Command | Description |
|---------|-------------|
| `scripts/build.sh m7 [dev\|prod]` | M7 application + display UI → `build/m7` |
| `scripts/build.sh m4` | M4 algorithm core → `build/m4` |
| `scripts/build.sh signed [dev\|prod]` | MCUboot + signed M7 → `build/m7s` |
| `scripts/build.sh esp32` | ESP32-C6 (external HealthyBridge repo) |
| `scripts/flash.sh [all\|m7\|m4\|signed\|factory\|esp32]` | Flash over SWD |
| `scripts/release.sh` | Production build + `.hpifw` bundle |
| `source scripts/env.sh` | Just the environment (venv, board, paths) |

**Never call `west build` directly.** The scripts select the board variant, the
conf fragments, the DT overlays and the output directory; a raw invocation
silently drops the display fragment and the signed layer.

**A `build.sh m7` image cannot be updated in the field** — no bootloader, no
MCUmgr img group, no recovery entry. That is fine for development and wrong for
anything that leaves the building. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#9-firmware-update-and-recovery).

## Making Changes

### Branch Naming

Use descriptive branch names:
- `feature/add-ecg-filter` - New features
- `fix/ipc-timeout` - Bug fixes
- `docs/update-readme` - Documentation
- `refactor/cleanup-module` - Code refactoring

### Commit Messages

Follow conventional commit format:

```
type(scope): short description

Longer description if needed.

Fixes #123
```

Types: `feat`, `fix`, `docs`, `refactor`, `test`, `chore`

Examples:
```
feat(ecg): add digital filter for 50Hz noise rejection
fix(ipc): resolve message timeout in high-load conditions
docs(readme): update build instructions for Zephyr 4.4
```

## Coding Standards

### C Code Style

- Use 4-space indentation (no tabs)
- Opening braces on same line
- Descriptive variable names
- Document public APIs with Doxygen-style comments

```c
/**
 * @brief Send data batch via IPC
 * @param type Message type (HPI_IPC_MSG_TYPE_*)
 * @param data Pointer to data buffer
 * @param len Length of data in bytes
 * @return 0 on success, negative errno on failure
 */
int hpi_ipc_send(uint8_t type, const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return -EINVAL;
    }
    // Implementation
}
```

### Critical Patterns

1. **IPC Callbacks**: Never block - use `k_msgq_put()` + `k_work_submit()`
2. **Message Size**: Keep IPC messages under 512 bytes
3. **Format Specifiers**: plain `%u`/`%d` for `uint32_t`/`int32_t` (both cores are
   32-bit ARM, where `uint32_t` *is* `unsigned int`); `%zu` for `size_t`, `%p` for pointers
4. **LVGL**: Use direct property setters, never `static lv_style_t`

### File Headers

All source files should include the SPDX license header:

```c
/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 */
```

**MIT is the default for new code written for this project.** Use `Apache-2.0`
*only* on a file that is derived from, or co-copyright with, upstream Zephyr, ST,
Nordic, NXP or Espressif — board definitions, devicetree bindings and panel
drivers, typically — because those cannot be relicensed. The per-file identifier
is authoritative; see [LICENSE.md](LICENSE.md).

## Submitting Changes

### Pull Request Process

1. Ensure `scripts/build.sh m7`, `scripts/build.sh m4` **and `scripts/build.sh signed`**
   all build without warnings. The signed flavor is the one that ships and the one
   that breaks; a green dev build says nothing about it.
2. Test on actual hardware if possible
3. Update documentation if needed
4. Create a pull request with:
   - Clear title describing the change
   - Description of what and why
   - Reference to related issues

### Pull Request Template

```markdown
## Description
Brief description of changes.

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Documentation update
- [ ] Refactoring

## Testing
- [ ] Tested on HealthyPi 6 hardware
- [ ] Builds with `scripts/build.sh m7`, `m4` and `signed`
- [ ] `tools/ci/check_layer_deps.sh` passes
- [ ] No new warnings

## Related Issues
Fixes #123
```

### Review Process

- All PRs require at least one review
- CI must pass (builds must succeed)
- Address review comments promptly

## Reporting Issues

### Bug Reports

Include:
- Board revision (v5 is the current target; v2-v4 are legacy)
- Firmware version
- Steps to reproduce
- Expected vs actual behavior
- Console logs if available

### Feature Requests

Include:
- Use case description
- Proposed solution (if any)
- Alternatives considered

## Questions?

- Open a GitHub Discussion for general questions
- Check existing issues before creating new ones
- Wi-Fi, BLE, MQTT and dashboard questions belong in
  [`healthybridge-esp32`](https://github.com/Protocentral/healthybridge-esp32)

Thank you for contributing!
