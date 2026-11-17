**Proof of concept** – experimental build for enthusiasts. Use at your own risk.

A Dune HD firmware plugin for the Dune HD firmware that brings the dvfel module itself (r25, plugin `s928x_dvfel`, shown as *DV FEL (S928X, S905X4)*). It brings the kernel modules and the libdvfelplay of VLC FEL 0.2.22-poc (`2026.11.17.1`) to the firmware (`config/libdvfelplay.so`), so the firmware, VLC FEL and CineFuze use the same module and library.

New: **libdvfelplay `2026.11.17.1` – S928X: FEL composition about 22% faster, the same picture bit for bit**, also in the Dune HD player, which composes FEL with this library. The GPU composer now imports the frame buffers as `RGB10_A2`, so the separate EL unpack passes are gone. On a Dune HD Pro 8K Plus with a 4K profile 7 FEL film the GPU time per frame went from ~35 to ~27 ms on average (p95 ~38 → ~29 ms), and the frames over one 24p frame time (41.7 ms) from 18 to 1 per 90 s. Every mode composes the same picture as before, checked frame by frame. If the GPU driver refuses the 10-bit import, the library goes on with the former RGBA8 path by itself (`setprop debug.dvfp.rgb10 0` forces it).

The library also brings the HDR10+ to Dolby Vision profile 8.1 conversion of `2026.11.10.1` (`DVFP_MODE_HDR10PLUS`, used only when a player asks for it). The kernel modules are unchanged (S928X `dvfel 2026.10.29.2`, S905X4 `dvfel 2026.10.31.1`), and so is `bin/ctl.sh`.

**Tested:** on a Dune HD Pro 8K Plus (S928X, firmware `261101_0347_r25`), the library copied to `config/libdvfelplay.so` the way the plugin does it: the Dune HD player loaded `2026.11.17.1` and composed a 4K profile 7 FEL sample as `RGB10_A2` (p95 ~31 ms a frame, no timeout, nothing dropped after the start); 6 seeks, each with 1–2 pictures with the mapping only and the picture back at once. The plugin package itself installed through the Dune HD user interface (over the firmware's built-in *DV FEL* plugin): `bin/ctl.sh install` ran, the module stayed loaded, `config/libdvfelplay.so` is this library.

### Install
Copy `dune_plugin_s928x_dvfel-2026.11.17.1-0.2.22.zip` to a drive or network share the player reaches, open it in the player's file browser and confirm (it overwrites an installed *DV FEL* plugin). Install it while nothing plays; the plugin copies the new library to the firmware and marks a restart of the player as needed (the library changed); restart the player afterwards.

Source: this repository, `libdvfelplay/`, `kernel/` and `kernel/sc2/` (GPL-2.0-or-later). The plugin is built on Dune HD's `s928x_dvfel` plugin template (`bin/ctl.sh`, `dune_plugin.xml`).
