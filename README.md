# admin_system (Plugify)

Administration core: admins, groups, flags/permissions, punishments (ban/mute/gag/silence), the admin menu and the plugin-facing IAdminApi. A port of the Metamod:Source plugin 'admin_system' by Pisex; the database layout (as_groups, as_admins, as_admins_servers, as_punishments, configurable prefix) is kept identical so Metamod and Plugify servers can share one database.

Plugify port (fork) of **cs2-admin_system** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.0
- Plugify dependencies: s2sdk (>=2.18.2), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `admin_system-1.2.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/admin_system`.

## License
GPL-3.0, see LICENSE.

