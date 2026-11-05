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
- **VLC:** `vlc/patches/0033`: a mediacodec modul saját `mc_api`-ja (MediaCodec helyett, újraindítással váltva;
  ha nem indul, MediaCodec + a szoftveres út). Opció: `--mediacodec-mvc-hw` = 0 szoftveres, 1 hardveres
  (alapértelmezett, 3D kijelzőn), 2 hardveres 2D-ben is (teszt).
- **HDMI-ráta:** a Dune AFR a MediaCodec-dekódertől tudja a film fps-ét. A 0032 óta a lejátszás elején
  megnyitott MediaCodec megkapja a frame rate-et, így az AFR várhatóan magától vált; ha mégsem, a
  `vlc/android-patches/0003` (DuneDesktopMode) 8 s után, ha a mód rátája nem a filmé, SystemControl-lal vált
  (pl. 2160p60 → 2160p24, 23,976 Hz), a végén visszaáll. 3D-ben ebből lesz 1080p24 frame packing.

Állapot: TV-n (2D) tesztelve (kép, tekerés, módváltás) a 0.2.8-as alapon; a v0.2.19-poc-ra igazítva lefordul,
dobozon még nem próbáltuk. A frame packed 3D projektoros tesztje hátravan. Ez az ág nem kiadás.

## English

The S928X's own MVC decoder corrupts the dependent view when `frame_num` wraps around. CoreELEC's fix
(github.com/CoreELEC/xbmc `acef2818e8`) rewrites the stream to 16 bit `frame_num` (pictures stay bit-identical);
`libmvc3dplay/src/mvcfnr.c` is its C port. Measured per picture against edge264: with the rewrite every picture
of both views is bit-identical (Gravity 713/713, Avatar 1384/1384); without it the views break at the wrap.

This branch plays 3D MVC on that decoder: libmvc3dplay feeds it (`/dev/amstream_vbuf`), mvc3d 2026.10.18
(`/dev/mvc3dhw`) composes each picture into the player's uvm buffers, and the player queues them into a video
tunnel at their display time; the surface's sideband stream makes the HWC show it. VLC (patch 0033) does so with
an `mc_api` of its own instead of MediaCodec (`--mediacodec-mvc-hw`: 0 software, 1 hardware, 2 hardware also in
2D). App patch 0003 switches the HDMI rate through Amlogic SystemControl when the firmware's AFR did not
(since VLC patch 0032 the AFR is told the frame rate and should switch by itself).
Tested on a 2D TV on the v0.2.8 base; rebased on v0.2.19-poc (builds, not yet run on a box); frame packed 3D
still to be tested on a projector. This branch is not a release.
