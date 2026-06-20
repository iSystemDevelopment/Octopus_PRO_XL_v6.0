# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 6.0.x   | Yes       |
| ≤ 5.3.x | No        |

## Reporting a vulnerability

If you discover a security issue in Octopus PRO XL firmware, OctopusApp, or hosted infrastructure:

1. **Do not** open a public GitHub issue for exploitable vulnerabilities.
2. Email **DIODAC ELECTRONICS / iSystem** with:
   - Description and impact
   - Steps to reproduce
   - Firmware/App version
   - Your contact for follow-up

We aim to acknowledge reports within **5 business days**.

## Scope notes

- OctopusApp runs entirely in the browser and communicates via **Web MIDI** to locally connected USB hardware; there is no cloud account or remote command path in v6.0.
- Hosted OctopusApp at `octopus.isystem.app` and product site at `octopus-info.isystem.app` should be served over **HTTPS** with standard TLS configuration.
- Firmware NVS blobs are integrity-checked (CRC); treat user pattern/slot data as local to the device.
