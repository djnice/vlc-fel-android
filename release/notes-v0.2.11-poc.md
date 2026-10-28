**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.11 (Amlogic S905X4 players):

- **No green picture / missing EL after the player was killed.** When the player process died during FEL playback (force-stop, a crash), the release fences of the EL pictures still on display were signalled only a second later, and never when the next playback started within that second. By then the media codec service had torn the EL decoder down and kept 9 of its buffers (~28 MB) for good. After a few such stops the codec memory ran out (`cma_alloc … failed` in the kernel log), the EL decoder got no buffers, and FEL files showed a uniformly green picture (an empty enhancement layer) or played without the EL at all – until a reboot. Reported by Dune HD with the *DV FEL Test DT DL P7 CMV4.0 4000nits v3* clip. The fences are now signalled at the end of the playback at once (the memory stays referenced until the display gives the picture back), and pictures an earlier playback left with the display go back after a second in any case.
- **For reports:** every playback writes a summary line to the kernel log (`dvfel: session N: el in … out … put … dropped …, bl paired … no_el …`), and libdvfelplay logs the EL decoder every 5 s (`dvfelplay: SC2 5 s: …`, `adb logcat -s dvfelplay`).

libdvfelplay `2026.10.28.2`, kernel module `dvfel 2026.10.28.2` (S905X4); the S928X module (`dvfel 2026.10.23.3`), the VLC and app patches, libmvc3dplay and `mvc3d` unchanged (of the native libraries only `libdvfelplay.so` differs from 0.2.10). The S928X playback is unchanged.

**Tested:** Dune HD Pro Vision 4K (S905X4, firmware `260303_1721_r24`): 8 playbacks of the test clip, each ended by killing the player: the codec memory back to 0 MB after every one (before: +30 MB per kill, the EL gone from the 5th on); playbacks ended normally (BACK) as before; the update of the installed module with the root question. On the TV: the clip's "THIS DEVICE CAN DECODE FEL" picture, also right after a killed playback.

**Known:** the first playback right after the module update showed a green picture for about 2 s at its start (the EL decoder gives no picture for its first ~1.5 s); later playbacks start correctly.

### Install / update
Install `VLC-FEL-0.2.11-poc-armeabi-v7a.apk` over an older version (or new). On an S905X4 it updates the kernel module (root question once). A player already in the state described above needs a reboot once. Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.
