# 3D MVC a hardveres dekóderrel (ág: `hwmvc`) / 3D MVC with the hardware decoder

## Magyarul

Az S928X (S5) saját MVC-dekódere (`amvdec_h264mvc`) a függő (jobb) nézetet elrontja, amikor a `frame_num`
körbefordul (Blu-ray: `log2_max_frame_num` 10 vagy 8 → percenként-kétpercenként blokkos kép, a dekóder nem jelez
hibát). A CoreELEC javítása (github.com/CoreELEC/xbmc `acef2818e8`, Portisch) az elemi folyamot 16 bites
`frame_num`-ra írja át; a képek bitazonosak maradnak. Ennek C-portja a `libmvc3dplay/src/mvcfnr.c`.

Mérés a Dune HD Pro One 8K Plus-on (R24), képenkénti CRC-vel mindkét nézetre, az edge264 szoftveres dekóderhez
képest (`libmvc3dplay/tools/hwmvctest.c`, `e264crc.c`):

| minta | átírás nélkül | átírással |
|---|---|---|
| Gravity (átfordulás 17,7 s) | a jobb nézet 10 képe hibás | 713/713 kép bitazonos |
| Avatar (átfordulás 35,8 s) | 2 jobb + 23 bal nézetes kép hibás | 1384/1384 kép bitazonos |

Ezzel a szoftveres dekóder nélkül megy a 3D:

- **Dekódolás:** a libmvc3dplay (`mvc3d_hw_*`) az átírt folyamot a régi Amlogic MVC-dekóderbe írja
  (`/dev/amstream_vbuf`). Ez a dekóder kizárja a MediaCodecet, ezért a VLC ilyenkor nem használ MediaCodecet.
- **Kompozíció:** a mvc3d kernelmodul (2026.10.18, `/dev/mvc3dhw`, `kernel/mvc3d/mvc3d_hw.c`) a dekóder képeit
  (32x32-es blokkok, 64 bites szavanként fordított bájtsorrend) frame packed (vagy 2D) UYVY képpé rakja össze a
  lejátszó uvm-puffereibe, és vframe-et akaszt rájuk.
- **Kijelzés:** a lejátszó a puffert a kép idejében egy videotunnelbe sorolja; a videófelület Amlogic sideband
  streamje (0x400, csatorna = tunnel) miatt a HWC a video_composer.0 → mvc3d → video_render.0 úton mutatja.
- **VLC:** `vlc/patches/0034`: a mediacodec modul saját `mc_api`-ja (MediaCodec helyett, újraindítással váltva;
  ha nem indul, MediaCodec + a szoftveres út). Opció: `--mediacodec-mvc-hw` = 0 szoftveres, 1 hardveres
  (alapértelmezett, 3D kijelzőn), 2 hardveres 2D-ben is (teszt).
- **HDMI-ráta:** a Dune AFR a MediaCodec-dekódertől tudja a film fps-ét. A 0032 óta a lejátszás elején
  megnyitott MediaCodec megkapja a frame rate-et, így az AFR várhatóan magától vált; ha mégsem, a
  `vlc/android-patches/0003` (DuneDesktopMode) 8 s után, ha a mód rátája nem a filmé, SystemControl-lal vált
  (pl. 2160p60 → 2160p24, 23,976 Hz), a végén visszaáll. 3D-ben ebből lesz 1080p24 frame packing.

- **TV-s teszt 3D kijelző nélkül:** `echo 1 > /sys/module/mvc3d/parameters/hw_2d_view` (root) és
  `--mediacodec-mvc-hw=2`: 2D-ben a függő (jobb) nézet látszik.

Állapot: a v0.2.20-poc-ra épül (benne a 0033-as SSIF-javítás). TV-n (2D) tesztelve: kép, tekerés, módváltás
(0.2.8-as alapon), és a BD3D_DEMO .ssif jobb nézete a `hw_2d_view` kapcsolóval (v0.2.19-alapon, a 0033-mal tiszta,
nélküle ~10 s-onként hibás). A HW-dekóder ezen a lemezen az edge264-gyel képről képre bitazonos. A frame packed
3D projektoros tesztje hátravan. Ez az ág nem kiadás.

### S905X4 (SC2, Dune HD Pro Vision 4K, r25)

Külön modul: `kernel/mvc3d/sc2` (`/dev/mvc3dhw`, `MVC3D_HW_F_DIRECT`), a lib SoC szerint választ (`MVC3D_KO_SC2`). Szoftveres MVC ezen a
SoC-on nincs: ha a hardveres dekóder nem indul, a MediaCodec 2D bázisnézete megy. A modul 2026.11.06.8-sc2 a dekóder 31 bites DMA-hibáját
kerüli meg (a munkaterület és a referenciapufferek 2 GB alatti, 28 MB-os poolban; ami nem fér el, azt elutasítja), és r25-kódmintával
ellenőrzött kprobe-okkal pótolja, amit a Dune 5.4 `aml_media` az MVC frame packingből kihagy a CoreELEC 5.15-höz képest (`video_vf_get`,
`vd_set_dcu`, `vpp_set_filters`, `config_vd_position`). Projektoron a CineFuze teszt-APK-val próbálva (.ssif, mkv, mk3d, m2ts, ISO); a VLC
FEL appal még nem. Az appból a modul a film előtt élesíthető: „arm” a `/dev/mvc3dhw`-re (a VLC FEL ezt még nem használja, ott a módváltás
a kép után jön).

## English

The S928X's own MVC decoder corrupts the dependent view when `frame_num` wraps around. CoreELEC's fix
(github.com/CoreELEC/xbmc `acef2818e8`) rewrites the stream to 16 bit `frame_num` (pictures stay bit-identical);
`libmvc3dplay/src/mvcfnr.c` is its C port. Measured per picture against edge264: with the rewrite every picture
of both views is bit-identical (Gravity 713/713, Avatar 1384/1384); without it the views break at the wrap.

This branch plays 3D MVC on that decoder: libmvc3dplay feeds it (`/dev/amstream_vbuf`), mvc3d 2026.10.18
(`/dev/mvc3dhw`) composes each picture into the player's uvm buffers, and the player queues them into a video
tunnel at their display time; the surface's sideband stream makes the HWC show it. VLC (patch 0034) does so with
an `mc_api` of its own instead of MediaCodec (`--mediacodec-mvc-hw`: 0 software, 1 hardware, 2 hardware also in
2D). App patch 0003 switches the HDMI rate through Amlogic SystemControl when the firmware's AFR did not
(since VLC patch 0032 the AFR is told the frame rate and should switch by itself).
For tests on a display without 3D, `hw_2d_view=1` (mvc3d module parameter) with `--mediacodec-mvc-hw=2` shows the
dependent (right) view in 2D.
Built on v0.2.20-poc (with the .ssif fix 0033). Tested on a 2D TV: picture, seeks and the rate switch (on the
v0.2.8 base), and the right view of Dune HD's BD3D_DEMO .ssif with `hw_2d_view` (clean with 0033, a glitch about
every 10 s without it). On that disc the hardware decoder is bit-identical to edge264 for every picture. Frame
packed 3D still to be tested on a projector. This branch is not a release.

### S905X4 (SC2, Dune HD Pro Vision 4K, r25)

Its own module: `kernel/mvc3d/sc2` (`/dev/mvc3dhw`, `MVC3D_HW_F_DIRECT`); the library picks by SoC (`MVC3D_KO_SC2`). There is no software
MVC on this SoC: when the hardware decoder does not start, MediaCodec's 2D base view plays. Module 2026.11.06.8-sc2 works around the
decoder's 31 bit DMA bug (work space and reference buffers in a 28 MB pool below 2 GiB; what does not fit is refused) and adds, with kprobes
checked against the r25 code, what Dune's 5.4 `aml_media` lacks for MVC frame packing compared with CoreELEC 5.15 (`video_vf_get`,
`vd_set_dcu`, `vpp_set_filters`, `config_vd_position`). Tested on a projector with the CineFuze test APK (.ssif, mkv, mk3d, m2ts, ISO), not
yet with the VLC FEL app. A player can arm frame packing before its film: write "arm" on `/dev/mvc3dhw` (VLC FEL does not do that yet: its
mode switch comes after the picture).
