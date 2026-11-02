# vip_test (Plugify)

[VIP] VIP Test: !viptest / mm_viptest (and !testvip / mm_testvip) hands a free VIP period (vip_test.ini: group, time, timeout) to a player who does not have VIP yet, at most once per timeout seconds; a running cooldown prints when the next grant becomes available. Ported from the Pisex VIP core module VIP_Test (vip_test). Requires the vip core.

Plugify port (fork) of **VIP Test** by Pisex.
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), vip (>=1.0.0), permissions (>=1.3.1, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `vip_test-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_test`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

