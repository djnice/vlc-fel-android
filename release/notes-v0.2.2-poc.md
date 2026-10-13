**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.2:

- **FEL demo mode**: a split screen comparison for Profile 7 FEL – the left half with the full FEL composition, the right half the base layer as a player without FEL support shows it (profile 8.1). The top of each half shows the brightness measured over the active picture (letterbox bars left out) as a level 1 – max and average of max(R', G', B') in nits – for both pictures, their difference, the RPU's own level 1 and the measured area.
- **Switchable during playback** from the player menu (⋯ → *Dolby Vision demo: on / off*), shown while a FEL composition runs; every file starts without it. Always on: Settings → Advanced → Custom libVLC options: `--mediacodec-dovi-demo`.
- The measurement costs ~3–5 ms GPU time per 4K frame; measured with no dropped or repeated frames at 4K 24p.

Kernel modules unchanged (`dvfel 2026.10.07.1`, `mvc3d 2026.10.12.3`); libdvfelplay `2026.10.13.1`. Everything from 0.2.1 (dual track / dual PID FEL, 3D MVC from m2ts / .ssif) as before.

**Tested:** Dune HD Pro One 8K Plus (R24), Dolby Vision TV: the menu switch on / off during playback, the libVLC option, 4K 24p FEL with the demo mode on and off (0 dropped frames).

### Install / update
Install `VLC-FEL-0.2.2-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0011 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the app patch `vlc/android-patches/0001` against vlc-android `fc5fa1f`.
