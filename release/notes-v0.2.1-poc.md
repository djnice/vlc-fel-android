**Proof of concept** – experimental build for enthusiasts; it installs kernel modules on your player. Use at your own risk.

New in 0.2.1:

- **Dual track / dual PID Profile 7 FEL (DTDL)**: the enhancement layer in its own track or PID now plays with full FEL composition – dual track **MP4** (dvhe/dvh1 EL track) and **m2ts / ts**, including **Ultra HD Blu-ray** streams (EL on PID 0x1015). VLC's demux appends each EL access unit to the base layer picture with the same time, the way single track files carry it.
- **3D MVC from Blu-ray m2ts and .ssif**: the dependent view on its own PID (e.g. tsMuxeR m2ts) and the disc's `.ssif` files (base and dependent view interleaved; rename to `.m2ts` if the file browser does not list them), with seeks.
- **R24 firmware update (2026-08-27)**: the `mvc3d` module (2026.10.12.3) now finds the HDMI 3D flags in the firmware's code; with 0.2.0 3D MVC played in 2D on that update.
- 3D: the software decoder no longer hangs after some seeks.
- VLC MP4 fix: with an edit list skipping the composition delay, the first pictures of B-frame streams got wrong presentation times.

**Tested:** Dune HD Pro 8K Plus, firmware `250815_1012_r22` (LG OLED: two DTDL MP4s, the same as m2ts, an Ultra HD Blu-ray main movie cut at 80 Mbps – all frames composed, pause and seeks; Optoma 1080p 3D projector: m2ts / .ssif / .mk3d in frame packing, the module update 2026.10.12.2 → .3 with root access off); Dune HD Pro One 8K Plus, firmware `260303_1721_r24` and its 2026-08-27 update (3D m2ts / .ssif / .mk3d).

### Install / update
1. Install `VLC-FEL-0.2.1-poc-armeabi-v7a.apk` over 0.2.0 / 0.1.0 (or new).
2. Play an MVC file on a 3D display: VLC FEL updates the `mvc3d` kernel module to `2026.10.12.3` (with the root question if root access is off). The `dvfel` module is unchanged (`2026.10.07.1`).

Details, notes and uninstall: `README.txt` (Hungarian / English).

### Known limits
- Not from Blu-ray ISO images / folders (VLC's Blu-ray access gives the 2D base view / the base layer only); not from fragmented MP4.
- 3D: the GUI shows in the left eye's view only; after a seek into an open GOP (Blu-ray) up to ~1 s may be 2D; some projectors drop HDMI when switching from a 2160p desktop to frame packing (use a 1080p desktop).
- FEL limits as before (4K 50/60p FEL too heavy).

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause): kernel modules `dvfel` and `mvc3d`, libdvfelplay, libmvc3dplay, VLC patches 0001–0010 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`, vlc-android `fc5fa1f`).
