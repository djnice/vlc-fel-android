VLC FEL – Dolby Vision Profile 7 FEL lejátszás S928X-es Dune HD / R_volution lejátszókon (Android)
=================================================================================================
PROOF OF CONCEPT – kísérleti build, saját felelősségre.

(English below)

MI EZ
-----
VLC for Android (3.7.2 Beta 3) build, amely a Dolby Vision Profile 7 FEL (Full Enhancement Layer)
tartalmat teljes minőségben játssza le: a BL-t és az EL-t is hardveresen dekódolja, a két réteget
a GPU-n komponálja (12 bites rekonstrukció, ditherrel), a kimenet Dolby Vision.
Külön alkalmazásként települ („VLC FEL”, org.videolan.vlc.fel), a hivatalos VLC mellett is futhat,
saját beállításokkal.

A 0.2.0 óta a 3D MVC (Blu-ray 3D) filmeket is 3D-ben játssza, mindkét nézet teljes felbontásban,
HDMI 1.4 frame packinggel (1080p24), 3D-s TV-n / projektoron (lásd lent: 3D MVC).

A 0.2.1 óta a kétsávos (DTDL) Profile 7 FEL fájlokat is (az EL külön sávban / PID-en: MP4, m2ts,
Ultra HD Blu-ray stream), és a 3D-t Blu-ray m2ts / .ssif fájlokból is.

A 0.2.2 óta demo mód is van: osztott kép, a FEL és a BL összehasonlítása mért fényerővel (lásd lent).

Előfutással működik: a VLC minden képet
3 vsync-kel (24 Hz-en 125 ms) korábban ad ki, a kernelmodul pedig a kész képet a saját vsyncjéig
visszatartja. Így a GPU-nak ~2,5 képkockányi ideje van ~1 helyett: a lejátszás közbeni GPU-kiugrások
nem okoznak akadást, és a kép a hanggal szinkronban van.
Mérve (4K 24p P7 FEL, 2-2 perc): 0 ismételt / eldobott képkocka Dune HD Pro 8K Plus-on (R22,
MMR-es króma; előfutás nélkül ~25 percenként) és Dune HD Pro One 8K Plus-on (R24).

FELTÉTELEK
----------
- Amlogic S928X-es Dune HD vagy R_volution lejátszó, Dune HD R22-es vagy R24-es Android firmware-rel.
  Tesztelve: Dune HD Pro 8K Plus, firmware 250815_1012_r22;
             Dune HD Pro One 8K Plus, firmware 260303_1721_r24 és a 2026-08-27-i R24 frissítés
             (egy modul mindegyikre).
  Más firmware-en / modellen kérjük jelezd,
  hogyan működik (adb logcat -s dvfelplay: a telepítő a tesztelt firmware-től eltérő
  kernelszimbólumokat is kiírja).
- Root-hozzáférés a modul telepítéséhez. Ha a lejátszón ki van kapcsolva, a VLC FEL megkérdezi,
  bekapcsolhatja-e a telepítés idejére („Install”); utána visszakapcsolja.

DEMO MÓD (FEL vs. BL)
---------------------
- Osztott kép P7 FEL-hez: a bal fél a teljes FEL-kompozíció, a jobb fél a BL úgy, ahogy egy FEL-t nem
  ismerő lejátszó mutatja (P8.1). Mindkét fél tetején a mért fényerő (L1: a max(R', G', B') maximuma és
  átlaga nitben, a fekete sávok nélkül), bal oldalon az RPU saját L1-e és a mért terület, jobb oldalon a
  BL és a különbség (FEL-BL).
- Be/ki lejátszás közben: lejátszó menü (⋯) → „Dolby Vision demo: on / off” (csak FEL-kompozíció
  közben látszik, az adott lejátszásra érvényes; minden fájl demó nélkül indul).
- Mindig bekapcsolva: Beállítások → Speciális → Egyéni libVLC opciók: --mediacodec-dovi-demo
- Az „RPU L1” a fájl RPU-jában tárolt érték; egyes fájlokban nincs vagy nulla.

KÉTSÁVOS (DTDL) FEL
-------------------
- Profile 7 FEL, ahol az EL (és az RPU) külön sávban / PID-en van: kétsávos MP4 (dvhe/dvh1 EL-sáv),
  m2ts / ts (Dolby Vision leíró, vagy Ultra HD Blu-ray: EL a 0x1015-ös PID-en). A VLC demuxa az EL-t
  a BL azonos idejű képéhez fűzi, utána ugyanúgy megy, mint egy egysávos (STDL) fájl.
- Tesztelve LG OLED-en: két DTDL MP4, ugyanezek m2ts-ben, és egy Ultra HD Blu-ray főfilm-m2ts
  részlete (80 Mbps, FEL): minden kép komponálva, szünet és ugrás is.
- Blu-ray ISO / mappa és töredezett (fragmented) MP4 nem támogatott.

3D MVC (BLU-RAY 3D)
-------------------
- MVC-s MKV / .mk3d fájlok (a függő nézet ugyanabban a sávban, pl. MakeMKV), 1080p vagy 720p.
- 0.2.1 óta: Blu-ray m2ts, a függő nézet külön PID-en (pl. tsMuxeR), és a lemez .ssif fájljai
  (BDMV/STREAM/SSIF/*.ssif; ha a fájlböngésző nem mutatja, nevezd át .m2ts-re).
- Ha a kijelző EDID-je 3D-t jelez, a lejátszás 3D-ben indul: a VLC FEL mindkét nézetet szoftveresen
  dekódolja, frame packing képet állít össze (1920x2205), a kimenet 1080p 24 Hz 3D (frame packing).
  A firmware AFR-je vált, 1080p-s és 2160p-s asztalról is működik.
- 2D-s kijelzőn a bázisnézet 2D-ben megy, mint eddig. Kézzel: Beállítások → Speciális → Egyéni
  libVLC opciók: --mediacodec-mvc=1 (mindig 2D) vagy --mediacodec-mvc=2 (mindig 3D).
- Az első 3D-s lejátszáskor a VLC FEL telepíti (vagy frissíti) az mvc3d kernelmodult (2026.10.12.3),
  a dvfel-hez hasonlóan (config/boot/mvc3d.sh, ha kell, root-kérdéssel).
- A GUI (VLC vezérlők, felirat) 3D-ben csak a bal szem képén látszik. Blu-ray ISO / mappa 2D-ben megy
  (a VLC Blu-ray-kezelője csak a bázisnézetet adja).
- Seek után 1-2 képkocka kimaradhat, amíg a szoftveres dekóder utoléri; Blu-ray lemezről (nyitott GOP)
  egy-egy ugrás után ~1 másodpercig 2D lehet a kép.
- Egyes projektorok 2160p-s asztalról frame packingre váltáskor bontják a HDMI-kapcsolatot, ettől
  megáll a lejátszás: ilyenkor 1080p-s asztalt használj.

TELEPÍTÉS
---------
1. Az APK telepítése (pendrive + fájlkezelő, „Downloader” app, vagy: adb install VLC-FEL-*.apk).
2. Az első FEL-fájl lejátszásakor a VLC FEL telepíti a kernelmodult (dvfel.ko, 2026.10.07.1) a
   firmware boot-könyvtárába (<app>/config/boot/dvfel.sh), és be is tölti; ha kell, előtte
   rákérdez a root-hozzáférésre. Ezután minden bekapcsoláskor magától betöltődik.
   Régebbi modulra a telepítő sosem cserél.
3. A FEL-es fájlok lejátszása automatikus, hálózati megosztásról (SMB/NFS) és helyi tárról is.
   Ha a telepítést elutasítottad, a FEL-fájlok EL nélkül (profile 8.1-ként) játszódnak; az
   alkalmazás adatainak törlése után újra rákérdez.

TUDNIVALÓK
----------
- A Dune automatikus frame rate (AFR) kezelése 23,976/24 Hz-re vált lejátszáskor. Stop után a Dune
  nem vált vissza, amíg vissza nem lépsz a Dune főképernyőjére (a firmware viselkedése minden
  külső lejátszónál).
- 4K 24p FEL valós időben megy. 4K 50/60p FEL-hez nem elég gyors.
- Ugrás (seek) után 1–2 képkocka csak az RPU-leképezéssel (EL nélkül) jelenhet meg, és az első
  képkockák egy vsync-kel késhetnek, amíg a lánc feltelik.
- A FEL-kompozíció kikapcsolható: Beállítások → Speciális → Egyéni libVLC opciók:
  --no-mediacodec-dovi-fel

VÁLTOZÁSOK
----------
0.2.5-poc (libdvfelplay 2026.10.15.1, VLC javítás, a modulok változatlanok) – tekerés után nem akad
          a film végéig: néha a FEL EL-dekódere egy tekerésnél beragadt, ettől kezdve minden kép késve,
          csak az alapréteggel ment, és a következő tekerés sem segített. A beragadt EL-dekódert 1,5 s
          után kiüríti, és a következő kulcsképtől újraindul. A libVLC a letiltottként jelölt sávot is
          kiválasztja, ha a fajtájából nincs engedélyezett (MKV FlagEnabled 0, MP4 tkhd).
0.2.4-poc (VLC javítás, a modulok és a libdvfelplay változatlanok) – 2160p-s kimeneten sem akad a
          felirat: 30 fps-ig a Dolby Vision képek 140 ms-mal előbb mennek a kijelzőre (eddig 80), így a
          feliratváltás 20–60 ms-os kirajzolása nem visz el képkockát. 1080p-n ezt már a 0.2.3 is tudta.
0.2.3-poc (VLC javítás, a modulok és a libdvfelplay változatlanok) – bekapcsolt felirattal nem akad:
          a FEL eddig a legtöbb feliratváltáskor eldobott egy képkockát (4K filmen ~40/perc, most
          1 alatt). A felirat a kijelző méretében készül (nem 4K-ban), és a kép a felirat előtt,
          időben kerül a kijelzőre.
0.2.2-poc (libdvfelplay 2026.10.13.1, modulok változatlanok) – FEL demo mód mért L1-gyel, a lejátszó
          menüjéből kapcsolható.
0.2.1-poc (dvfel 2026.10.07.1, mvc3d 2026.10.12.3) – kétsávos (DTDL) FEL: MP4 és m2ts (Ultra HD
          Blu-ray is); 3D m2ts-ből és .ssif-ből; az R24 2026-08-27-i frissítésén is 3D (0.2.0-val
          2D volt); a 3D nem akad el bizonyos ugrások után.
0.2.0-poc (dvfel 2026.10.07.1, mvc3d 2026.10.12.2) – 3D MVC (Blu-ray 3D) 3D-ben, HDMI frame packing.
0.1.0-poc (kernelmodul 2026.10.07.1) – első kiadás.

ELTÁVOLÍTÁS
-----------
- Az app eltávolítása Androidból.
- A kernelmodul boot-scriptjének törlése (ADB, root), ha más alkalmazás sem használja:
      adb root
      adb shell rm -r /data/data/com.dunehd.app/config/boot/dvfel.sh /data/data/com.dunehd.app/config/dvfel
      adb shell rm -r /data/data/com.dunehd.app/config/boot/mvc3d.sh /data/data/com.dunehd.app/config/mvc3d
  majd újraindítás.

FORRÁS
------
GPL v2+. https://github.com/djnice/vlc-fel-android

--------------------------------------------------------------------------------------------------

VLC FEL – Dolby Vision Profile 7 FEL playback on S928X Dune HD / R_volution players (Android)
==============================================================================================
PROOF OF CONCEPT – experimental build, use at your own risk.

WHAT IT IS
----------
A VLC for Android (3.7.2 Beta 3) build that plays Dolby Vision Profile 7 FEL (full enhancement layer)
content at full quality: base and enhancement layers are both decoded in hardware, composed on the
GPU (12-bit reconstruction with dither), output is Dolby Vision. It installs as a separate app
("VLC FEL", org.videolan.vlc.fel) next to the official VLC, with its own settings.

Since 0.2.0 it also plays 3D MVC (Blu-ray 3D) in 3D, both views at full resolution, as HDMI 1.4 frame
packing (1080p24), on 3D TVs / projectors (see 3D MVC below).

Since 0.2.1 also dual track (DTDL) Profile 7 FEL files (the EL in its own track / PID: MP4, m2ts,
Ultra HD Blu-ray streams), and 3D from Blu-ray m2ts / .ssif files.

Since 0.2.2 a demo mode: split screen comparing FEL and the base layer, with measured brightness (below).

It works with a lead time: VLC presents
every frame 3 vsyncs (125 ms at 24 Hz) early and the kernel module holds the composed frame back to
its vsync. The GPU gets ~2.5 frame periods instead of ~1: its time spikes during playback no longer
cause stutter, and the picture is in sync with the audio.
Measured (4K 24p P7 FEL, 2 minutes each): 0 repeated / dropped frames on a Dune HD Pro 8K Plus
(R22, MMR chroma; without the lead time ~25 a minute) and on a Dune HD Pro One 8K Plus (R24).

REQUIREMENTS
------------
- An Amlogic S928X Dune HD or R_volution player with Dune HD R22 or R24 Android firmware.
  Tested: Dune HD Pro 8K Plus, firmware 250815_1012_r22;
          Dune HD Pro One 8K Plus, firmware 260303_1721_r24 and its 2026-08-27 R24 update
          (one module build for all).
  On other firmware / models please report how it
  works (adb logcat -s dvfelplay: the installer also lists kernel symbols whose versions differ from
  the tested firmware).
- Root access for the module installation. If it is switched off on the player, VLC FEL asks whether
  to switch it on for the installation ("Install"), and switches it off again afterwards.

DEMO MODE (FEL vs. BASE LAYER)
------------------------------
- Split screen for P7 FEL: the left half with the full FEL composition, the right half the base layer
  as a player without FEL support shows it (P8.1). The top of each half shows the measured brightness
  (L1: max and average of max(R', G', B') in nits, letterbox bars left out); left the RPU's own L1 and
  the measured area, right the base layer and the difference (FEL-BL).
- On / off during playback: player menu (⋯) → "Dolby Vision demo: on / off" (shown while a FEL
  composition runs, for the current playback; every file starts without it).
- Always on: Settings → Advanced → Custom libVLC options: --mediacodec-dovi-demo
- "RPU L1" is the value the file's RPU carries; some files carry none or zeros.

DUAL TRACK (DTDL) FEL
---------------------
- Profile 7 FEL with the EL (and RPU) in its own track / PID: dual track MP4 (dvhe/dvh1 EL track),
  m2ts / ts (Dolby Vision descriptor, or Ultra HD Blu-ray: the EL on PID 0x1015). VLC's demux appends
  the EL to the base layer picture with the same time; it then plays like a single track (STDL) file.
- Tested on an LG OLED: two DTDL MP4s, the same as m2ts, and a cut of an Ultra HD Blu-ray main movie
  m2ts (80 Mbps, FEL): all frames composed, pause and seeks too.
- Blu-ray ISO / folders and fragmented MP4 are not supported.

3D MVC (BLU-RAY 3D)
-------------------
- MVC MKV / .mk3d files (the dependent view in the same track, e.g. MakeMKV), 1080p or 720p.
- Since 0.2.1: Blu-ray m2ts with the dependent view on its own PID (e.g. tsMuxeR), and the disc's
  .ssif files (BDMV/STREAM/SSIF/*.ssif; rename to .m2ts if the file browser does not list them).
- When the display's EDID reports 3D, playback starts in 3D: VLC FEL decodes both views in software,
  composes a frame packed picture (1920x2205), the output is 1080p 24 Hz 3D (frame packing). The
  firmware's AFR switches; works from 1080p and from 2160p desktops.
- On 2D displays the base view plays in 2D as before. By hand: Settings → Advanced → Custom libVLC
  options: --mediacodec-mvc=1 (always 2D) or --mediacodec-mvc=2 (always 3D).
- The first 3D playback also installs (or updates) the mvc3d kernel module (2026.10.12.3), like dvfel
  (config/boot/mvc3d.sh, with the root question if needed).
- In 3D the GUI (VLC's controls, subtitles) shows in the left eye's view only. Blu-ray ISO / folders
  play in 2D (VLC's Blu-ray access only gives the base view).
- After a seek 1-2 frames may be skipped while the software decoder catches up; from Blu-ray discs
  (open GOP) the picture may be 2D for ~1 second after some seeks.
- Some projectors drop the HDMI connection when switching from a 2160p desktop to frame packing,
  which stops playback: use a 1080p desktop then.

INSTALLATION
------------
1. Install the APK (USB stick + file manager, the "Downloader" app, or adb install VLC-FEL-*.apk).
2. When it plays the first FEL file, VLC FEL installs the kernel module (dvfel.ko, 2026.10.07.1) into
   the firmware's boot hook directory (<app>/config/boot/dvfel.sh) and loads it, asking for root
   access first if needed. It then loads at every boot.
   The installer never replaces a newer module with an older one.
3. FEL files are handled automatically, from network shares (SMB/NFS) and local storage alike.
   If you declined the installation, FEL files play without the EL (as profile 8.1); clearing the
   app's data makes it ask again.

NOTES
-----
- The Dune auto frame rate (AFR) switches to 23.976/24 Hz for playback and does not switch back after
  stop until you return to the Dune home screen (firmware behaviour with all third-party players).
- 4K 24p FEL plays in real time; 4K 50/60p FEL is too heavy.
- After a seek, 1–2 frames may be shown with the RPU mapping only (no EL), and the first frames may
  be a vsync late while the chain fills.
- FEL composition can be switched off: Settings → Advanced → Custom libVLC options:
  --no-mediacodec-dovi-fel

CHANGES
-------
0.2.5-poc (libdvfelplay 2026.10.15.1, VLC fix, modules unchanged) – no stutter for the rest of the
          film after a seek: once in a while the FEL's EL decoder got stuck at a seek, from then on every
          frame was late and base layer only, and the next seek did not help. A stuck EL decoder is now
          emptied after 1.5 s and starts again at its next key picture. libVLC selects a track marked
          disabled when none of its kind is enabled (MKV FlagEnabled 0, MP4 tkhd).
0.2.4-poc (VLC fix, modules and libdvfelplay unchanged) – no subtitle stutter with a 2160p output
          either: up to 30 fps Dolby Vision pictures reach the display 140 ms ahead (was 80), so the
          20–60 ms a subtitle change takes to render no longer costs a frame. 0.2.3 did so at 1080p.
0.2.3-poc (VLC fix, modules and libdvfelplay unchanged) – no stutter with subtitles on: FEL
          dropped a frame at most subtitle changes (~40 per minute on a 4K film, now under 1). The
          subtitles are drawn at the display's size (not in 4K), and the picture reaches the display
          at its time, before the subtitle.
0.2.2-poc (libdvfelplay 2026.10.13.1, modules unchanged) – FEL demo mode with measured L1, switchable
          from the player menu.
0.2.1-poc (dvfel 2026.10.07.1, mvc3d 2026.10.12.3) – dual track (DTDL) FEL: MP4 and m2ts (Ultra HD
          Blu-ray too); 3D from m2ts and .ssif; 3D on the R24 2026-08-27 update too (2D with 0.2.0);
          3D no longer stalls after some seeks.
0.2.0-poc (dvfel 2026.10.07.1, mvc3d 2026.10.12.2) – 3D MVC (Blu-ray 3D) in 3D, HDMI frame packing.
0.1.0-poc (kernel module 2026.10.07.1) – first release.

UNINSTALL
---------
- Uninstall the app in Android.
- If no other app uses it: remove the module boot script (ADB, root):
      adb root
      adb shell rm -r /data/data/com.dunehd.app/config/boot/dvfel.sh /data/data/com.dunehd.app/config/dvfel
      adb shell rm -r /data/data/com.dunehd.app/config/boot/mvc3d.sh /data/data/com.dunehd.app/config/mvc3d
  and reboot.

SOURCE
------
GPL v2+. https://github.com/djnice/vlc-fel-android
