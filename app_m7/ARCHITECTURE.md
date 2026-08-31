# M7 application architecture

Layered, publish/subscribe, plugin-style. This describes the M7 application's
internal structure; for the system picture — the three processors, the data path
and the cross-core contracts — see
[`../../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

## Layers (data flows up to the bus only)

```
L5  ui/ + control/(mcumgr_hpi, shell_hpi, events)      presentation & control
L4  services/(stream recording telemetry diag config power connectivity)
L3  core/sample_bus + bus/(zbus)                        the spine
L2  core/acquisition + healthylink/                     producers
L1  drivers/ + DT (HAL)                                 + platform/ipc → M4 algos
```

## Directories

| Dir | Layer | Responsibility |
|-----|-------|----------------|
| `core/` | L2/L3 | sample bus, channel registry, onboard acquisition producers |
| `bus/` | L3 | zbus channels, event catalog |
| `services/` | L4 | independent services behind fixed APIs, each its own thread |
| `control/` | L5 | MCUmgr group-64 adapter, shell adapter, event-notify adapter |
| `transport/` | — | USB composite function table, CDC stream/SMP/shell, MSC |
| `healthylink/` | L2 | module provider framework, arbiter, supervisor, modules |
| `ui/` | L5 | LVGL screens — bus subscribers + service clients only |
| `platform/` | L1 | watchdog, OpenAMP IPC to M4, boot, fs_mount |

## The one rule (CI-enforced)

`core/` and `healthylink/` may include **only** `bus/`/`core/` and HAL headers.
They must **never** include `services/`, `control/`, `transport/`, or `ui/` headers.
Producers publish; they do not know who consumes. A CI grep fails the build on violation.

## Adapter-parity

Every capability is implemented once in a `services/` module. `control/mcumgr_hpi`
(for CLI/Studio over CDC 1) and `control/shell_hpi` (for humans, dev/factory) are
**thin adapters** over the same service APIs — no duplicated logic.
