# Technical notes (Dune HD R22/R24 Android firmware, Amlogic S928X)

Findings from bringing up FEL composition on the Dune HD Pro One 8K Plus (firmware `260303_1721_r24`,
kernel `5.4.210` GKI, Android 11 userdebug, SELinux permissive).

## Video path

- MediaCodec (OMX, V4L2 decoder) → HWC → `video_composer.0` → `video_render.0` → amvideo. The vfm map
  `vcom-map-0` is created by video_composer at playback start. dvfel takes over the unused PIP map slot
  `dvelpath2` (`video_composer.0 dvfel video_render.0`), which precedes `vcom-map-0` (vfm uses the first
  valid map naming the provider).
- video_composer hands frames out at display time and allows at most 2 outstanding: dvfel releases the
  original right after VICP A ("early release", copying the Dolby Vision metadata into its slot).
  The release fence only signals for `vf->rendered` frames – the copy's flag must be carried over.
- **Repeated buffers**: when HWC sends the same buffer again (pause, seek, chapter jump), video_composer
  only increments `repeat_count` of the frame its receiver still holds and releases the repeats with it.
  A frame given back too early leaks the repeats: the decoder waits for their release fences
  (`OMX_AmlogicPeerBufferWrapper: wait fence 1001ms` ×3 → codec error, playback frozen) and codec_mm
  leaks until reboot. dvfel keeps the newest upstream frame until the next one arrives (`up_held`).
  Leak check: `/sys/class/video_composer/total_get_count - total_put_count`.
- **Pacing**: video_composer releases frames relative to the *last vsync* (it expects to be asked from the
  display's vsync interrupt). Polling it in between takes frames early and two within one vsync
  (`vpp_drop_cnt` in `/sys/class/video_composer/drop_cnt`). dvfel takes at most one per vsync
  (`get_vsync_count()`, `vc_pace`), two only with a backlog.
- Frame identification: on Android `pts_us64` is the time the frame reached video_composer from HWC (µs since
  the composer was opened), not the packet pts. dvfel reports the
  `rpu_data_crc32` of the frame's RPU (`DVFEL_IOC_WAIT_JOB2`), the compositor pairs it with the packet. The decoder
  hands the RPU to the kernel already unescaped and dvfel removes `00 00 03` once more: the compositor computes the
  CRC the same way, or CRCs starting with `03` after two zero bytes never match.
- **Direct display** (module 2026.10.06.2, `DVFEL_IOC_REG_BUFS2`): the VD1 reads linear 10-bit 4:4:4
  (`VIDTYPE_VIU_444`, `bit_mode 2`) – the compositor's frames are shown as they are, one compositor
  buffer per slot, no VICP B. Verified with the VD1 probe (`/sys/class/video/matrix_*`): luma identical
  to the AFBC path, chroma ±1–3 (4:4:4 instead of 4:2:0).

## VICP (5.4 GKI driver)

- The AFBC encoder is never switched off: every VICP operation needs a valid AFBC sink, or the next
  operation writes to stale addresses (ISR timeouts until reboot) – `a_fbc_sink`.
- **The first VICP operation after boot latches the HDR state**: if its input vframe carries a PQ
  `signal_type`, every later output gets a luma curve until reboot (chroma intact). The private input copy
  has `signal_type` cleared (`vin_sdr`).
- `vicp_process_enable(1)` (RDMA) caused silent resets; reading VICP registers outside an operation hangs
  the bus (watchdog reboot).
- Times: A (4K AFBC → linear) ~13 ms, B (linear → AFBC, plain mode) 13–22 ms (no longer used with
  direct display), EL import (1080p) ~3.5–4 ms.

## Hardware EL

- The HEVC decoder's CPU outputs are 8-bit only (V4L2 capture NV12/NV21/YUV420; no P010).
- Rendered into an `AImageReader` (PRIVATE, GPU_SAMPLED_IMAGE, max 2 images – the decoder allows 12
  output buffers in total), every image's dmabuf is a uvm buffer carrying the decoder vframe: 10-bit AFBC
  (`type 0x509000`, `bitdepth 0x2a00`). `dmabuf_get_vframe()` + VICP give a bit-exact linear picture.
- **Each EL access unit must end with the RPU NAL unit** (type 62), as in a demuxed EL stream: without a
  NAL unit after the last slice, the HEVC decoder's back end times out on some pictures (200 ms,
  `timeout_process_back`), and the pictures referencing them are lost until the next IRAP.

## GPU

- Mali-G57 under `simple_ondemand` stays at 666 MHz for the composer (37 ms per 4K frame); dvfel raises the
  devfreq `min_freq` to the maximum (830 MHz, 31 ms) while a compositor is registered (`gpu_boost`).
- A high priority EGL context made it worse (the player's own GPU work waits).

## Lead time (module 2026.10.07.1)

- On CoreELEC dvfel sits right after the decoder, which runs frames ahead: a slow GPU frame only eats into
  the buffer. On Android video_composer hands each frame out at its display vsync, so VICP A (~13 ms) and the
  GPU (~35 ms) had about two vsyncs: the frame showed ~2 vsyncs (84 ms at 24 Hz) late against the audio,
  and every GPU frame over ~35 ms of slack repeated a frame and dropped the next. Under playback load the
  GPU time has spikes from DDR contention (4K decoder + VICP + display): p95 45 ms, max 70–110 ms.
- Lead time: the compositor declares one (`DVFEL_IOC_SET_LEAD`, `out_lead_ms`, 125 ms = 3 vsyncs at 24 Hz),
  the player presents every frame that many whole vsyncs early, dvfel holds each processed frame back to
  its due vsync: the vsync the display would have shown it in without dvfel (video_composer releases a
  frame at a vsync, or mid-period when it came in more than ~10 ms after one) plus the lead. A frame taken
  late (no free slot) keeps its due vsync through the chain of arrival times (`pts_us64` intervals are
  whole vsyncs).
- The vsync timeline comes from the display's vsync interrupt itself: amvideo peeks `video_render.0` (us)
  every vsync, `is_in_vsync_isr()` tells those calls apart. A held frame becomes visible in the **middle**
  of the period before its due vsync. Releasing it at a vsync edge (a thread waking at "vsync + N") raced
  the interrupt: 56 repeats and 57 drops a minute. Shifting the outgoing `pts_us64` changes nothing.
- debugfs `stats`: `vsync:` (period, lead, held / late / chained frames) and `av: shown minus due vsync`
  (0: on time; without a lead this is dvfel's delay against the direct path, 2 vsyncs).
- Measured (24 Hz, 4K P7 FEL with MMR chroma, 120 s): without lead 27 repeats + 26 double takes, video
  84 ms late; with lead 0 + 0, all frames in their vsync, A/V as without dvfel.

## VLC

- VLC 3 has no Dolby Vision support on Android: patch 0001 probes the first access unit's RPU, restarts
  the decoder as `video/dolby-vision` and passes every access unit through libdvfelplay.
- The vout prepares a picture only a few ms before its date; `releaseOutputBuffer(index, time)` with a time
  in the past is shown as soon as possible, so the vsync it lands in depends on the phase of VLC's clock to
  the display (random per start or seek; near SurfaceFlinger's latch: repeats and drops). Dolby Vision
  pictures are dated 80 ms earlier and released for their real time snapped to the middle between two
  vsyncs (patches 0002, 0004), plus the lead time (0003). Subtitles stay in sync: the vout picks them by the
  picture's date.
- The Dune auto frame rate switches the HDMI mode behind Android's back; the vsync period is measured from
  AChoreographer callbacks (median, follows the switch to 24 Hz within 16 vsyncs).

## Root and module loading

- `su` (Koushik Superuser client) talks to the `sud` daemon, which runs while `persist.vendor.root_access`
  is empty (default) or `1`. On this permissive firmware an app may set that property itself.
- Module symbol CRC mismatches only produce a warning (`disagrees about version of symbol`) – the module
  loads anyway, so one build serves all firmware versions. A mismatch means a type in that symbol's
  signature changed (e.g. `struct vframe_s`): the first suspect if a firmware misbehaves. The installer
  logs them; `tools/fwcheck.sh` lists them for a player.
- `<FS_PREFIX>` comes from `FSP=` in `/system/xbin/dunehd/init` (`/data/data/com.dunehd.app` on Dune);
  `/system/dunehd/firmware/scripts/binit.sh` (R24) or `/system/xbin/dunehd/init` itself (R22, the
  `dunehd_init` service: root, `u:r:shell:s0`) runs the executables in `<FS_PREFIX>/config/boot/` at boot.
  R22 (`250815_1012_r22`, kernel 5.4.210 of 2025-08-15) takes the same module without any symbol version
  difference.
- Files pushed with adb to `/sdcard/Download` are invisible to apps until the media scanner sees them
  (Android 11 scoped storage); for tests put them into the app's own `Android/data/<package>/files/`.
- **USB / internal drives: folders but no files** (users' report, 2026-10-19). The Dune's MediaProvider does
  not index its drives (`content://media/<volume>/file` is empty), and on Android 11 an app without "all files
  access" (`MANAGE_EXTERNAL_STORAGE`) sees only indexed files through the FUSE mount: the storage permission
  shows the folders, none of the files. The firmware has no "all files access" settings page
  (`ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION`: no activity), so app patch `android-patches/0002`
  (`DuneRoot`) grants it with root: `su -c "appops set org.videolan.vlc.fel MANAGE_EXTERNAL_STORAGE allow"`;
  when su is off on a Dune (detected by `dune.afrc.afr`) `persist.vendor.root_access` is set to 1 for the
  moment and back to its value after (kept in the preferences meanwhile and put back at the next start if the
  app died in between – a crash in that window left it on in a test). It runs at the app's start before the
  media library (silently; after a failure not again for a day) and when the storage permission is asked
  (always). Measured on a Pro One 8K Plus (R24) in a user's state (storage permission, MANAGE default, root access 0):
  root on for 0.64 s, back to `0`, MANAGE allow; the browser lists the drive's videos with thumbnails and the
  folders' file counts.

## 3D MVC (mvc3d, libmvc3dplay)

- **32-bit userspace, arm64 kernel**: there is no `linker64`, so an aarch64 decoder cannot load into VLC. A
  *static* aarch64 executable runs fine though (also as the app's uid, from its native library directory:
  `libmvc3dhelper.so`). The software decoder (edge264-mvc, both views, ~1.8 cores for a 1080p24 3D Blu-ray)
  lives there; libmvc3dplay in VLC feeds it over a socket. The S5 hardware MVC decoder breaks the dependent
  view, so the base view comes from the hardware H.264 decoder (MediaCodec) and only carries the timing.
- **Frame identification**: the H.264 frame reaching the video composer is a deinterlacer copy
  (`VIDTYPE_DI_PW`); its `vf_ext` is the decoder vframe, whose `timestamp` is `N * 10^9` with N the number
  of the input buffer (codec config buffers not counted) since the decoder started – **or since the last
  flush**: the Amlogic codec2 decoder numbers from 0 again after every flush (seek). The pts fields are 0.
- After a seek VLC prerolls from the key frame before the position: those pictures are decoded but never
  shown; the helper must give their buffers back early or it cannot reach the first frame shown.
- **Lead time**: 1 vsync is enough (the picture is ready a moment after the frame); 125 ms makes
  SurfaceFlinger drop every 7th frame (the H.264 decoder cannot run that far ahead with its buffers).
- **HDMI 1.4 frame packing on the stock hdmitx21** (no firmware change): `config` takes `3dfp` (flag,
  vendor infoframe, 3D clock table) but the mode keeps the 2D timing. CoreELEC's linux-amlogic adds the frame
  packed timing in `hdmitx21_get_fmtpara()` (95e29f15de); the encoder takes its timing from the VIC table
  (`set_tv_encp_new()` → `hdmitx21_gettiming_from_vic()`). mvc3d adds both with return probes, for the mode
  sets of the firmware's auto frame rate only (callers `hdmitx_validate_vmode` / `hdmitx_set_current_vmode`;
  the HDR/vendor packet refreshes keep what the output has). The 3D flag byte of `struct hdmitx_dev`:
  R24 `+0x2f08`, R22 `+0x2ee8` (from `config_store`'s `3dfp` branch); the module picks it by the kernel build
  in `/proc/version`.
- Setting a mode through `/sys/class/display/mode` (allowed only with `display/debug` 1) makes the
  composer (MesonHwc) handle a hotplug: SurfaceFlinger recreates the display and VLC's playback stalls.
  The firmware's auto frame rate must set the mode; mvc3d only changes what the hdmitx makes of it.
- From a **2160p desktop** the auto frame rate picks 2160p24 (no frame packing exists for it): the hdmitx
  gets `1080p24hz` instead (Android/DRM keep 2160p24, the frame buffer is 1080p anyway), and the GUI planes
  (placed in 2160p coordinates by the composer) get their destination halved in `meson_plane_atomic_check`.
  The firmware also has its own `auto_resolution` AFR setting (`dune.afrc.ar`), not needed.
- Kernel CRCs of symbols no firmware module imports (`register_kretprobe`, `register_kprobe`,
  `sprint_symbol`) come from genksyms over the kernel tree (checked: `printk` gives the firmware's CRC).
- **Never read core kernel structures from the module** (`utsname()`: task/nsproxy layouts differ between
  the R22 and R24 kernels – it panicked R22); `/proc/version` through `kernel_read()` is safe.
