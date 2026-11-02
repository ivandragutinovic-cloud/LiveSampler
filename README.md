# Live Sampler

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
- `build\LiveSampler_artefacts\Release\Standalone\Live Sampler.exe` (samostalan program)
- `build\LiveSampler_artefacts\Release\VST3\Live Sampler.vst3` (plagin). Kopiraj ga u
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
- Faza 1 uzima glavni multisempl programa. Slojevi, rasponi, filter i envelope iz
  Kronosa dolaze u fazi 2, a kombiji u fazi 3.

## 3. Editor mapiranja

Stranica **MAPIRANJE** (ili dugme MAPA kod oscilatora na stranici USER) otvara editor za instrument izabranog oscilatora.
- Grafik: tasteri levo-desno, velocity dole-gore. Klik bira zonu, prevlačenje je pomera,
  vučenje ivica menja raspon tastera ili jačine. Trouglić je root nota.
- Desno se ručno podešavaju root, raspon, velocity, štim, jačina, pan, loop i "ista visina"
  (za bubnjeve). Zona može da se isključi, duplira (za round robin ili lejer) ili obriše.
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

**USER** = zvuk od do 8 oscilatora. Svaki oscilator ima instrument iz biblioteke, jačinu,
pan, transpoziciju, fino štimovanje, raspon tastera i velocity-ja, i pripada **Zvuku A ili B**
(kao OSC1/OSC2 na Kronosu). Dugme MAPA otvara editor mapiranja za taj instrument.

Editor zvuka (dole na USER stranici), posebno za Zvuk A i Zvuk B:
- **PITCH**: pitch envelope sa intenzitetom, vibrato (brzina, dubina, kašnjenje),
  mod točkić i aftertouch na vibrato.
- **FILTER**: tip (Lowpass 24/12 dB, Highpass, Bandpass), cutoff, rezonanca, key tracking,
  velocity na cutoff, filter envelope sa intenzitetom, LFO na filter, mod točkić i aftertouch.
- **AMP**: amp envelope, velocity osetljivost i kriva, aftertouch na jačinu.
- Envelope je kao na Kronosu: Start, Attack (vreme i nivo), Decay, Break nivo, Slope,
  Sustain, Release (i Release nivo za pitch i filter). Grafik desno pokazuje oblik.

Zajedničko: **Poly / Mono / Mono legato** (legato ne pokreće notu ispočetka),
prioritet note (poslednja, najviša, najniža), **portamento** (vreme, "samo vezano" /
fingered, konstantna brzina) i pitch bend posebno gore i dole. Insert efekat je na EFEKAT.
Useri se čuvaju automatski; useri iz prethodne verzije se prenose sami.

**PERFORMANS** = 8 delova. Svaki deo: user, uključen/isključen, jačina, pan, transpozicija,
oktava, raspon tastera (split), velocity raspon, MIDI kanal, reverb/delay send, terca i
sustain pedala. Traka ispod pokazuje gde je koji deo na klavijaturi. Performans pamti i
master efekte (stranica EFEKTI).
- Mesta za performanse: 4 banke (A–D) × 16. Uključi SNIMI pa klikni mesto da sačuvaš.
- MIDI Program Change 0–63 poziva performanse. Instrumenti iz cele banke se učitavaju unapred.

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
