# Compatibility overlays

This directory contains narrowly scoped source overlays for the exact upstream
revisions pinned in `config/west.yml`.

| Project | Pinned revision | Overlay |
| --- | --- | --- |
| `badjeff/zmk-feature-split-esb` | `1f4cd4558bb9e0626ec2507f334f239862af859d` | ESB wire source/sequence/tick metadata, per-source state, queue-safe retry IDs, heartbeat and benchmark hooks |
| `carrefinho/prospector-zmk-module` | `ed98221f3b52b7066dbb10ba3af8a29150b93a5a` | ESB-only Operator output widget and replacement for the unconditional BLE observer |
| `badjeff/sdk-nrf` | `9b3d2623fdcd9c0fd0284f860beea924568c9826` | Per-pipe PRX ACK count/cancellation; other pipes retain RX/PID state |

The copied files retain their upstream SPDX headers. Files derived from Nordic
code remain under `LicenseRef-Nordic-5-Clause`; ZMK-derived files retain the MIT
header. New Totem-only files use the MIT license.

During an ESB CMake configure, the root module copies these files over the
matching files in the pinned west checkouts. Before the first copy, it saves the
upstream file beside it with the suffix `.totem-esb-upstream`. A later standard
BLE configure restores the Prospector and SDK originals from those backups, so building
ESB and then BLE in the same west workspace does not leave the rollback build
using the ESB-only display code.

These overlays are revision-specific. When changing an upstream SHA,
compare every overlaid file with the new upstream version and re-run all sixteen
GitHub Actions builds before accepting the update.
