**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.20:

- **3D MVC from `.ssif` without right eye glitches.** A Blu-ray 3D `.ssif` file interleaves the dependent (right) view and the base view in extents. VLC's ts demux took the right view's picture in progress as complete as soon as the next base view picture started, but an extent can end inside a picture. On Dune HD's BD3D_DEMO disc, 19 of the 4899 right view pictures (about one every 9.5 s) are split that way, and the right eye showed a blocky picture each time. Such a picture is now kept whole until it really ends (VLC patch 0033).

The decoder (edge264-mvc) decodes the whole disc correctly for both eyes; only the demux was at fault. MKV / `.mk3d` files and m2ts files with the dependent view on its own PID were not affected.

The VLC patch 0033 is new. libdvfelplay (`2026.10.31.1`), the kernel modules (S928X `dvfel 2026.10.29.2`, S905X4 `dvfel 2026.10.31.1`), libmvc3dplay, `mvc3d` and the app patches are unchanged. The Dune HD plugin (`2026.10.31.1-0.2.17`) stays current.

**Tested** on a Dune HD Pro 8K Plus (S928X) with the disc's `.ssif`:
- every merged right view picture checked against the disc's separate dependent view file in a simulation of the demux: 4899 of 4899 byte-identical (before: 19 cut short);
- on a 2D TV, the right view shown in 2D with a test build: a glitch about every 10 s before the fix, a clean picture with it;
- this build: the `.ssif` plays in 2D with the dependent view merged and nothing dropped (one picture only at a seek, as before).

Frame packed 3D output with this build has not been tested on a 3D projector yet.

### Install / update
Install `VLC-FEL-0.2.20-poc-armeabi-v7a.apk` over an older version, or as a new install. `README.txt` (Hungarian / English) has the details, notes and uninstall steps.

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause):
- kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`;
- libdvfelplay and libmvc3dplay;
- VLC patches 0001–0033 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`);
- the libvlcjni patch `vlc/jni-patches/0001`;
- the app patches `vlc/android-patches/0001` and `0002` against vlc-android `fc5fa1f`.
