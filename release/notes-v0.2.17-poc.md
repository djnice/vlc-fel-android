**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.17:

- **S905X4: no green or wrong picture at the start of a FEL playback.** The start of a FEL playback stays black until the EL comes steadily. The video driver, however, applies a mute one vsync after it is asked for, so the first picture without its EL, where the mute began, was on the screen for a vsync or two (green, or other wrong colours). It was hidden when the TV changed mode at the start, e.g. with frame rate matching. The mute now begins at the first base layer access unit, before any picture is decoded. It ends at most 10 s later even when no picture reaches the Dolby Vision driver.

libdvfelplay `2026.10.31.1` (it bundles the new S905X4 module; its code is the same as 2026.10.30.1), kernel module `dvfel 2026.10.31.1` (S905X4). The S928X module (`dvfel 2026.10.29.2`), the VLC and app patches, libmvc3dplay and `mvc3d` are unchanged. Of the native libraries, only `libdvfelplay.so` differs from 0.2.16.

Nothing changes on S928X. The Dune HD plugin (`2026.10.30.1-0.2.16`) stays current.

**Tested:** on a Dune HD Pro Vision 4K (S905X4, r25 `261021_0117`) with a TV, SDR output to an SDR menu with no mode change. The former module showed a green picture at every start (2 of 2); the new one showed none (2 of 2). Later runs with Dolby Vision output, with the same mute, showed none either. This APK was checked for FEL playback with the new library. The update of an installed module through the root question was not repeated for this release.

Known, not changed: with Dolby Vision output, a seek on the S905X4 can blank the TV for about a second. The first frames after a seek can come without their EL, because the EL is decoded by a second MediaCodec decoder.

### Install / update
Install `VLC-FEL-0.2.17-poc-armeabi-v7a.apk` over an older version, or as a new install. `README.txt` (Hungarian / English) has the details, notes and uninstall steps.

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause):
- kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`;
- libdvfelplay and libmvc3dplay;
- VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`);
- the libvlcjni patch `vlc/jni-patches/0001`;
- the app patches `vlc/android-patches/0001` and `0002` against vlc-android `fc5fa1f`.
