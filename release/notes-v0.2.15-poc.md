**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.15 (Amlogic S928X players):

- **FEL is no longer lost for a whole playback after a quick change.** When one FEL playback followed another within a second or two, the S928X kernel module could pass every frame through without the enhancement layer until the end, also without the demo mode. This happened with a Blu-ray disc handed over from the Dune HD menu player, or with a quick restart. The module waited for its queues to empty before it reallocated the former playback's buffers, while every frame passed through into those queues. The 125 ms lead keeps 1–3 frames there, so they never emptied. Now only the queued frames of its own buffers count.
- The demo mode no longer shows for a second at a start when the previous playback ended with it on.

libdvfelplay `2026.10.29.2`, kernel module `dvfel 2026.10.29.2` (S928X); the S905X4 module (`dvfel 2026.10.29.1`), the VLC and app patches, libmvc3dplay and `mvc3d` unchanged (of the native libraries only `libdvfelplay.so` differs from 0.2.14).

**On Dune HD r25** the firmware brings its own module, and VLC FEL uses it: install the **Dune HD plugin `2026.10.29.2-0.2.15`** (release `dune-plugin-2026.10.29.2`) to get the fixed module there. On other firmware VLC FEL installs the new module with the first FEL file.

**Tested:** Dune HD Pro 8K Plus (S928X, r25) with The Godfather UHD Blu-ray folder: back-to-back restarts in the Dune HD player and the hand-over to another player, with the module's debug statistics. The stuck state came up and cleared within about a second.

### Install / update
Install `VLC-FEL-0.2.15-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0020 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the libvlcjni patch `vlc/jni-patches/0001`, the app patches `vlc/android-patches/0001`, `0002` against vlc-android `fc5fa1f`.
