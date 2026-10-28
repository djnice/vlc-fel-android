**Proof of concept** – experimental build for enthusiasts. Use at your own risk.

A Dune HD firmware plugin for the Dune HD firmware that brings the dvfel module itself (r25, plugin *S928X DV FEL*, `s928x_dvfel`). It brings the libdvfelplay of VLC FEL 0.2.11-poc (`2026.10.28.2`) to the firmware (`config/libdvfelplay.so`), so the firmware, VLC FEL and CineFuze use the same library. The kernel module is unchanged: `dvfel 2026.10.23.3`.

The library's changes are for the S905X4 path (no green picture after a killed player, a log line every 5 s); on the S928X the playback is as with the plugin `2026.10.25.1-0.2.10`.

**Tested:** the library on a Dune HD Pro Vision 4K (S905X4) in VLC FEL 0.2.11. The plugin package itself was not installed on a player (same `bin/ctl.sh` and module as `2026.10.25.1-0.2.10`, only `libdvfelplay.so` and `dune_plugin.xml` differ).

### Install
Copy `dune_plugin_s928x_dvfel-2026.10.28.1-0.2.11.zip` to a drive or network share the player reaches, open it in the player's file browser and confirm (it overwrites the installed *S928X DV FEL* plugin). Install it while nothing plays; the plugin copies the new library to the firmware and marks a restart of the player as needed (the library changed); restart the player afterwards.

Source: this repository, `libdvfelplay/` and `kernel/` (GPL-2.0-or-later). The plugin is built on Dune HD's `s928x_dvfel` plugin template (`bin/ctl.sh`, `dune_plugin.xml`).
