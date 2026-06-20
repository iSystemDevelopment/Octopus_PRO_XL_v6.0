# Contributing

Thank you for your interest in Octopus PRO XL.

This repository is **proprietary** (see [LICENSE](./LICENSE)). Public visibility does not grant a license to fork, redistribute, or commercialize the code without written permission from **DIODAC ELECTRONICS / iSystem**.

## How to help

### Bug reports

Open a [GitHub Issue](../../issues/new?template=bug_report.md) and include:

- Firmware version (`6.0.00` or build from `code_info.h` → `SYSTEM_FW_VERSION`)
- Hardware variant (ESP32-S3 module, flash/PSRAM if known)
- Whether **OctopusApp** was connected ([octopus.isystem.app](https://octopus.isystem.app))
- Product docs: [octopus-info.isystem.app](https://octopus-info.isystem.app) · [GitHub](https://github.com/iSystemDevelopment/Octopus_PRO_XL_v6.0) · [Facebook](https://www.facebook.com/diodac.co.uk/)
- Steps to reproduce and expected vs actual behaviour
- Serial log excerpt if applicable

### Feature requests

Open a [Feature request](../../issues/new?template=feature_request.md). Describe the **use case** (live performance, studio, education) and whether it affects firmware, OctopusApp, or both.

### Pull requests

Pull requests are accepted **by invitation only** unless prior agreement exists with DIODAC ELECTRONICS. If you have been authorized to contribute:

1. Branch from `main`: `feature/short-description` or `fix/short-description`
2. Keep changes focused; match existing code style in the touched modules
3. Update `CHANGELOG.md` under `[Unreleased]` or the target version
4. Do not bump `SETTINGS_VERSION` or SysEx command IDs without updating `code_info.h`, `sysex.h`, and `OctopusApp.html` together
5. Reference the related issue in the PR description

### Developer reference

| File | Purpose |
|------|---------|
| `code_info.h` | Architecture manifest, core assignment, protocol rules |
| `sysex.h` | SysEx command ID SSOT |
| `patches.h` | Parameter apply-path SSOT |
| `Upgrade.md` | Maintainer roadmap |

## Code of conduct

Be respectful and constructive. Issues and discussions about this product should stay on-topic and professional.
