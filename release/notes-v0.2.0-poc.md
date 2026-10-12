**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.0: **3D MVC (Blu-ray 3D) in 3D** – both views at full resolution, **HDMI 1.4 frame packing** (1080p24), on 3D TVs and projectors. Dolby Vision Profile 7 FEL playback as in 0.1.0 (unchanged `dvfel` module).

How it works: VLC keeps decoding the base view in hardware (it carries the timing, A/V sync, pause and seek); a helper process decodes both views in software (edge264-mvc) and composes a frame packed picture (1920x2205) for every frame; the new `mvc3d` kernel module shows it in place of the base view and makes the firmware's own HDMI driver output frame packing (no firmware change). The firmware's auto frame rate switches as usual, from a 1080p or a 2160p desktop.

Measured on an Optoma 1080p 3D projector: 1080p 24 Hz 3D, 1430 of 1432 frames shown in a minute, picture in sync with the audio; seeks and pause work (1–2 frames skipped after a seek).

**Tested:** Dune HD Pro 8K Plus, firmware `250815_1012_r22`; Dune HD Pro One 8K Plus, firmware `260303_1721_r24` – including the module installation (R22 with root access off: the question dialog, R24 with it on) and loading at boot. On other S928X Dune HD / R_volution firmware the `mvc3d` module plays 3D MVC in 2D (it only knows the HDMI driver of these two builds); please report (model, firmware, `adb logcat -s mvc3dplay`).

### Install / update
1. Install `VLC-FEL-0.2.0-poc-armeabi-v7a.apk` over 0.1.0 (or new).
2. Play an MVC file (`.mk3d`, MKV with the dependent view in the same track) on a 3D display: the first time VLC FEL installs the `mvc3d` kernel module (`2026.10.12.2`) in the firmware's boot hook, asking to switch root access on if it is off.
3. 2D displays play the base view in 2D. Force with `--mediacodec-mvc=1` (2D) / `=2` (3D) in Settings → Advanced → Custom libVLC options.

Details, notes and uninstall: `README.txt` (Hungarian / English).

### Known limits
- In 3D the GUI (VLC's controls, subtitles) shows in the left eye's view only.
- 3D Blu-ray folders / ISO / m2ts (the dependent view in a separate PID) play in 2D.
- 1080p and 720p MVC; the software decoder needs ~2 CPU cores for a 3D Blu-ray.
- FEL limits as in 0.1.0 (4K 50/60p FEL too heavy, single track FEL only).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0008 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`, vlc-android `fc5fa1f`).
