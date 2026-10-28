**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.12 (Amlogic S905X4 players):

- **FEL with the stock Dune HD firmware (R24 and r25).** The Dolby Vision driver (`aml_media.ko`) of the stock S905X4 firmware lacks four fixes that dual-layer FEL needs. The S905X4 kernel module now applies them itself at run time, with kprobes on the driver's instructions, each found by the code around it:
  - the driver cleared its BL/EL pairing table on every frame (`vf->omx_index == 0`) without returning the EL pictures, so the EL decoder ran out of buffers after a few pictures and the EL was missing from then on;
  - two `DOLBY_PATH_CTRL` settings in the core1 setup left out the S905X4 (the SC2 part of the known Amlogic FEL fix);
  - the EL picture was programmed into the VD1 layer instead of VD2, so the EL showed as a distorted strip over noise instead of going into the Dolby Vision core.

  Earlier versions were tested on a player that ran our own patched driver, which hid the missing fixes. Reported by Dune HD (black picture with the *DV FEL Test DT DL P7 CMV4.0 4000nits v3* clip). On a driver that already has the fixes, or an unknown build, a fix finds no match and stays off; the kernel log lists the ones applied (`dvfel: fix … at …`).
- **r25 for the S905X4:** the module needs no other change for r25 (same kernel, symbols and boot hook). Unlike on the S928X, r25 brings no dvfel module for the S905X4, so VLC FEL installs its own there.

libdvfelplay `2026.10.28.3`, kernel module `dvfel 2026.10.28.3` (S905X4); the S928X module (`dvfel 2026.10.23.3`), the VLC and app patches, libmvc3dplay and `mvc3d` unchanged (of the native libraries only `libdvfelplay.so` differs from 0.2.11). The S928X playback is unchanged.

**Tested:** Dune HD Pro Vision 4K (S905X4) with the stock firmware `261021_0117_r25`: the module update with the root question and the load at boot (all fixes applied); the test clip as mp4, mkv and m2ts with pause, resume and seeks: all frames paired with their EL once the first EL key picture is in (no EL for a few frames at the start and after a seek), every EL picture returned, the codec memory back to 0 MB afterwards. On the TV: the clip's "THIS DEVICE CAN DECODE FEL" picture, also after seeks. Not tested: the stock R24 firmware (the same driver build as r25 apart from an unrelated change; checked offline).

**Known:** during a pause the EL decoder logs a few "wait fence" timeouts (as before).

### Install / update
Install `VLC-FEL-0.2.12-poc-armeabi-v7a.apk` over an older version (or new). On an S905X4 it updates the kernel module (root question once). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.
