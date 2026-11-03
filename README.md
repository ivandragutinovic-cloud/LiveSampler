# Pikaso Workstation

Sempler za sviranje uživo na PC-ju: tri dela (Gornji 1, Gornji 2, Donji), split i lejer,
mono i portamento, velocity, filter, ADSR, insert efekti po delu, master efekti i 32 registra.

## 1. Kako da dobiješ .exe

### Opcija A: na svom računaru
1. Instaliraj **Visual Studio 2022 Community** (besplatan). U instalaciji štikliraj
   **"Desktop development with C++"**. Uz njega dolazi i CMake.
2. Instaliraj **Git for Windows** (git-scm.com). CMake ga koristi da sam skine JUCE.
3. Raspakuj ovaj folder, npr. u `C:\LiveSampler`.
4. Otvori **"x64 Native Tools Command Prompt for VS 2022"** (iz Start menija) i ukucaj:

```
cd C:\LiveSampler
cmake -B build -A x64
cmake --build build --config Release
```

Prvi put traje 5–15 minuta jer skida i kompajlira JUCE.

Gotov program je ovde:
- `build\LiveSampler_artefacts\Release\Standalone\Pikaso Workstation.exe` (samostalan program)
- `build\LiveSampler_artefacts\Release\VST3\Pikaso Workstation.vst3` (plagin). Kopiraj ga u
  `C:\Program Files\Common Files\VST3` ako hoćeš da ga koristiš u Cantabile-u, Reaper-u i sl.

### Opcija B: bez instaliranja ičega (GitHub)
1. Napravi nalog na github.com i novi repozitorijum.
2. Prebaci sve fajlove iz ovog foldera u repozitorijum (i skriveni folder `.github`).
3. Otvori karticu **Actions**. Build se pokreće sam i traje oko 15 minuta.
4. Kad se završi, klikni na build i skini **LiveSampler-Windows** (zip sa .exe i VST3).

## 2. Kako da organizuješ semplove

### WAV folderi

Izabereš jedan glavni folder. **Svaki podfolder sa WAV fajlovima je jedan instrument.**
Podfolderi mogu da se grupišu, pa se u meniju vide kao podmeniji:

```
Semplovi\
  Harmonike\
    Weltmeister Musette\
      Musette_C2_v1.wav
      Musette_C2_v2.wav
      Musette_E2_v1.wav
      ...
  Bas\
    Fretless\
      Fretless 036.wav
      Fretless 040.wav
```

Program iz **naziva fajla** prepoznaje:
- **notu**: `C3`, `C#3`, `Db3`, `F#-1`, `Bb2` ili MIDI broj `060`
- **velocity sloj** (nije obavezno): `v1`, `v2`, `vel3`, `layer2` ili `pp`, `p`, `mp`, `mf`, `f`, `ff`.
  Ako su brojevi veći (npr. `v40`, `v80`, `v127`), koriste se kao gornja granica jačine udarca.
- Više fajlova sa istom notom i istim slojem sviraju naizmenično (round robin).

Ako naziv nema notu, program je traži u WAV fajlu (smpl blok). Ako u folderu postoji samo
jedan fajl bez note, svira se na celoj klavijaturi kao C3.

**Loop**: ako WAV ima loop tačke (smpl blok, kao iz Edison-a, Wavosaur-a, Endless WAV-a),
nota traje dok držiš taster. Bez loop-a sempl se odsvira do kraja.

**C3 = MIDI 60**: kvačica gore desno. Uključeno je kao kod Yamahe i Korga (C3 je srednje C).
Ako ti sve svira oktavu više ili niže, promeni je.

Tišina na početku fajla se automatski preskače.

### Korg KMP/KSF
Svaki `.kmp` fajl je jedan instrument. KSF semplovi treba da budu u folderu istog imena
kao KMP (npr. `STRINGS.KMP` + folder `STRINGS`) ili pored KMP-a. Mapiranje (rasponi,
root note, štim, jačina, pan) čita se iz KMP-a, a loop iz KSF-a.
Iz KSF-a se čitaju Start, Loop Start, End i da li je loop uključen (Loop Off).
Nije podržano: kompresovani KSF i semplovi iz ROM-a klavijature ("INTERNAL").

### Kontakt NKI
Svaki `.nki` fajl je jedan instrument. Semplovi (WAV/AIFF) se traže tamo gde ih je Kontakt
sačuvao: u folderu `IME Samples` pored `IME.nki`, u podfolderima ili u folderu `Samples`.
- Kontakt 2/3 NKI (npr. sačuvani iz Kontakt-a sa "Samples" folderom, ili konvertovani iz
  Korg-a): zone, rasponi, root, velocity, štim, jačina, pan, početak sempla i loop čitaju se
  tačno iz fajla.
- Release zone (sviraju kad se pusti taster) se za sada preskaču.
- Noviji NKI (Kontakt 4 i dalje) su zaključan format: program uzme semplove iz `IME Samples`
  foldera i mapira ih po nazivu ili po root noti zapisanoj u WAV-u. **Proveri mapu u editoru.**
- Monolitni NKI (semplovi upakovani u sam NKI) i NCW semplovi nisu podržani.

### Korg Kronos (PCG + KSC)
Stranica **KRONOS** → "Učitaj Kronos PCG". Pored PCG-a treba da budu KSC fajl(ovi) i folder
sa semplovima, onako kako ih Kronos sačuva (npr. `Pikaso stage.PCG`, `Pikaso stage.KSC`,
`Pikaso stage_UserBank.KSC` i folder `Pikaso stage`). Folder se sam dodaje u biblioteku.
- Multisemplovi iz KSC-a dobijaju broj i ime kao na Kronosu (npr. `MS0000 Kabina pr`).
- Tabela pokazuje programe koji koriste tvoje semplove; fabrički ROM zvukovi se preskaču.
- Dupli klik na program: napravi usera i stavi ga na PART 1, da odmah sviraš.
- Iz programa se prenose: velocity zone (MS1–MS8, sa crossfade-om), OSC1 i
  OSC2 (kod Double programa), raspon tastera, transpozicija, štim, jačina zone i programa, pan,
  filter (cutoff, rezonanca, 12/24 dB, filter EG sa intenzitetom), amp EG, LFO vibrato,
  Mono / Legato / prioritet, portamento i pitch bend.
- Vrednosti (vremena, Hz) su preračunate približno. Uporedi sa Kronosom i doteraj na USER strani.
- Još se ne prenose: pitch EG, ROM zone i efekti.
- **KOMBIJI** (dugme gore levo na KRONOS strani): dupli klik učitava kombi kao trenutni performans
  sa 16 delova. Za svaki timbre na Play: program (pravi se user ako ga nema), jačina, pan,
  transpozicija, detune, key i velocity zona, MIDI kanal (Gch = svi kanali), Send1 → reverb,
  Send2 → delay. Timbre-i sa fabričkim ROM programima i drum kit programi (još) se preskaču. MIDI kanali
  iz kombija se po defaultu ignorišu (svi delovi sviraju sa tvoje klavijature); uključi "Zadrži MIDI
  kanale" ako sviraš sa više klavijatura. Sačuvaj performans sa SNIMI.

## 3. Editor mapiranja

Stranica **MAPIRANJE** (ili dugme MAPA kod oscilatora na stranici USER) otvara editor za instrument izabranog oscilatora.
- Grafik: tasteri levo-desno, velocity dole-gore. Klik bira zonu, prevlačenje je pomera,
  vučenje ivica menja raspon tastera ili jačine. Trouglić je root nota.
- Desno se ručno podešavaju root, raspon, velocity, štim, jačina, pan, loop i "ista visina"
  (za bubnjeve). Zona može da se isključi, duplira (za round robin ili lejer) ili obriše.
- **SAMPLE / LOOP EDIT** (dugme iznad grafika): oblik talasa izabrane zone. Start (S), Loop start i
  End (E) se vuku mišem ili upisuju; Loop uključuje/isključuje loop; "Na nulu" pomera tačke na prolaz
  kroz nulu. LOOP EDIT pokazuje spoj kraja i početka loop-a. Točkić zumira, prevlačenje pomera prikaz.
- **AUTO LOOP** (u SAMPLE / LOOP EDIT): sam nađe loop u delu zvuka koji drži i podesi crossfade
  (pretapanje pred krajem loop-a, čuje se odmah, fajl se ne menja). **AUTO LOOP SVE ZONE** radi isto za
  ceo instrument; zvuci koji opadaju (klavir, gitara) ostaju bez loop-a. Podešavanja: Crossfade, Min i
  Max dužina loop-a.
- **Sve − / + oktava**: kad ceo instrument svira oktavu pogrešno.
- **Rasponi po root**: ponovo deli tastere između zona prema root notama.
- Sve promene se čuju odmah dok sviraš. **Poništi** (Ctrl+Z) vraća korak nazad.
- **Sačuvaj** upisuje mapu pored instrumenta (`LiveSampler-mapa.xml` u WAV folderu,
  ili `IME.kmp.lsmap.xml` / `IME.nki.lsmap.xml`). Originalni fajlovi se ne menjaju.
- **Odbaci izmene** vraća poslednju sačuvanu mapu, **Automatsko** briše sačuvanu mapu i
  pravi je ponovo iz fajla.

## 4. User, Performans i Terce

**Podešavanja**: dodaj jedan ili više foldera sa semplovima (disk, USB stick). Kad ubaciš
USB, klikni "Osveži". Instrumenti dobijaju ime po folderu, npr. `USB/Gudaci/Violina`.

**USER** = zvuk od do **8 OSC-ova** (Kronos koristi OSC1 i OSC2), a u svakom do 8 multisemplova (MS1–MS8).
Svaki multisempl ima instrument iz biblioteke, jačinu, pan, transpoziciju, fino štimovanje,
raspon tastera i velocity-ja (sa crossfade-om). Dugmad OSC 1 / OSC 2 (dole) biraju koji se OSC
prikazuje: gore se vide njegovi MS1–MS8, a dole njegov zvuk. Dugme MAPA otvara editor mapiranja za taj instrument.

Editor zvuka (dole na USER stranici), posebno za OSC1 i OSC2:
- **PITCH**: pitch envelope sa intenzitetom, vibrato (brzina, dubina, kašnjenje),
  mod točkić i aftertouch na vibrato.
- **FILTER**: tip (Lowpass 24/12 dB, Highpass, Bandpass), cutoff, rezonanca, key tracking,
  velocity na cutoff, filter envelope sa intenzitetom, LFO na filter, mod točkić i aftertouch.
- **AMP**: amp envelope, velocity osetljivost i kriva, aftertouch na jačinu.
- Envelope je kao na Kronosu: Start, Attack (vreme i nivo), Decay, Break nivo, Slope,
  Sustain, Release (i Release nivo za pitch i filter). Grafik desno pokazuje oblik.

Svaki MS ima i: **KO** (key off: svira kad pustiš taster) i **EQ** (bas, sredina sa frekvencijom, visoki,
samo za taj MS). Strelica pored MS-a: kopiraj / nalepi / obriši MS. Strelica pored OSC dugmadi: kopiraj /
nalepi / obriši ceo OSC (zvuk + 8 MS), i između različitih usera.

**MONO / PRELAZ** (za izabrani OSC):
- **Mono opseg**: kad je user Mono / Mono legato, samo tasteri u ovom opsegu sviraju mono (legato, portamento);
  ostali sviraju poly i bez portamenta.
- **Prelaz** (kao žica preko pragova): uključi PRELAZ na više OSC-ova. OSC bez izabranih intervala svira prvi
  ton. Dok držiš ton i odsviraš sledeći, OSC čiji interval (1–24 polustepena, gore/dole) je pogođen se pali,
  a prethodni se gasi.
- Terce na mono useru su i same mono / legato / portamento (svaki glas terce ima svoju mono liniju).

Zajedničko: **Poly / Mono / Mono legato** (legato ne pokreće notu ispočetka),
prioritet note (poslednja, najviša, najniža), **portamento** (vreme, "samo vezano" /
fingered, konstantna brzina) i pitch bend posebno gore i dole. Insert efekat i EQ usera (bas, sredina sa frekvencijom, visoki) su na EFEKAT / EQ. EQ ide sa
userom u svaki performans, a u performansu se dodatno doteruje dugmetom EQ na delu (važi samo za taj performans).
Useri se čuvaju automatski; useri iz prethodne verzije se prenose sami.

**PERFORMANS** = 16 delova (kao timbre-i u Kronos kombiju), prikazanih po 8: dugmad DELOVI 1-8 / 9-16. Svaki deo: user, uključen/isključen, jačina, pan, transpozicija,
oktava, raspon tastera (split), velocity raspon, MIDI kanal, reverb/delay send, terca i
sustain pedala. Traka ispod pokazuje gde je koji deo na klavijaturi. Performans pamti i
master efekte (stranica EFEKTI).
- Strelica pored PART dugmeta: kopiraj / nalepi / isprazni ceo deo (i u drugi performans).
- **LISTE**: 16 lista po 128 performansa. Klik učitava, SNIMI + klik čuva, desni klik: meni.
  MIDI: Bank Select (CC0) bira listu, Program Change 0–127 performans.
- **SET LISTA**: 16 set lista po 128 velikih dugmadi (strane po 16). EDIT + klik: stavi performans, natpis,
  boju. Traka dole pokazuje trenutnu stranu set liste za brzo prebacivanje. Uključi "Program Change bira set
  listu" da CC0 bira set listu, a PC dugme. Instrumenti sa trenutne strane se učitavaju unapred.
- **CEO EKRAN** (gore desno): na širokom ekranu PERFORMANS pokazuje svih 16 delova odjednom.

**TERCE**
- Deo koji treba da dobije terce: uključi TERCA na tom delu (stranica PERFORMANS).
- Dugme TERCA gore (ili dugme na klavijaturi): **drži ga i odsviraj akord** → terce se
  uključuju u tom tonalitetu (npr. D mol). Odsviraš drugi akord dok držiš → menja se
  tonalitet. **Kratak pritisak bez akorda** pali/gasi terce.
- **LEARN**: klikni LEARN pa pritisni dugme ili pad na klavijaturi (npr. Impulse).
  Od tada to dugme radi isto što i TERCA dugme.
- Stranica TERCE:
  - **Skale**: za svaki od 12 tonova (od tonike) koliko polutonova gore i dole ide terca.
    Dur i Mol (harmonski) su podrazumevani, a možeš da dodaš svoje skale.
  - **Pravila**: koji akord ili koja dva tona uključuju koju skalu, i pomeraj tonike
    (npr. E7 sa +5 uključuje A mol). Pravila se proveravaju redom.
  - Terca iznad / ispod / oba, jačina terce i "samo na najviši ton" (melodija).

## 5. Podešavanje zvuka (bitno za svirku uživo)

U samostalnom programu klikni **Options → Audio/MIDI Settings** (gore levo):
- **Audio device type: ASIO** ako tvoja zvučna kartica ima ASIO drajver.
  Ako nema, probaj **Windows Audio (Exclusive Mode)**.
- **Buffer size**: 128 ili 256 samplova. Manji broj znači manje kašnjenja, ali ako čuješ
  pucketanje, povećaj ga.
- Izaberi svoju MIDI klavijaturu pod **Active MIDI inputs**.

Semplovi se drže u RAM memoriji. Status gore pokazuje koliko memorije zauzimaju.

## 6. Struktura koda

- `Source/SampleLibrary.*`: pretraga foldera, WAV/KMP/KSF/NKI učitavanje, automatsko mapiranje, fajl mape
- `Source/MappingEditor.*`: editor mapiranja
- `Source/SamplerPart.*`: glasovi, mono/portamento, velocity, filter, ADSR, insert efekti
- `Source/MasterEffects.*`: reverb, delay, EQ, kompresor, limiter
- `Source/KronosPcg.*`: čitanje Kronos PCG fajla
- `Source/Model.*`: User, Performans, Terce (podaci, čuvanje, prepoznavanje akorda)
- `Source/PluginProcessor.*`: MIDI rutiranje, delovi, terce, čuvanje u `%APPDATA%\LiveSampler`
- `Source/PluginEditor.*`, `Source/Pages.*`: interfejs

## 7. Pikaso Autosampler (poseban program)

`Pikaso Autosampler.exe` (u paketu **KronosAutosampler-Windows**) presempluje fabričke ROM / EXs
multisemplove koje koriste tvoji programi i kombiji, da bi ih Pikaso Workstation svirao umesto ROM-a.

1. **Učitaj PCG** i izaberi programe ili kombije (Kombiji) → **Dodaj izabrane**. Desno se vidi spisak
   ROM multisemplova koje treba snimiti (svaki samo jednom).
2. **Direktno preko SysEx-a** (preporučeno, uključeno po defaultu): na Kronosu izaberi Init Program
   (npr. USER-FF 006), izaberi MIDI ulaz i izlaz Kronosa i klikni **Pročitaj šablon sa Kronosa**.
   Autosampler zatim za svaki multisempl menja samo trenutni program na Kronosu (edit buffer) i
   ništa ne snima u memoriju Kronosa. Korak sa PCG-om tada ne treba.

   Ili preko PCG-a: izaberi **šablon** (inicijalizovan program sa Kronosa, npr. "Init"), **banku** i **od broja**, pa
   **Napravi PCG**. Nastaje `<ime>_AUTOSAMPLER.PCG` sa po jednim programom za svaki multisempl
   (OSC1 MS1 = taj multisempl, otvoren filter, bez efekata, bez velocity uticaja). Na Kronosu ga
   učitaj samo u tu banku.
3. **Audio ulaz**: Kronos USB audio. **MIDI izlaz**: Kronos. Podesi **Bank CC0 / CC32** tako da
   **Test** na Kronosu izabere prvi novi program. Zatim **START**.
   - **Auto loop**: za zvuke koji drže sam nađe loop, upiše crossfade u sempl i loop u WAV.
   - Snima se svaki treći ton (podesivo) punom jačinom; u Pikaso Workstation svaki sempl pokriva i polustepen
     ispod i iznad.
   - Postojeći snimci se preskaču, pa snimanje može da se prekine i nastavi.
   - Semplovi idu u `C:\KRONOS\KRONOS ROM\...` sa fajlom `kronos-ms.txt`. U Pikaso Workstation klikni
     Osveži; Kronos programi i kombiji onda koriste ove semplove umesto ROM-a.

### EXi programi (AL-1, CX-3, STR-1, MS-20EX, PolysixEX, MOD-7, SGX-2, EP-1)
EXi sinteze nemaju multisemplove, pa Pikaso Autosampler snima ceo program: salje ga u edit buffer
preko SysEx-a (nista se ne snima na Kronos), iskljucuje IFX/MFX/TFX (ili ostavlja IFX po izboru),
i snima svaki treci ton u vise jacina (Auto: klavir 6, EP 5, STR-1 4, orgulje 1, sint 2).
Meri i release posle pustanja tastera. Pikaso Workstation od toga sam pravi usera.

### Kategorije performansa
Kategorije su iste kao Kronos kategorije u Live Kontroleru (Keys, Acc, Winds, Sax, Brass, Strings, Pluck,
Guitar, Synth, Lead, Bass, Pesme, Main), sa istim bojama. Pikaso Workstation prati
Desktop\Live Kontroler\podesavanja-server.json: kad u Live Kontroleru promenis boju, ime ili kombi
u kategoriji, za par sekundi se promeni i ovde. Performans napravljen od Kronos kombija dobija
kategoriju tog kombija. Bez kategorije: narandzast okvir i tacka na dugmetu liste.
