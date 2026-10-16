**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.7:

- **No stutter in the FEL demo mode.** The demo mode measures the level 1 of both halves of the picture for its readout. It read the measurement back from the GPU every frame, synchronously: the composition had to wait for the GPU in the middle of every frame. On a heavy 4K 24p FEL film whose composition alone takes 35–40 ms of the 41.7 ms a frame has, that made it 54 ms (95th percentile), 30–39 of every 100 frames missed their vsync and were shown as the plain base layer or repeated. The readout only changes 4 times a second, so now every 6th frame is measured, and its result is taken a few frames later when the GPU has finished it, without waiting. On the same film with the demo mode on: no frame shown as the base layer, none repeated, 95th percentile 42 ms (the 125 ms lead time absorbs the few longer frames).

libdvfelplay `2026.10.16.1` is new; kernel modules (`dvfel 2026.10.15.1`, `mvc3d`), VLC patches and the app patch unchanged. Everything from 0.2.6 as before.

**Tested:** Dune HD Pro One 8K Plus (R24), 4K 24p P7 FEL film (MMR mapping) with the demo mode on: 30 s and 20 s, 0 frames shown as the base layer, 0 repeated (before the fix: 8 and 9 in 15 s).

### Install / update
Install `VLC-FEL-0.2.7-poc-armeabi-v7a.apk` over an older version (or new). Details, notes and uninstall: `README.txt` (Hungarian / English).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0017 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`), the app patch `vlc/android-patches/0001` against vlc-android `fc5fa1f`.
