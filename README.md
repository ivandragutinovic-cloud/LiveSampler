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

## 3. Editor mapiranja

Dugme **MAPIRANJE** gore otvara editor za instrument izabranog dela (Gornji 1/2, Donji).
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

## 4. Sviranje

- **Gornji 1 / Gornji 2**: sviraju zajedno (lejer). Drugi se uključuje dugmetom sa imenom.
- **SPLIT**: levo od "Tačke" svira Donji, desno Gornji. "Donji kanal" može da pošalje Donji
  na poseban MIDI kanal (druga klavijatura); tada split nije potreban.
- **< >** pored instrumenta: brzo prebacivanje na prethodni/sledeći instrument.
- Dupli klik na potenciometar vraća ga na početnu vrednost.
- **MONO + Portamento**: jedna nota u isto vreme, klizanje između nota. Portamento radi i u
  poli režimu, od poslednje odsvirane note.
- **Insert efekat** (po delu): Chorus, Phaser, Tremolo, Overdrive, Eho. Brzina/Dubina/Mix.
- **Reverb / Delay** na delu su "send": koliko tog dela ide u master reverb i delay.
- **Master**: reverb, delay, EQ (bas/sredina/visoki), kompresor (Odnos 1 = isključen),
  jačina. Na kraju je uvek limiter da ne krcne u zvučnicima.
- **Registri**: uključi **SNIMI**, pa klikni broj da sačuvaš trenutno stanje (instrumenti +
  sva podešavanja delova). Klik na broj ih vraća. 4 banke (A–D) × 8 = 32 registra.
  **MIDI Program Change 0–31** sa klavijature poziva registre.
  Instrumenti iz svih registara se učitavaju unapred, da prebacivanje bude trenutno.
- **Sustain pedala** (CC64) i **pitch bend** rade na svim delovima.
- **PANIK** odmah utišava sve.

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
- `Source/Registers.*`: registri (čuvaju se u `%APPDATA%\LiveSampler\registers.xml`)
- `Source/PluginProcessor.*`: parametri, MIDI rutiranje, split
- `Source/PluginEditor.*`: interfejs
