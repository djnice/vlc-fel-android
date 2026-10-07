**Proof of concept** – experimental build for enthusiasts; it installs a kernel module on your player. Use at your own risk.

Dolby Vision Profile 7 **FEL** playback at full quality in VLC for Android (3.7.2 Beta 3) on the Android firmware of Amlogic S928X players: base and enhancement layer decoded in hardware, composed on the GPU (12-bit, dither), Dolby Vision output, with a **lead time**: VLC presents every frame 3 vsyncs (125 ms at 24 Hz) early and the kernel module holds the composed frame back to its vsync, so the GPU has ~2.5 frame periods instead of ~1.

Measured (4K 24p P7 FEL, 2 minutes each): **0 repeated or dropped frames** on a Dune HD Pro 8K Plus (R22, MMR chroma; without the lead time ~25 a minute) and on a Dune HD Pro One 8K Plus (R24), picture in sync with the audio (before: 84 ms late).

**Tested:** Dune HD Pro 8K Plus, firmware `250815_1012_r22`; Dune HD Pro One 8K Plus, firmware `260303_1721_r24` (one module build for both). On other S928X Dune HD / R_volution players please report how it works (model, firmware and `adb logcat -s dvfelplay`).

### Install
1. Install `VLC-FEL-0.1.0-poc-armeabi-v7a.apk` ("VLC FEL", `org.videolan.vlc.fel`, next to the official VLC).
2. Play a FEL file: the first time, VLC FEL installs the `dvfel` kernel module (`2026.10.07.1`) in the firmware's boot hook. If root access is off on the player, it asks to switch it on for the installation (and switches it off again).
3. Play FEL files – from SMB/NFS shares or local storage.

Details, notes and uninstall: `README.txt` (Hungarian / English).

### Known limits
- 4K 24p FEL in real time; 4K 50/60p FEL is too heavy.
- The Dune auto frame rate switches back to the GUI mode only when returning to the Dune home screen.
- After a seek, 1–2 frames may show with the RPU mapping only (no EL), and the first frames may be a vsync late.

Source: this repository (GPL-2.0-or-later): kernel module, libdvfelplay, VLC patches 0001–0005 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`, vlc-android `fc5fa1f`).
