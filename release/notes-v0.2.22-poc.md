**Proof of concept**: an experimental build for enthusiasts. It installs kernel modules on your player; use it at your own risk.

New in 0.2.22:

- **S928X: FEL composition about 22% faster, the same picture bit for bit.** The GPU composer now imports the frame buffers as `RGB10_A2` (their pixel word is `ABGR2101010`), so the separate EL unpack passes are gone. On a Dune HD Pro 8K Plus with a 4K profile 7 FEL film the GPU time per frame went from ~35 to ~27 ms on average (p95 ~38 → ~29 ms), and the frames over one 24p frame time (41.7 ms) from 18 to 1 per 90 s. Every mode (normal, dither, demo mode, test pattern, software EL planes) composes the same picture as before, checked frame by frame on reference frames and on frames dumped during playback. If the GPU driver refuses the 10-bit import, the library goes on with the former RGBA8 path by itself; `setprop debug.dvfp.rgb10 0` forces that path for a comparison.
- **The libVLC of CineFuze 1.3.8** (`3.0.24-93-g0cb8157d28`, VLC patches 0035–0058):
  - **a seek shows its target's picture** (0042–0054). VLC seeks precisely unless *Always fast seek* is on.
  - **no picture lost at subtitle changes** in 4K SDR / HDR10 films: every picture is now released ahead of its time like the Dolby Vision ones, for a time snapped between the vsyncs (0055). Custom libVLC option `--no-mediacodec-release-ahead` turns it off.
  - **interlaced streams without drops**: a 1080i stream comes out of the decoder at twice its frame rate, and the release-ahead time now follows that rate (0056).
  - **no lost pictures after a chapter jump** on the Dolby Vision decoder: it gave pictures from before the jump with their old times (0057).
  - one SMB connection and login less at each start: TagLib no longer opens the film a second time (0058).
  - DSD / DST audio (DSF / DSDIFF) plays, as 88.2 / 96 kHz PCM (0036, 0037); AV1 with the hardware decoder (0039).
  - **HDR10+ films as Dolby Vision profile 8.1**, for Dolby Vision displays without HDR10+: custom libVLC option `--mediacodec-dovi-hdr10plus` (off by default). The RPU is made from each frame's own HDR10+ metadata (libdvfelplay's `DVFP_MODE_HDR10PLUS`, 0038). `--mediacodec-dovi-no-hdr10plus` sends a hybrid Dolby Vision + HDR10+ stream without its HDR10+ part, for displays that pick HDR10+ first (0040).

libdvfelplay `2026.11.17.1` is new (it carries the HDR10+ conversion of `2026.11.10.1` as well). The kernel modules (S928X `dvfel 2026.10.29.2`, S905X4 `dvfel 2026.10.31.1`), libmvc3dplay, `mvc3d` and the app patches are unchanged. VLC patches 0035 and 0041 (the `native` audio output device) and the libvlcjni patch 0002 (`nextFrame()`) are in the build for CineFuze but not used by this app.

**On Dune HD r25** the Dune HD player composes FEL with the firmware plugin's copy of libdvfelplay: install the **Dune HD plugin `2026.11.17.1-0.2.22`** (release `dune-plugin-2026.11.17.1`) for the faster composer there too.

**Tested** on a Dune HD Pro 8K Plus (S928X, firmware `261101_0347_r25`, its own `dvfel 2026.10.29.2`) with this APK:
- a 4K profile 7 FEL sample (single track MKV, 114 s): composed as `RGB10_A2`, GPU job 28 ms on average, no fallback and no timeout in 1373 frames, 0 dropped pictures after the start (2 at the start); 6 chapter jumps: the picture back within a second each time, nothing dropped;
- a 4K HDR10+ sample with chapters: 6 chapter jumps with the HEVC decoder and 12 with `--mediacodec-dovi-hdr10plus` (the Dolby Vision decoder): the picture came back within a second after every jump, and the Amlogic media codec service never died (the patch left out of this build, 0059, killed it at 3 of 4 runs in CineFuze).

The subtitle change, interlaced, chapter drop and SMB start fixes (0055–0058) were measured in CineFuze 1.3.8, which has the same libVLC; not again with this app. Not tested: this build on the S905X4, a whole film.

### Install / update
Install `VLC-FEL-0.2.22-poc-armeabi-v7a.apk` over an older version, or as a new install. `README.txt` (Hungarian / English) has the details, notes and uninstall steps.

Source: this repository (GPL-2.0-or-later; edge264-mvc BSD-2-Clause):
- kernel modules `dvfel` (S928X: `kernel/`, S905X4: `kernel/sc2/`) and `mvc3d`;
- libdvfelplay and libmvc3dplay;
- VLC patches 0001–0058 against VLC 3.0.x (`84177b2273`, libvlcjni-3.x `0b8dc65`);
- the libvlcjni patches `vlc/jni-patches/0001` and `0002`;
- the app patches `vlc/android-patches/0001`–`0004` against vlc-android `fc5fa1f`.
