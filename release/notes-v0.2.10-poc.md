**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.10:

- **Amlogic S905X4 Dune HD players** (e.g. Dune HD Pro Vision 4K, R24 firmware). The S905X4's Dolby Vision core composes the enhancement layer itself, and the Dune firmware's Dolby Vision driver already has the FEL fixes – but an Android player decodes with MediaCodec, which outputs the base layer only. VLC FEL now decodes the EL with a second decoder and a new kernel module for the S905X4 (`dvfel 2026.10.25.4`, `kernel/sc2/`) hands each EL picture to the driver, paired with its base layer frame (by pts and RPU CRC). No GPU work; the RPU stays profile 7. The module is bundled next to the S928X one and installed the same way (the root question once, a boot script).
- **Demo mode**: S928X only. On the S905X4 the SoC composes, so the player menu item reads *Dolby Vision demo: not available* and says why.
- **S928X**: the bundled module is now `dvfel 2026.10.23.3`, the module of the Dune HD plugin of 2026-10-23 (its fixes are for the firmware's own player; VLC FEL plays as with `2026.10.15.1`). On the Dune HD **r25** firmware, which brings the module itself (plugin *S928X DV FEL*), VLC FEL uses the firmware's module and installs nothing: the firmware unloads any other module version at every boot, so VLC FEL would otherwise ask for root at every start.
- **Dual PID m2ts: no FEL drop outs** (VLC patch `0020`, by @a1d4r, #2). The ts demux holds a base layer picture until its enhancement layer has come; after an EL PES with no length two pictures were held, and the loop that let them go on read the DTS of an access unit it had just merged and freed. Once that memory was reused, a picture went on before its EL: played without its EL and RPU (*Dolby Vision: no enhancement layer for dts …*), at other pictures in every run.

libdvfelplay `2026.10.25.2`, kernel modules `dvfel 2026.10.23.3` (S928X) and `dvfel 2026.10.25.4` (S905X4), VLC patch `0020`, the app patch `vlc/android-patches/0001` changed; libmvc3dplay and `mvc3d` unchanged.

**Known on the S905X4:** after a seek 1–4 frames show without the EL; during a pause the EL decoder logs a few fence wait timeouts. 3D MVC is S928X only (the `mvc3d` module does not load on the S905X4; with a 3D display the installer may ask for root once and then report a failure, MVC plays in 2D). R22 on the S905X4 not tested; r25 is not out for the S905X4 yet.

**Tested:** Dune HD Pro Vision 4K (S905X4, firmware `260303_1721_r24`) on the TV: playback (all frames paired with their EL once the first EL key picture is in), pause, seeks, the installation with the root question, the update of an installed module, the load at boot. Dune HD Pro 8K Plus (S928X, r25): two dual PID m2ts (one with every EL PES without a length, a cut of an Ultra HD Blu-ray main movie) played with every picture merged with its EL (0 *no enhancement layer*), no late pictures, the firmware's module kept (nothing installed, no root question), the demo mode on.

### Install / update
Install `VLC-FEL-0.2.10-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.
