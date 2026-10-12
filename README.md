# VLC FEL for Android (Dune HD / R_volution, Amlogic S928X)

> **Proof of concept.** Experimental build for enthusiasts. It installs a kernel module on your player –
> use at your own risk.

Dolby Vision **Profile 7 FEL** (full enhancement layer) playback at full quality on the Android firmware of
Amlogic S928X players (Dune HD, R_volution), in a modified VLC for Android ("VLC FEL",
`org.videolan.vlc.fel`, installs next to the official VLC). The S5 Dolby Vision hardware has no enhancement
layer input, so the two layers are composed on the GPU:

```
VLC demux ─ mediacodec module ─ libdvfelplay: RPU → profile 8.1 (no-op mapping), EL NAL units taken out
  ├─ BL: MediaCodec (Dolby Vision decoder) → HWC → video_composer.0
  │        → dvfel (kernel): VICP A, AFBC → linear 10-bit 4:4:4 ─┐
  └─ EL: 2nd MediaCodec (HEVC) → AImageReader → dmabuf (10-bit AFBC)
           → DVFEL_IOC_EL_IMPORT: VICP → linear 4:4:4 ─────────────┤
                                     GPU (felgpu): Lanczos-3 EL upscale, polynomial/MMR prediction,
                                     NLQ, 12-bit composition, dither ←──┘ (+ original RPU)
  → GPU writes the dvfel slot's display buffer (linear 10-bit 4:4:4)
  → dvfel holds it to its vsync (lead time) → video_render.0 → amvideo → Dolby Vision core (profile 8.1)
```

Since v0.2.0 it also plays **3D MVC (Blu-ray 3D)** in 3D – both views at full resolution, HDMI 1.4 frame
packing (1080p24) – on 3D displays and projectors, see [3D MVC](#3d-mvc-blu-ray-3d).

With a **lead time**: VLC presents every frame 3 vsyncs (125 ms at 24 Hz) early and the kernel module holds
the composed frame back to its vsync. The GPU has ~2.5 frame periods instead of ~1, so its time spikes during
playback (up to ~110 ms measured) no longer repeat or drop frames, and the picture is in sync with the audio
(before: 84 ms late). Measured, 4K 24p P7 FEL, 2 minutes each: 0 repeated or dropped frames on a Dune HD
Pro 8K Plus (R22, MMR chroma; without the lead time: ~25 a minute) and on a Dune HD Pro One 8K Plus (R24).

## 3D MVC (Blu-ray 3D)

H.264 MVC files (e.g. MakeMKV `.mk3d`, the dependent view in the same track) are shown in 3D when the display
reports 3D support (HDMI EDID), with both views at full resolution as HDMI 1.4 **frame packing**
(1920x2205, 1080p24):

```
VLC demux ─ mediacodec module ─┬─ MediaCodec H.264 (base view, the timing) → HWC → video_composer.0
                               │     → mvc3d (kernel): replaces every frame by its frame packed picture
                               │     → video_render.0 → amvideo
                               └─ libmvc3dplay (every access unit) ─ socket ─ mvc3dhelper (static aarch64)
                                     edge264-mvc: both views in software → L, 45 lines, R (8-bit 4:2:2)
HDMI: the firmware's auto frame rate sets 1080p24 (or 2160p24 on a 2160p desktop); mvc3d's probes in the
stock hdmitx21 make it 1080p24 frame packing (timing, 3D vendor infoframe).
```

- Pictures in sync with VLC's clock (1 vsync lead time), seeks and pause as usual. Measured on the
  projector (Optoma, 1080p24 3D): 1430 of 1432 frames shown in a minute, A/V 0 vsyncs; after a seek
  1–2 frames until the software decoder has caught up.
- Works from a 1080p and from a 2160p desktop (on 2160p the GUI is moved to where it is on 1080p).
- 2D displays: the base view in 2D (as before); VLC option `--mediacodec-mvc=1` forces 2D, `=2` 3D
  (Settings → Advanced → Custom libVLC options).
- Needs the `mvc3d` kernel module, installed by VLC FEL like `dvfel` (one root question).

## Download

Releases: <https://github.com/djnice/vlc-fel-android/releases> (APK, read `README.txt`).

## Requirements

- Amlogic S928X Dune HD or R_volution player with the Dune HD **R22 or R24** Android firmware.
  Tested: Dune HD Pro 8K Plus, firmware `250815_1012_r22`; Dune HD Pro One 8K Plus, firmware
  `260303_1721_r24` (one module build for both, no symbol version differences): playback, pause / seek,
  the module installation (R22 with root access off, R24 with it on) and the module loading at boot.
- Profile 7 FEL files with the enhancement layer in the base layer's track (**single track, STDL**, e.g. MKV
  remuxes). Dual track (DTDL) files are not supported yet.
- 3D MVC: files with the dependent view in the base view's track (MKV / `.mk3d`); a 3D display (frame
  packing 1080p24). Tested with an Optoma 1080p 3D projector on both players.
- Root access for the one-time module installation (the firmware's `sud`; VLC FEL asks to switch it on for
  the installation and switches it off again).

## How the module gets installed

libdvfelplay carries the `dvfel` kernel module. When VLC FEL plays the first profile 7 FEL file and the
module is not loaded (or older):

1. `su` – if the firmware's root access is off (`persist.vendor.root_access=0`), a dialog asks to switch it
   on for the installation ("Install" / "No"); the previous value is restored afterwards. A "No" is
   remembered (the file then plays without the enhancement layer, as profile 8.1).
2. The module is loaded. The firmware kernel only **warns** about symbol CRC mismatches and loads it
   anyway; such warnings go to the log (`adb logcat -s dvfelplay`, `tools/fwcheck.sh`).
3. The module goes to `<FS_PREFIX>/config/dvfel/dvfel.ko` and a boot script to
   `<FS_PREFIX>/config/boot/dvfel.sh`, which the firmware runs as root at every boot (R24: `binit.sh`,
   R22: the `dunehd_init` service script `/system/xbin/dunehd/init`). A newer module is never replaced
   by an older one.

libmvc3dplay installs the `mvc3d` module the same way (`config/mvc3d/mvc3d.ko`, `config/boot/mvc3d.sh`) when
the first MVC file is to play in 3D.

## Repository

| Path | Content |
|---|---|
| `kernel/` | `dvfel` kernel module (GPL-2.0+): vfm node between video_composer and video_render, VICP A, EL import, GPU job interface with direct display buffers, lead time (`dvfel_uapi.h`). Builds for the Dune 5.4 GKI kernel. |
| `libdvfelplay/` | The player side as a C library (GPL-2.0+): access unit filter (P7 → P8.1, EL extraction), EL decoder, GPU composer (`felgpu`), BL/EL pairing, vsync-aligned render times, lead time, the bundled module and its installer. `include/dvfelplay.h` is the API. Builds with NDK r28c. |
| `kernel/mvc3d/` | `mvc3d` kernel module (GPL-2.0+): vfm node between video_composer and video_render that replaces the frames by user space's frame packed pictures (`mvc3d_uapi.h`), lead time, and the HDMI frame packing probes for the stock hdmitx21 (R22 and R24). |
| `libmvc3dplay/` | 3D MVC, player side (GPL-2.0+): the library in the player, `mvc3dhelper` (static aarch64: edge264-mvc decoding, frame packing), the bundled module and its installer, `edge264/` (edge264-mvc at `5757e71` + the picture tag patch). `include/mvc3dplay.h` is the API. |
| `vlc/patches/` | Patches for VLC 3.0.x (`84177b2273`, as libvlcjni-3.x `0b8dc65` takes it): 0001 Dolby Vision decoder + FEL composition, 0002 vsync-aligned render times, 0003 lead time, 0004 pictures released ahead of their time, 0005 module installation, 0006 3D MVC, 0007 3D MVC lead time, 0008 3D MVC module installation. |
| `vlc/build-in-docker.sh` | The APK build (vlc-android `fc5fa1f`, libvlcjni-3.x, armeabi-v7a) in VideoLAN's docker image; package `org.videolan.vlc.fel`, name "VLC FEL". |
| `tools/` | `kbuild.sh` (module build with the firmware's CRCs), `modver.py` (CRC database/symvers from firmware modules), `fwcheck.sh` (does a build fit a player? trial load over adb), `sync.sh` (maintainer: copies the working sources here), `tests/` (cadence and pause/seek measurements over adb). |
| `docs/NOTES.md` | Technical notes and pitfalls of this firmware, the lead time, VLC. |

## Building (outline)

1. Module: `tools/kbuild.sh kernel/` – clang 11.0.1 (`LLVM=1`, as the firmware kernel), a kernel tree
   configured like the firmware, the Amlogic media headers matching the firmware's structures, and the
   firmware's symbol CRCs (`modver.py db/symvers` from its vendor modules and `aml_media.ko`).
2. Library: `DVFEL_KO=kernel/dvfel.ko libdvfelplay/build.sh` – NDK r28c, libdovi (`LIBDOVI_DIR`, built
   for arm-linux-androideabi with cargo-c), API 26, armeabi-v7a (the players' userspace is 32-bit).
3. 3D MVC: `tools/kbuild.sh kernel/mvc3d`, `libmvc3dplay/edge264/build.sh`, `libmvc3dplay/build.sh`
   (NDK r28c for the library, a static capable aarch64 gcc for the helper).
4. VLC: `docker run -v vlcfel:/work -v $PWD:/src:ro -e PATCHES=/src/vlc/patches
   -e DVFELPLAY_SO=/src/libdvfelplay.so -e MVC3D_DIR=/src registry.videolan.org/vlc-debian-android-3.0:20260701183656
   bash /src/vlc/build-in-docker.sh` (copy `libdvfelplay.so`, `libmvc3dplay.so` and `libmvc3dhelper.so` to
   the repository root first; VLC's contribs are built from source the first time, which takes long), then
   sign the APK with your own key.

## Use / switches

- Plays like VLC; profile 7 FEL files (single track, STDL – see the limits below) are composed, MEL and profile 7 without EL play as profile 8.1,
  other Dolby Vision profiles through the Dolby Vision decoder. VLC option `--no-mediacodec-dovi-fel`
  (Settings → Advanced → Custom libVLC options) turns FEL composition off.
- Test switches (`adb shell setprop`, read by libdvfelplay): `debug.dvfp.lead_ms` (lead time, 0: off),
  `debug.dvfp.profile` (2: GPU frame times and the release lead to the log every 5 s), `debug.dvfp.dither`
  (0 round, 1 dither, 2 half).
- Kernel statistics: `adb shell cat /sys/kernel/debug/dvfel/stats` (root): display cadence, `vsync:`,
  `av: shown minus due vsync`; 3D MVC: `/sys/kernel/debug/mvc3d/stats` (jobs, frame packing state),
  `adb logcat -s mvc3dplay` (helper statistics every 10 s).

## Status / known limits

- **Single track (STDL) only**: the enhancement layer must be in the same video track as the base layer (as in
  MKV remuxes, e.g. MakeMKV). Dual track files (DTDL: the EL in a separate track or PID, e.g. Blu-ray m2ts /
  ISO, dual track MP4) play the base layer only, without Dolby Vision.
- Proof of concept: two firmware versions / models tested. 4K 50/60p FEL is too heavy (GPU time).
- 32-bit (armeabi-v7a) only, like the players' userspace.
- The Dune auto frame rate does not switch back after stop until you return to the Dune home screen.
- After a seek, 1–2 frames may be shown with the RPU mapping only (no EL residual), and the first frames
  after a seek may be a vsync late while the chain fills.
- Subtitles and the OSD are VLC's as usual; Dolby Vision itself is shown by the player's DV core.
- 3D MVC: the GUI (VLC's controls, subtitles) shows in the left view only; 3D Blu-ray folders / ISO / m2ts
  (the dependent view in a separate PID) play in 2D; frame packing only on the two tested firmware builds
  (elsewhere: 2D); 1080p and 720p MVC only.

## Changes

- **v0.2.0-poc** (kernel modules `dvfel 2026.10.07.1` (unchanged), `mvc3d 2026.10.12.2`, libmvc3dplay
  `2026.10.12`): 3D MVC (Blu-ray 3D) in 3D with HDMI 1.4 frame packing, both views at full resolution
  (software decoding of both views, the stock hdmitx21 made to output frame packing), from 1080p and 2160p
  desktops; bundled `mvc3d` module installer.
- **v0.1.0-poc** (kernel module `2026.10.07.1`, libdvfelplay `2026.10.07.1`): first release – VLC for
  Android 3.7.2 Beta 3 with FEL composition, lead time, vsync-aligned release, bundled module installer.

## License

GPL-2.0-or-later (VLC, libdvfelplay, libmvc3dplay and the kernel modules); edge264-mvc is BSD-2-Clause.
Dolby Vision is a trademark of Dolby Laboratories; this project is not affiliated with Dolby, Dune HD,
R_volution or VideoLAN.
