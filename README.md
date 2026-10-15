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
packing (1080p24) – on 3D displays and projectors, see [3D MVC](#3d-mvc-blu-ray-3d). Since v0.2.1 also from
Blu-ray **m2ts / .ssif** files, and profile 7 FEL from **dual track / dual PID** files (MP4, m2ts, Ultra HD
Blu-ray streams), see [Dual layer files](#dual-layer-files-dtdl). Since v0.2.2 a split screen **demo mode**
compares the FEL composition with the base layer, with measured brightness, see
[Demo mode](#demo-mode-fel-vs-base-layer).

With a **lead time**: VLC presents every frame 3 vsyncs (125 ms at 24 Hz) early and the kernel module holds
the composed frame back to its vsync. The GPU has ~2.5 frame periods instead of ~1, so its time spikes during
playback (up to ~110 ms measured) no longer repeat or drop frames, and the picture is in sync with the audio
(before: 84 ms late). Measured, 4K 24p P7 FEL, 2 minutes each: 0 repeated or dropped frames on a Dune HD
Pro 8K Plus (R22, MMR chroma; without the lead time: ~25 a minute) and on a Dune HD Pro One 8K Plus (R24).

## Demo mode (FEL vs. base layer)

A split screen comparison for profile 7 FEL: the left half with the full FEL composition, the right half as
the base layer converted to profile 8.1 shows it (what a player without FEL support shows). The top of each
half shows the brightness measured on the active picture (letterbox bars left out: RPU level 5, or bars found
in the picture) as a level 1 – maximum and average of max(R', G', B'), in nits:

```
FEL     MAX … AVG … NIT        BL      MAX … AVG … NIT
RPU L1  MAX … AVG … NIT        FEL-BL  MAX +… AVG +… NIT
AREA    3840X1600 L5
```

- **On / off during playback:** player menu (⋯) → *Dolby Vision demo: on / off* – shown while a FEL
  composition runs, for the current playback; every file starts without it.
- **Always on:** VLC option `--mediacodec-dovi-demo` (Settings → Advanced → Custom libVLC options).
- `RPU L1` is the level 1 the file's RPU carries (some files carry none or zeros there).

## 3D MVC (Blu-ray 3D)

H.264 MVC files are shown in 3D when the display reports 3D support (HDMI EDID), with both views at full
resolution as HDMI 1.4 **frame packing** (1920x2205, 1080p24). Sources:

- MKV / `.mk3d` (e.g. MakeMKV): the dependent view in the same track.
- Blu-ray **m2ts** with the dependent view on its own PID (stream type 0x20, e.g. tsMuxeR), and the disc's
  **`.ssif`** files (`BDMV/STREAM/SSIF/*.ssif`: the base and the dependent view file interleaved; rename to
  `.m2ts` if the file browser does not list them). VLC's ts demux merges the dependent view into the base
  view's access units (by DTS); for `.ssif` it drops the dependent view file's PMT and PCRs and, after a seek,
  reads back the dependent view extent the position needs.
- Not from Blu-ray ISO images / folders: VLC's Blu-ray access (libbluray) only gives the 2D base view.

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

## Dual layer files (DTDL)

Profile 7 with the enhancement layer (EL + RPU) in a track or PID of its own is turned into the single track
form by VLC's demux: each EL access unit is appended to the base layer access unit with the same DTS (every EL
NAL unit wrapped in a NAL unit of type 63, the RPU last), so it plays exactly like an STDL file.

- **MP4**: an EL track `dvhe`/`dvh1` at a quarter of the base layer's size next to the HEVC base layer track
  (the EL track gets no stream of its own; its parameter sets are added at IRAP pictures when not in-band).
- **m2ts / ts**: the Dolby Vision video stream descriptor without the base layer (its dependency PID), or
  Ultra HD Blu-ray's HEVC PID 0x1015 next to 0x1011 (also when the PMT spans several packets).
- Tested on an LG OLED (Dolby Vision): two DTDL MP4s, the same as m2ts, and a cut of an Ultra HD Blu-ray
  main movie m2ts (80 Mbps, FEL): all frames composed, pause and seeks.
- Also fixed on the way, in VLC's MP4 demux: with an edit list skipping the composition delay, the first
  pictures of B-frame streams got wrong (even duplicate) presentation times.

## Download

Releases: <https://github.com/djnice/vlc-fel-android/releases> (APK, read `README.txt`).

## Requirements

- Amlogic S928X Dune HD or R_volution player with the Dune HD **R22 or R24** Android firmware.
  Tested: Dune HD Pro 8K Plus, firmware `250815_1012_r22`; Dune HD Pro One 8K Plus, firmware
  `260303_1721_r24` and its 2026-08-27 R24 update (one module build for all; no symbol version differences): playback,
  pause / seek, the module installation (R22 with root access off, R24 with it on) and the module loading
  at boot.
- Profile 7 FEL files: single track (STDL, e.g. MKV remuxes) or dual track / dual PID (DTDL: MP4, m2ts).
- 3D MVC: MKV / `.mk3d`, m2ts with the dependent view PID, `.ssif`; a 3D display (frame packing 1080p24).
  Tested with an Optoma 1080p 3D projector on both players.
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
| `vlc/patches/` | Patches for VLC 3.0.x (`84177b2273`, as libvlcjni-3.x `0b8dc65` takes it): 0001 Dolby Vision decoder + FEL composition, 0002 vsync-aligned render times, 0003 lead time, 0004 pictures released ahead of their time, 0005 module installation, 0006 3D MVC, 0007 3D MVC lead time, 0008 3D MVC module installation, 0009 3D MVC from m2ts / `.ssif` (ts demux, h264 packetizer), 0010 Dolby Vision dual track / dual PID merging (mp4 and ts demux, MP4 presentation time fix), 0011 the demo mode option, 0012 subtitles drawn at the display's size, 0013 pictures released at their time before the subtitles (no dropped frames at subtitle changes), 0014 the live level 1 / level 5 of the RPUs for a player's information panel (a file in the app's files dir, only while asked for), 0015 Dolby Vision pictures released 140 ms ahead up to 30 fps, 0016 those levels in time with the picture on the screen, 0017 a disabled track plays when no track of its kind is enabled (Matroska FlagEnabled 0, MP4 tkhd). |
| `vlc/android-patches/` | Patches for the vlc-android app (`fc5fa1f`): 0001 the player menu's *Dolby Vision demo* switch. |
| `vlc/build-in-docker.sh` | The APK build (vlc-android `fc5fa1f`, libvlcjni-3.x, armeabi-v7a) in VideoLAN's docker image; package `org.videolan.vlc.fel`, name "VLC FEL". |
| `tools/` | `kbuild.sh` (module build with the firmware's CRCs), `modver.py` (CRC database/symvers from firmware modules), `fwcheck.sh` (does a build fit a player? trial load over adb), `sync.sh` (maintainer: copies the working sources here), `tests/` (cadence and pause/seek measurements, the stop / start / seek stress test `stopstart.sh`, over adb). |
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
   -e APATCHES=/src/vlc/android-patches
   -e DVFELPLAY_SO=/src/libdvfelplay.so -e MVC3D_DIR=/src registry.videolan.org/vlc-debian-android-3.0:20260701183656
   bash /src/vlc/build-in-docker.sh` (copy `libdvfelplay.so`, `libmvc3dplay.so` and `libmvc3dhelper.so` to
   the repository root first; VLC's contribs are built from source the first time, which takes long), then
   sign the APK with your own key.

## Use / switches

- Plays like VLC; profile 7 FEL files (single or dual track) are composed, MEL and profile 7 without EL play as profile 8.1,
  other Dolby Vision profiles through the Dolby Vision decoder. VLC option `--no-mediacodec-dovi-fel`
  (Settings → Advanced → Custom libVLC options) turns FEL composition off.
- Test switches (`adb shell setprop`, read by libdvfelplay): `debug.dvfp.lead_ms` (lead time, 0: off),
  `debug.dvfp.profile` (2: GPU frame times and the release lead to the log every 5 s), `debug.dvfp.dither`
  (0 round, 1 dither, 2 half), `debug.dvfp.demo` (1: demo mode, like the menu switch).
- Kernel statistics: `adb shell cat /sys/kernel/debug/dvfel/stats` (root): display cadence, `vsync:`,
  `av: shown minus due vsync`; 3D MVC: `/sys/kernel/debug/mvc3d/stats` (jobs, frame packing state),
  `adb logcat -s mvc3dplay` (helper statistics every 10 s).

## Status / known limits

- Dual layer: MP4 (non-fragmented) and m2ts / ts; not from Blu-ray ISO images / folders (libbluray), not from
  fragmented MP4 or MKV with the EL in a second track.
- Proof of concept: two models, three firmware builds tested. 4K 50/60p FEL is too heavy (GPU time).
- 32-bit (armeabi-v7a) only, like the players' userspace.
- The Dune auto frame rate does not switch back after stop until you return to the Dune home screen.
- After a seek, 1–2 frames may be shown with the RPU mapping only (no EL residual), and the first frames
  after a seek may be a vsync late while the chain fills.
- Subtitles and the OSD are VLC's as usual; Dolby Vision itself is shown by the player's DV core.
- 3D MVC: the GUI (VLC's controls, subtitles) shows in the left view only; Blu-ray ISO images / folders play
  in 2D; frame packing only on the tested firmware builds (elsewhere: 2D); 1080p and 720p MVC only. After a
  seek into an open GOP (Blu-ray discs), up to ~1 s may show the base view twice (2D) until the next key
  picture. Some projectors drop the HDMI connection when switching from a 2160p desktop to frame packing,
  which stops playback: use a 1080p desktop then.

## Changes

- **v0.2.6-poc** (kernel module `dvfel 2026.10.15.1`, libdvfelplay `2026.10.15.3`; VLC patches unchanged): no
  more **kernel panics** after quick stop / start / seek actions. When the display buffers were reallocated at a
  playback start, dvfel could reserve a slot twice and hand the video composer its own copy of a frame; it now
  keeps its reserved slots and gives back each upstream frame exactly once (anything else is refused and logged).
  The boot script also turns off the Amlogic H.265 decoder's back-end fast check
  (`decode_timeout_back_fast_check`), whose race with the back-end interrupt panicked the kernel
  (`vh265_back_irq_cb`); the regular 200 ms back-end timeout still applies. **No flicker** with single track
  files whose access units partly lack the EL and the RPU (muxed from a dual layer source with another EL
  picture order): such a picture gets the last RPU again, and the EL decoder waits for its next key picture
  instead of stalling. Test: `tools/tests/stopstart.sh`.
- **v0.2.5-poc** (libdvfelplay `2026.10.15.1`, VLC patches 0016, 0017; kernel modules unchanged): no more
  stutter for the rest of the film after a seek – once in a while the FEL's enhancement layer decoder got stuck
  at a seek (pictures in it, nothing out); every frame then fell back to the base layer, late, and the next seek
  did not help either. A stuck EL decoder is now emptied after 1.5 s and starts again at its next key picture.
  libVLC: a track marked disabled (Matroska FlagEnabled 0, MP4 tkhd) is selected when no track of its kind is
  enabled (VLC FEL's player picked such a track itself; other players on this libVLC got no picture / sound).
- **v0.2.4-poc** (VLC patches 0014, 0015; libdvfelplay and kernel modules unchanged): no stutter with
  subtitles on a **2160p output** either – up to 30 fps Dolby Vision pictures are handed to the display 140 ms
  ahead (was 80 ms), so the 20–60 ms a subtitle change takes to render no longer costs a frame. With a 1080p
  output 0.2.3 already did; with 2160p it still dropped a frame at many subtitle changes.
- **v0.2.3-poc** (VLC patches 0012, 0013; libdvfelplay and kernel modules unchanged): no more stutter with
  subtitles on – FEL dropped a frame at most subtitle changes (measured ~40 per minute on a 4K film, now
  under 1). The subtitles are drawn at the display's size instead of the video's (3840x2160 → 1920x1080 on a
  1080p desktop), and a picture is handed to the display at its time before the subtitle is drawn, also when
  rendering the subtitle text took longer than its margin.
- **v0.2.2-poc** (libdvfelplay `2026.10.13.1`, kernel modules unchanged: `dvfel 2026.10.07.1`,
  `mvc3d 2026.10.12.3`): FEL demo mode – split screen with the measured level 1 of both halves and the RPU's
  level 1, switchable from the player menu during playback (or always on with `--mediacodec-dovi-demo`).
- **v0.2.1-poc** (kernel modules `dvfel 2026.10.07.1` (unchanged), `mvc3d 2026.10.12.3`, libmvc3dplay
  `2026.10.12` with the helper fix): profile 7 FEL from dual track MP4 and dual PID m2ts (incl. Ultra HD
  Blu-ray streams); 3D MVC from m2ts and `.ssif`; `mvc3d` finds the HDMI 3D flags in the firmware's code (the
  R24 2026-08-27 update played 3D in 2D with 0.2.0); the 3D helper no longer hangs after some seeks.
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
