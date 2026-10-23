**Proof of concept** – experimental build for enthusiasts. Use at your own risk.

A Dune HD firmware plugin with the kernel module `dvfel 2026.10.23.3`, for the Dune HD firmware that brings the dvfel module itself (r25, plugin `s928x_dvfel`). It replaces the module of the firmware's plugin; its `libdvfelplay` (`2026.10.16.1`) is the one of the firmware.

Fixed, in the firmware's own player:

- **Blu-ray menu mode: FEL from the start of the main movie and after seeks.** The player keeps one video composer session across the titles of a disc (menu, intros, main movie). When the FEL composition started, the composer already played to the display, the route through dvfel was set but never became active, and no frame was composed ("FEL composition started", 0 frames). dvfel now joins the running path; the frames the display held from before go back to the composer.
- **No stutter in a MEL / SDR title after a FEL one** (e.g. a 4K 59.94p MEL film after a FEL intro). dvfel stayed in the path as long as the composer session lasted; without a compositor it copied every frame through VICP (~25 ms per 2160p frame: about 40% of the frames came too late and were dropped). dvfel now leaves the running path when the compositor is gone, so such titles play exactly as without dvfel; it joins again for the next FEL title.

The VLC FEL and CineFuze apps are unchanged (their bundled module stays `2026.10.15.1`; they use the newer module when it is loaded).

**Tested:** Dune HD Pro 8K Plus, firmware r25, the firmware's player in full Blu-ray menu mode, on the TV.
- *Sonic the Hedgehog (2020)* UHD (P7 FEL): FEL from the start of the main movie (dvfel joined at the FEL logo, left at the MEL clip, joined at the movie) and after 6 seeks (5–60 min): ~377 frames composed per 15 s each (24p), no decoder fence waits.
- *Gemini Man (2019)* UHD (4K 59.94p, P7 MEL, after its FEL intro): before 1841 of 4337 frames dropped; now dvfel is out of the path in the movie, 1–3 drops per 30 s at 62.5 fps, no visible stutter.
- Installing from the player's file browser and the load at boot were tested with the same package built with an earlier module (`2026.10.23.1`).

Known: at the start of a 4K 60p MEL title the player drops pictures for a few seconds (about 2 s worth); this also happens with dvfel out of the path, so it is not from this module.

### Install
Copy `dune_plugin_s928x_dvfel-2026.10.23.3-0.2.10.zip` to a drive or network share the player reaches, open it in the player's file browser and confirm (it overwrites the installed *S928X DV FEL* plugin). Install it while nothing plays; the plugin swaps the loaded module and loads its own at every boot.

Source: this repository, `kernel/` (GPL-2.0-or-later). The plugin is built on Dune HD's `s928x_dvfel` plugin template (`bin/ctl.sh`, `dune_plugin.xml`).
