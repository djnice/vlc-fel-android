**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.14 (Amlogic S905X4 players):

- **No green picture at the start of a FEL playback, also after the first EL picture.** 0.2.13 kept the picture black only until the first frame paired with its enhancement layer. The EL decoder then pauses once more (its resolution change), so 1–2 s of frames without the EL could still show green (reported by Dune HD with their `fel_test.mp4`). The S905X4 kernel module now keeps the video muted until 6 frames in a row had their EL, and mutes it again if one is missing within the first 48 (at most 10 s in all): the picture stays black for that moment, then starts with the full FEL composition. Seeks and pause are unchanged.

libdvfelplay `2026.10.29.1`, kernel module `dvfel 2026.10.29.1` (S905X4); the S928X module (`dvfel 2026.10.23.3`), the VLC and app patches, libmvc3dplay and `mvc3d` unchanged (of the native libraries only `libdvfelplay.so` differs from 0.2.13). The S928X playback is unchanged; no Dune HD plugin update.

**Tested:** Dune HD Pro Vision 4K (S905X4) with the stock firmware r25 and `fel_test.mp4`: on the TV a black start, then the correct FEL picture; every frame without its EL at the start was muted. Pause and seeks unchanged.

### Install / update
Install `VLC-FEL-0.2.14-poc-armeabi-v7a.apk` over an older version (or new). On an S905X4 it updates the kernel module (root question once). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.
