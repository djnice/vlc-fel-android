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
             Dune HD Pro One 8K Plus, firmware 260303_1721_r24 (egy modul mindkettőre).
  Más firmware-en / modellen kérjük jelezd,
  hogyan működik (adb logcat -s dvfelplay: a telepítő a tesztelt firmware-től eltérő
  kernelszimbólumokat is kiírja).
- Root-hozzáférés a modul telepítéséhez. Ha a lejátszón ki van kapcsolva, a VLC FEL megkérdezi,
  bekapcsolhatja-e a telepítés idejére („Install”); utána visszakapcsolja.

3D MVC (BLU-RAY 3D)
-------------------
- MVC-s MKV / .mk3d fájlok (a függő nézet ugyanabban a sávban, pl. MakeMKV), 1080p vagy 720p.
- Ha a kijelző EDID-je 3D-t jelez, a lejátszás 3D-ben indul: a VLC FEL mindkét nézetet szoftveresen
  dekódolja, frame packing képet állít össze (1920x2205), a kimenet 1080p 24 Hz 3D (frame packing).
  A firmware AFR-je vált, 1080p-s és 2160p-s asztalról is működik.
- 2D-s kijelzőn a bázisnézet 2D-ben megy, mint eddig. Kézzel: Beállítások → Speciális → Egyéni
  libVLC opciók: --mediacodec-mvc=1 (mindig 2D) vagy --mediacodec-mvc=2 (mindig 3D).
- Az első 3D-s lejátszáskor a VLC FEL telepíti az mvc3d kernelmodult is (2026.10.12.2), a dvfel-hez
  hasonlóan (config/boot/mvc3d.sh, ha kell, root-kérdéssel).
- A GUI (VLC vezérlők, felirat) 3D-ben csak a bal szem képén látszik. Blu-ray mappa / ISO / m2ts
  (a függő nézet külön PID-en) egyelőre 2D-ben megy.
- Seek után 1-2 képkocka kimaradhat, amíg a szoftveres dekóder utoléri.

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
          Dune HD Pro One 8K Plus, firmware 260303_1721_r24 (one module build for both).
  On other firmware / models please report how it
  works (adb logcat -s dvfelplay: the installer also lists kernel symbols whose versions differ from
  the tested firmware).
- Root access for the module installation. If it is switched off on the player, VLC FEL asks whether
  to switch it on for the installation ("Install"), and switches it off again afterwards.

3D MVC (BLU-RAY 3D)
-------------------
- MVC MKV / .mk3d files (the dependent view in the same track, e.g. MakeMKV), 1080p or 720p.
- When the display's EDID reports 3D, playback starts in 3D: VLC FEL decodes both views in software,
  composes a frame packed picture (1920x2205), the output is 1080p 24 Hz 3D (frame packing). The
  firmware's AFR switches; works from 1080p and from 2160p desktops.
- On 2D displays the base view plays in 2D as before. By hand: Settings → Advanced → Custom libVLC
  options: --mediacodec-mvc=1 (always 2D) or --mediacodec-mvc=2 (always 3D).
- The first 3D playback also installs the mvc3d kernel module (2026.10.12.2), like dvfel
  (config/boot/mvc3d.sh, with the root question if needed).
- In 3D the GUI (VLC's controls, subtitles) shows in the left eye's view only. Blu-ray folders / ISO /
  m2ts (the dependent view in a separate PID) play in 2D for now.
- After a seek 1-2 frames may be skipped while the software decoder catches up.

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
