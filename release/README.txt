VLC FEL – Dolby Vision Profile 7 FEL lejátszás S928X-es Dune HD / R_volution és S905X4-es Dune HD
lejátszókon (Android)
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

A 0.2.8 óta a hangot bitstreamként (passthrough) is kiadhatja az erősítőnek: Dolby TrueHD (Atmos),
DTS-HD MA / DTS:X, E-AC-3 (Atmos), AC-3, DTS (lásd lent: HANG BITSTREAMKÉNT).

A 0.2.10 óta Amlogic S905X4-es Dune HD lejátszókon is megy: ott a két réteget a SoC saját Dolby
Vision magja rakja össze (lásd lent: S905X4).

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
  Vagy Amlogic S905X4-es Dune HD lejátszó R24-es vagy r25-ös Android firmware-rel.
  Tesztelve: Dune HD Pro Vision 4K, firmware 261021_0117_r25 (és a 0.2.10–0.2.11 a 260303_1721_r24-en;
             R22-n nem tesztelt).
  Más firmware-en / modellen kérjük jelezd,
  hogyan működik (adb logcat -s dvfelplay: a telepítő a tesztelt firmware-től eltérő
  kernelszimbólumokat is kiírja).
- Root-hozzáférés a modul telepítéséhez. Ha a lejátszón ki van kapcsolva, a VLC FEL megkérdezi,
  bekapcsolhatja-e a telepítés idejére („Install”); utána visszakapcsolja.
- USB-s / belső meghajtók: a lejátszó médiaindexe kihagyja őket, ezért Android 11-en csak a „Minden
  fájl elérése” engedéllyel látszanak a fájljaik (a tárhely-engedéllyel csak a mappák), és ehhez a
  firmware-ben nincs beállítási oldal. A 0.2.9 óta a VLC FEL induláskor rootból megadja magának
  (ha a root ki van kapcsolva, egy másodpercnél rövidebb időre bekapcsolja, utána visszaállítja).

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
- Csak S928X-en: S905X4-en a FEL-képet a SoC Dolby Vision magja rakja össze, a menüpont ott
  „Dolby Vision demo: not available”.

S905X4 (DUNE HD)
----------------
- Az S905X4 Dolby Vision magja maga is fogad EL-t; az Android-lejátszók MediaCodec-dekódere viszont
  csak az alapréteget adja ki. A VLC FEL az EL-t egy második dekóderrel dekódolja, és a dvfel
  kernelmodul S905X4-es változata (2026.10.29.1) képenként átadja a Dolby Vision meghajtónak, a hozzá
  tartozó alapréteg-képpel párosítva. GPU-munka nincs, az RPU marad profile 7.
- A Dune S905X4-es firmware-ének (R24 és r25) Dolby Vision meghajtójából (aml_media.ko) hiányzik négy
  FEL-javítás: a meghajtó minden képkockánál eldobja az EL-párosítást (az EL-dekóder kifogy a
  pufferekből), két DOLBY_PATH_CTRL-feltételből kimaradt az S905X4, és az EL-képet a VD2 helyett a
  VD1 réteg regisztereibe írja. A modul 0.2.12 óta futás közben pótolja őket (kprobe-ok a meghajtó
  utasításain, a környezetük alapján megkeresve); olyan meghajtón, amelyben már benne vannak, vagy
  egy ismeretlen buildnél, magától kimarad. A kernelnaplóban: „dvfel: fix ... at ...”.
- A modult a VLC FEL ugyanúgy telepíti, mint S928X-en (a lejátszó típusa szerint választja ki).
- Tesztelve Dune HD Pro Vision 4K-n (r25, a gyári firmware-rel), TV-n nézve: lejátszás (mp4, mkv,
  m2ts), szünet, tekerés, a modul frissítése root-kérdéssel és a betöltés bekapcsoláskor.
- A lejátszás elején a kép fekete marad, amíg az EL folyamatosan meg nem jön (0,3–4 s, legfeljebb
  10 s): az alapréteg első képkockái az EL-dekóder képei előtt jönnek, és EL nélkül egy FEL-kép zöld
  lenne.
- Tekerés után néhány képkocka (legfeljebb kb. egy másodperc, az EL következő kulcsképéig) EL nélkül
  jelenik meg. Szünet közben az EL-dekóder néhány
  „wait fence” időtúllépést ír a naplóba.
- Demó mód nincs (lásd fent). 3D MVC csak S928X-en: az mvc3d modul S905X4-en nem töltődik be;
  3D-s kijelzővel a telepítő egyszer rákérdezhet a rootra, utána hibát jelez, az MVC 2D-ben megy.

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

HANG BITSTREAMKÉNT (PASSTHROUGH)
--------------------------------
- Bekapcsolás: Beállítások → Hang → „Digitális hangkimenet (áteresztés)” (angol felületen: „Digital
  audio output (passthrough)”). Alapból ki: ekkor a VLC dekódol, a hang PCM-ként megy.
- Bekapcsolva a VLC FEL a hangot dekódolás nélkül, IEC 61937-be csomagolva adja a HDMI-re, az
  erősítő dekódol. Azokat a formátumokat küldi így, amelyeket a HDMI-kimenet és a csatlakozott eszköz
  (EDID) is fogad; a többi (pl. AAC) PCM marad. Root nem kell hozzá.
- Dolby TrueHD (Atmos): MAT, 8 csatorna, 192 kHz (HBR). A TrueHD-t az Amlogic HDMI-kimenet EDID-
  adatából („MAT”) ismeri fel, mert az Android a HDMI-eszköz kódolásai közt nem listázza.
- DTS-HD MA / DTS:X: 8 csatorna, 192 kHz (HBR); a gyári VLC 3 csak a DTS core-t adta át. Egy DTS-sáv
  az elején egy pillanatra 48 kHz-es core-ként indul, utána vált HD-ra.
- E-AC-3 (Atmos: JOC): 192 kHz; AC-3 és DTS: 48 kHz.
- A hang bitstreamként is szinkronban van a képpel (a gyári VLC 3 passthrough-ban a pufferelés
  mértékével előbb adta ki a hangot). Ha mégis eltolódást látsz, a lejátszó hangkésleltetésével
  állítható.
- Sávváltáskor ~1,5 s csend, amíg a kimenet újraindul (PCM-nél is így van); az erősítő ilyenkor
  újraszinkronizál.
- Tesztelve Dune HD Pro 8K Plus-on (R22): TrueHD, DTS-HD MA, E-AC-3, AC-3, DTS; a kimenet formátuma
  (pl. TrueHD: MAT, 8 csatorna, 192 kHz) a box HDMI-meghajtójában ellenőrizve, sávváltás és tekerés
  rendben. Kérjük jelezd, mit mutat az erősítőd kijelzője.

TELEPÍTÉS
---------
1. Az APK telepítése (pendrive + fájlkezelő, „Downloader” app, vagy: adb install VLC-FEL-*.apk).
2. Az első FEL-fájl lejátszásakor a VLC FEL telepíti a kernelmodult (dvfel.ko, 2026.10.29.2;
   S905X4-en 2026.10.29.1) a
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
- 4K 24p FEL valós időben megy. 4K 50/60p FEL-hez S928X-en nem elég gyors (S905X4-en nem tesztelt).
- S928X-en a Dune HD r25-ös firmware-e maga hozza a modult: ott a VLC FEL a firmware moduljával
  megy, nem telepít semmit. (S905X4-en az r25 nem hoz modult, ott a VLC FEL telepíti.)
- Ugrás (seek) után 1–2 képkocka csak az RPU-leképezéssel (EL nélkül) jelenhet meg, és az első
  képkockák egy vsync-kel késhetnek, amíg a lánc feltelik.
- A FEL-kompozíció kikapcsolható: Beállítások → Speciális → Egyéni libVLC opciók:
  --no-mediacodec-dovi-fel

VÁLTOZÁSOK
----------
0.2.15-poc (libdvfelplay 2026.10.29.2, S928X-re dvfel 2026.10.29.2; az S905X4-modul, a VLC- és
          app-javítások és a 3D változatlanok) – S928X-en egy gyors váltás után nem veszhet el a FEL az
          egész lejátszásra: ha egy FEL-lejátszás egy-két másodpercen belül követett egy másikat (Blu-ray
          lemez átadása a Dune HD menüs lejátszójától egy másik lejátszónak, vagy gyors újraindítás), a
          modul az előző kompozitor puffereinek újrafoglalása előtt megvárta, hogy kiürüljenek a sorai,
          közben viszont minden képkocka kompozíció nélkül ezekbe a sorokba került (a 125 ms-os előfutás
          1–3 képkockát tart ott), így sosem ürültek ki, és az egész film EL nélkül (demó mód nélkül is)
          ment. Most csak a saját puffereinek sorban álló képkockái számítanak. A demó mód sem jelenik meg
          egy másodpercre az indításkor, ha az előző lejátszás bekapcsolt demóval ért véget. Dune HD r25-ön
          a firmware modulja megy: azt a Dune HD plugin 2026.10.29.2-0.2.15 frissíti.
0.2.14-poc (libdvfelplay 2026.10.29.1, S905X4-re dvfel 2026.10.29.1; az S928X-modul, a VLC- és
          app-javítások és a 3D változatlanok) – S905X4-en az első EL-kép után sincs zöld kép a
          FEL-lejátszás elején: a 0.2.13 az első EL-lel párosított képkockánál visszakapcsolta a videót,
          de az EL-dekóder utána még egyszer megáll (felbontásváltás), és 1–2 s-ig még zöld volt a kép
          (a Dune HD jelezte a fel_test.mp4-gyel). A modul most addig némít, amíg 6 képkocka egymás után
          meg nem kapja az EL-jét, és az első 48-on belül egy kimaradásnál újra némít (összesen
          legfeljebb 10 s). Tekerés és szünet változatlan.
0.2.13-poc (libdvfelplay 2026.10.28.4, S905X4-re dvfel 2026.10.28.4; az S928X-modul, a VLC- és
          app-javítások és a 3D változatlanok) – S905X4-en nincs zöld kép a FEL-lejátszás elején: az
          alapréteg első képkockái 0,3–4 s-mal az EL-dekóder első képe előtt jönnek, és ezeket a Dolby
          Vision mag EL nélkül zöldnek mutatta. A modul most a lejátszás elejétől az első EL-lel
          párosított képkockáig (legfeljebb 8 s) elnémítja a videót; a lejátszás vége (vagy a modul
          eltávolítása) mindig visszakapcsolja. Tekerés és szünet változatlan. HDR10, SDR és Dolby Vision
          kimenettel is.
0.2.12-poc (libdvfelplay 2026.10.28.3, S905X4-re dvfel 2026.10.28.3; az S928X-modul, a VLC- és
          app-javítások és a 3D változatlanok) – S905X4-en a gyári Dune firmware-rel (R24, r25) is megy
          a FEL: a firmware Dolby Vision meghajtójából hiányzó négy FEL-javítást a modul futás közben
          pótolja. Nélkülük az EL-dekóder néhány kép után kifogyott a pufferekből (az EL elmaradt, a kép
          fekete, zöld vagy torz lett). A korábbi verziókat egy olyan tesztlejátszón próbáltuk, amelyen
          egy saját, javított meghajtó futott, ezért ez nem jött elő. r25-ön tesztelve.
0.2.11-poc (libdvfelplay 2026.10.28.2, S905X4-re dvfel 2026.10.28.2; az S928X-modul, a VLC- és
          app-javítások és a 3D változatlanok) – S905X4-en nincs zöld kép / hiányzó EL, miután a
          lejátszót leállították (force-stop) vagy összeomlott: ilyenkor a kijelzőn lévő EL-képek
          kiadási jelzése késve ment ki, és az Android dekóderszolgáltatása leállásonként 9 EL-puffert
          (~28 MB) bent tartott. Néhány ilyen leállás után elfogyott a dekódermemória, a FEL-fájlok zöld
          képpel (üres EL) vagy EL nélkül mentek, újraindításig. A jelzés most a lejátszás végén azonnal
          kimegy. Minden lejátszás egy összegző sort ír a kernelnaplóba (dvfel: session N: ...), a
          libdvfelplay 5 másodpercenként egy sort a hibajelentésekhez.
0.2.10-poc (libdvfelplay 2026.10.25.2, dvfel 2026.10.23.3 és S905X4-re 2026.10.25.4, VLC- és
          app-javítás; a 3D változatlan) – Amlogic S905X4-es Dune HD lejátszók: a P7 FEL-t a SoC
          Dolby Vision magja rakja össze, az EL-t a VLC FEL dekódolja, és az új S905X4-es modul adja át
          a meghajtónak (lásd fent); a demó menüpontja ott „not available”. S928X-en a csomagolt modul
          a Dune HD plugin 2026.10.23.3-as modulja (a javításai a firmware saját lejátszójának szólnak,
          a VLC FEL ugyanúgy játszik); a r25-ös firmware-en a firmware modulját használja, nem telepít
          (a firmware minden bekapcsoláskor kiveszi a más verziójú modult, a VLC FEL különben minden
          induláskor rootot kérne). Kétsávos (dual PID) m2ts-nél nincs FEL-kiesés (a1d4r javítása):
          a ts demux néha egy alapréteg-képet a hozzá tartozó EL előtt továbbengedett, az EL és RPU
          nélkül ment.
0.2.9-poc (app-javítás, a modulok, a libdvfelplay és a VLC-javítások változatlanok) – látszanak az
          USB-s / belső meghajtók fájljai (eddig csak a mappáik): a lejátszó médiaindexe kihagyja a
          meghajtókat, Android 11-en pedig „Minden fájl elérése” nélkül egy app ott csak az indexelt
          fájlokat látja, és a firmware-ben nincs oldal ennek a megadására. A VLC FEL most induláskor
          rootból megadja magának: su-val, vagy ha a root ki van kapcsolva, egy pillanatra (~0,7 s)
          bekapcsolja, utána visszaállítja (egy közbeni összeomlás után a következő induláskor).
0.2.8-poc (VLC-javítások, a modulok és a libdvfelplay változatlanok) – hang bitstreamként (HDMI
          passthrough): TrueHD (MAT) és DTS-HD MA 8 csatornás 192 kHz-es HBR-en (eddig a TrueHD-t nem
          ismerte fel, a DTS-HD-ból csak a core ment át), E-AC-3, AC-3, DTS; a hang passthrough-ban is
          szinkronban a képpel; sávváltás után nem esik vissza PCM-re (az épp elengedett kimenetet az
          Android ~1 s-ig nem adja ki újra, most újrapróbálja).
0.2.7-poc (libdvfelplay 2026.10.16.1, a modulok és a VLC-javítások változatlanok) – nem akad a FEL
          demó mód: a demó L1-mérését minden képkockánál várakozással olvasta vissza a GPU-ról, ettől egy
          nehéz 4K 24p filmnél 100 képből 30–39 lekéste a vsync-et. Most minden 6. képet mér, és
          várakozás nélkül olvassa vissza (a kijelzés úgyis másodpercenként 4-szer frissül).
0.2.6-poc (dvfel 2026.10.15.1, libdvfelplay 2026.10.15.3) – nincs kernelpánik gyors leállítás /
          indítás / tekerés után: egy lejátszás indulásakor a dvfel a saját képmásolatát adhatta vissza
          a video composernek; most minden képet pontosan egyszer ad vissza. A boot-szkript kikapcsolja
          az Amlogic H.265 dekóder back-end „fast check”-jét is, amely szintén kernelpánikot okozhatott.
          Nem villog az olyan egysávos fájl, amelynek a képeinél részben hiányzik az EL és az RPU
          (kétrétegű forrásból, eltérő EL-képsorrenddel muxolva). Frissítéskor a modult is lecseréli.
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

VLC FEL – Dolby Vision Profile 7 FEL playback on S928X Dune HD / R_volution and S905X4 Dune HD
players (Android)
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

Since 0.2.8 it can send the audio to the AV receiver as a bitstream (passthrough): Dolby TrueHD (Atmos),
DTS-HD MA / DTS:X, E-AC-3 (Atmos), AC-3, DTS (see AUDIO BITSTREAM below).

Since 0.2.10 also on Amlogic S905X4 Dune HD players, where the SoC's own Dolby Vision core composes
the two layers (see S905X4 below).

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
  Or an Amlogic S905X4 Dune HD player with the R24 or r25 Android firmware.
  Tested: Dune HD Pro Vision 4K, firmware 261021_0117_r25 (and 0.2.10–0.2.11 on 260303_1721_r24;
          R22 not tested).
  On other firmware / models please report how it
  works (adb logcat -s dvfelplay: the installer also lists kernel symbols whose versions differ from
  the tested firmware).
- Root access for the module installation. If it is switched off on the player, VLC FEL asks whether
  to switch it on for the installation ("Install"), and switches it off again afterwards.
- USB / internal drives: the player's media index leaves them out, so on Android 11 their files show
  only with "All files access" (the storage permission shows the folders only), and the firmware has
  no settings page for it. Since 0.2.9 VLC FEL grants it to itself with root at its start (if the
  root access is off, it is switched on for under a second and then restored).

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
- S928X only: on the S905X4 the SoC's Dolby Vision core composes the FEL picture, the menu item shows
  "Dolby Vision demo: not available" there.

S905X4 (DUNE HD)
----------------
- The S905X4's Dolby Vision core takes an EL itself, but the MediaCodec decoder of Android players
  outputs the base layer only. VLC FEL decodes the EL with a second decoder, and the S905X4 build of
  the dvfel kernel module (2026.10.29.1) hands each EL picture to the Dolby Vision driver, paired with
  its base layer picture. No GPU work, the RPU stays profile 7.
- The Dolby Vision driver (aml_media.ko) of the Dune S905X4 firmware (R24 and r25) lacks four FEL
  fixes: the driver drops its BL/EL pairing on every frame (the EL decoder runs out of buffers), two
  DOLBY_PATH_CTRL conditions leave out the S905X4, and it writes the EL picture into the VD1 layer's
  registers instead of VD2's. Since 0.2.12 the module applies them at run time (kprobes on the driver's instructions, found
  by the code around them); on a driver that already has them, or an unknown build, it leaves them
  out by itself. The kernel log lists them: "dvfel: fix ... at ...".
- VLC FEL installs the module as on the S928X (it picks the build by the player's SoC).
- Tested on a Dune HD Pro Vision 4K (r25, the stock firmware), watched on the TV: playback (mp4, mkv,
  m2ts), pause, seeks, the module update with the root question and the load at boot.
- At the start of a playback the picture stays black until the EL comes steadily (0.3–4 s, at most
  10 s): the base layer's first frames come before the EL decoder's pictures, and a FEL frame without
  its EL would show green.
- After a seek a few frames (up to about a second, until the EL's next key picture) show without the
  EL. During a pause the EL decoder logs a few "wait fence"
  timeouts.
- No demo mode (see above). 3D MVC on the S928X only: the mvc3d module does not load on the S905X4;
  with a 3D display the installer may ask for root once and then report a failure, MVC plays in 2D.

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

AUDIO BITSTREAM (PASSTHROUGH)
-----------------------------
- To switch it on: Settings → Audio → "Digital audio output (passthrough)". Off by default: VLC then
  decodes and the audio goes out as PCM.
- When on, VLC FEL sends the audio undecoded, packed as IEC 61937, over HDMI, and the AV receiver
  decodes it. It does so for the formats both the HDMI output and the connected device (EDID) accept;
  the rest (e.g. AAC) stays PCM. No root needed.
- Dolby TrueHD (Atmos): MAT, 8 channels, 192 kHz (HBR). TrueHD is detected from the Amlogic HDMI
  output's EDID data ("MAT"), as Android does not list it among the HDMI device's encodings.
- DTS-HD MA / DTS:X: 8 channels, 192 kHz (HBR); stock VLC 3 only passed the DTS core. A DTS track
  starts as a 48 kHz core for a moment, then switches to HD.
- E-AC-3 (Atmos: JOC): 192 kHz; AC-3 and DTS: 48 kHz.
- The audio is in sync with the picture as a bitstream too (stock VLC 3 played pass-through audio
  ahead by the amount it buffered). If you still see an offset, use the player's audio delay.
- A track change gives ~1.5 s of silence while the output restarts (with PCM too); the AV receiver
  resyncs then.
- Tested on a Dune HD Pro 8K Plus (R22): TrueHD, DTS-HD MA, E-AC-3, AC-3, DTS; the output format
  (e.g. TrueHD: MAT, 8 channels, 192 kHz) checked in the box's HDMI driver, track changes and seeks
  fine. Please report what your AV receiver's display shows.

INSTALLATION
------------
1. Install the APK (USB stick + file manager, the "Downloader" app, or adb install VLC-FEL-*.apk).
2. When it plays the first FEL file, VLC FEL installs the kernel module (dvfel.ko, 2026.10.29.2; on
   the S905X4 2026.10.29.1) into
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
- 4K 24p FEL plays in real time; 4K 50/60p FEL is too heavy on the S928X (not tested on the S905X4).
- On the S928X the Dune HD r25 firmware brings the module itself: VLC FEL uses the firmware's module
  there and installs nothing. (r25 for the S905X4 brings none; VLC FEL installs it there.)
- After a seek, 1–2 frames may be shown with the RPU mapping only (no EL), and the first frames may
  be a vsync late while the chain fills.
- FEL composition can be switched off: Settings → Advanced → Custom libVLC options:
  --no-mediacodec-dovi-fel

CHANGES
-------
0.2.15-poc (libdvfelplay 2026.10.29.2, dvfel 2026.10.29.2 for the S928X; the S905X4 module, the VLC
          and app patches and 3D unchanged) – S928X: FEL is no longer lost for a whole playback after a
          quick change: when a FEL playback followed another within a second or two (a Blu-ray disc
          handed over from the Dune HD menu player to another player, or a quick restart), the module
          waited for its queues to empty before it reallocated the former compositor's buffers, while
          every frame passed through into those queues (the 125 ms lead keeps 1–3 there): they never
          emptied, and the whole film played without the EL (and without the demo mode). Now only the
          queued frames of its own buffers count. The demo mode no longer shows for a second at a start
          when the previous playback ended with it on. On Dune HD r25 the firmware's module is used: the
          Dune HD plugin 2026.10.29.2-0.2.15 updates it.
0.2.14-poc (libdvfelplay 2026.10.29.1, dvfel 2026.10.29.1 for the S905X4; the S928X module, the VLC
          and app patches and 3D unchanged) – S905X4: no green picture at the start of a FEL playback,
          also after the first EL picture: 0.2.13 unmuted the video at the first frame paired with its
          EL, but the EL decoder then pauses once more (its resolution change), and 1–2 s still showed
          green (reported by Dune HD with fel_test.mp4). The module now keeps the video muted until 6
          frames in a row had their EL, and mutes it again if one is missing within the first 48 (at
          most 10 s in all). Seeks and pause unchanged.
0.2.13-poc (libdvfelplay 2026.10.28.4, dvfel 2026.10.28.4 for the S905X4; the S928X module, the VLC
          and app patches and 3D unchanged) – S905X4: no green picture at the start of a FEL playback:
          the base layer's first frames come 0.3–4 s before the EL decoder's first picture, and the Dolby
          Vision core showed them without their EL as a green picture. The module now mutes the video
          from the start of a playback until its first frame paired with its EL (at most 8 s); the end
          of a playback (or unloading the module) always unmutes. Seeks and pause unchanged. Also with
          HDR10, SDR and Dolby Vision output.
0.2.12-poc (libdvfelplay 2026.10.28.3, dvfel 2026.10.28.3 for the S905X4; the S928X module, the VLC
          and app patches and 3D unchanged) – S905X4: FEL also with the stock Dune firmware (R24, r25):
          the module applies the four FEL fixes the firmware's Dolby Vision driver lacks at run time.
          Without them the EL decoder ran out of buffers after a few pictures (the EL was missing, the
          picture black, green or distorted). Earlier versions were tested on a player that ran our own
          patched driver, so it did not show there. Tested on r25.
0.2.11-poc (libdvfelplay 2026.10.28.2, dvfel 2026.10.28.2 for the S905X4; the S928X module, the VLC
          and app patches and 3D unchanged) – S905X4: no green picture / missing EL after the player was
          stopped by force or crashed: the release of the EL pictures on display was signalled late, and
          the Android media codec service kept 9 EL buffers (~28 MB) per such stop. After a few of them
          the codec memory ran out and FEL files played with a green picture (an empty EL) or without the
          EL, until a reboot. The release is now signalled at the end of the playback at once. Each
          playback writes a summary line to the kernel log (dvfel: session N: ...), libdvfelplay a line
          every 5 s, for reports.
0.2.10-poc (libdvfelplay 2026.10.25.2, dvfel 2026.10.23.3 and for the S905X4 2026.10.25.4, VLC and
          app fixes; 3D unchanged) – Amlogic S905X4 Dune HD players: P7 FEL composed by the SoC's
          Dolby Vision core, the EL decoded by VLC FEL and handed to the driver by the new S905X4 module
          (see above); the demo menu item says "not available" there. On the S928X the bundled module
          is the Dune HD plugin's 2026.10.23.3 (its fixes are for the firmware's own player, VLC FEL
          plays as before); on the r25 firmware the firmware's module is used, nothing is installed
          (the firmware unloads other module versions at every boot, VLC FEL would otherwise ask for
          root at every start). Dual PID m2ts: no FEL drop outs (fix by a1d4r): the ts demux now and
          then let a base layer picture go on before its EL, which then played without EL and RPU.
0.2.9-poc (app fix; modules, libdvfelplay and VLC patches unchanged) – the files of USB / internal
          drives show (before: their folders only). The player's media index leaves the drives out, and
          on Android 11 an app without "All files access" sees only indexed files there; the firmware
          has no page to allow it. VLC FEL now grants it to itself with root at its start: with su, or
          if the root access is off, it is switched on for a moment (~0.7 s) and restored afterwards
          (after a crash in between, at the next start).
0.2.8-poc (VLC fixes, modules and libdvfelplay unchanged) – audio as a bitstream (HDMI passthrough):
          TrueHD (MAT) and DTS-HD MA on 8 channel 192 kHz HBR (TrueHD was not detected before, of
          DTS-HD only the core went through), E-AC-3, AC-3, DTS; pass-through audio in sync with the
          picture; no fallback to PCM after a track change (Android keeps an output just released for
          ~1 s, it is now retried).
0.2.7-poc (libdvfelplay 2026.10.16.1, modules and VLC patches unchanged) – no stutter in the FEL
          demo mode: its level 1 measurement was read back from the GPU with a wait every frame, so on
          a heavy 4K 24p film 30–39 of 100 frames missed their vsync. Now every 6th frame is measured
          and read back without waiting (the readout changes 4 times a second anyway).
0.2.6-poc (dvfel 2026.10.15.1, libdvfelplay 2026.10.15.3) – no kernel panic after quick stop / start /
          seek: at a playback start dvfel could hand the video composer its own copy of a frame; it now
          gives back every frame exactly once. The boot script also turns off the Amlogic H.265
          decoder's back-end fast check, which could panic the kernel too. No flicker with single track
          files whose pictures partly lack the EL and the RPU (muxed from a dual layer source with
          another EL picture order). The update replaces the module too.
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
