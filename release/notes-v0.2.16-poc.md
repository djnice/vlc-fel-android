**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.16:

- **No more Dolby Vision stutter for a whole playback.** At some starts every 3rd–4th frame was lost until the end of the playback (about 15 of 24 frames per second); a restart cleared it. Profile 8, MEL and FEL alike. Every picture's display time is snapped to a vsync. The phase of the desired times to the vsync grid is random per start; when it fell at the border between two vsync slots, the ~1 ms timestamp jitter of the file put frame after frame now in one slot, now in the other, and two frames shared a vsync. Near the border a frame now keeps the previous frame's cadence.

libdvfelplay `2026.10.30.1`; the kernel modules (S928X `dvfel 2026.10.29.2`, S905X4 `dvfel 2026.10.29.1`), the VLC and app patches, libmvc3dplay and `mvc3d` unchanged (of the native libraries only `libdvfelplay.so` differs from 0.2.15).

**On Dune HD r25** the Dune HD player times its Dolby Vision pictures with the firmware plugin's copy of this library: install the **Dune HD plugin `2026.10.30.1-0.2.16`** (release `dune-plugin-2026.10.30.1`) to fix it there too.

**Tested:** Dune HD Pro 8K Plus (S928X, r25) with a profile 8 film, measured with the video composer's counters: starts forced to the slot border gave 10–11 frames per second with the former library and 24 with the new one (3 of 3 each); 13 of 13 normal starts played 24 frames per second. These measurements were made with CineFuze, which uses the same library; this VLC FEL APK itself was not measured.

### Install / update
Install `VLC-FEL-0.2.16-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.
