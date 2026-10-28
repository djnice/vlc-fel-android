**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.13 (Amlogic S905X4 players):

- **No green picture at the start of a FEL playback.** At a start the player's first base layer frames come 0.3–4 s before the EL decoder's first picture, and the Dolby Vision core showed those FEL frames without their enhancement layer as a green picture. The S905X4 kernel module now keeps the video muted (the Dolby Vision driver's own video mute) from the start of a playback until its first frame paired with its EL, at most 8 s: the picture stays black for that moment, then starts with the full FEL composition. The end of a playback, and unloading the module, always unmute. Seeks and pause are unchanged (1–4 frames without the EL after a seek, as before). Works with Dolby Vision, HDR10 and SDR output.

libdvfelplay `2026.10.28.4`, kernel module `dvfel 2026.10.28.4` (S905X4); the S928X module (`dvfel 2026.10.23.3`), the VLC and app patches, libmvc3dplay and `mvc3d` unchanged (of the native libraries only `libdvfelplay.so` differs from 0.2.12). The S928X playback is unchanged; no Dune HD plugin update.

**Tested:** Dune HD Pro Vision 4K (S905X4) with the stock firmware r25: on the TV a black start, then the correct FEL picture; with Dolby Vision, HDR10 and SDR output.

### Install / update
Install `VLC-FEL-0.2.13-poc-armeabi-v7a.apk` over an older version (or new). On an S905X4 it updates the kernel module (root question once). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.
