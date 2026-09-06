# spfy

Native C reimplementation of the SpeechWorks **Speechify 3.0.5** (2003) TTS
engine. The goal, byte-exact 1:1 output with the original Windows engine on a
fixed audit corpus, is achieved for every voice and language audited here, on
Windows, Linux, macOS, ARM and WASM from one source tree.

```text
==============================================================================
PER-VOICE SUMMARY
voice      lang       n     slot      uid     emit   byte-identical
------------------------------------------------------------------------------
aicraig    en-US    221 10168/10168 10016/10016  221/221          221/221
aimara     en-US    221 10168/10168 10016/10016  221/221          221/221
aimara2    en-US    221 10168/10168 10016/10016  221/221          221/221
jill       en-US    221 10168/10168 10076/10076  221/221          221/221
tom        en-US    221 10168/10168 10016/10016  221/221          221/221
javier     es-MX    100 6992/6992 6946/6946  100/100          100/100
paulina    es-MX    100 6992/6992 6932/6932  100/100          100/100
felix      fr-CA    100 5578/5578 5578/5578  100/100          100/100
==============================================================================
```

1405 phrases byte-identical, 70402/70402 slots, 69596/69596 path uids, and the
gate exits 0 with no mismatch categories on any voice. Selection is audited
against Frida traces captured from the original engine; `BYTE-IDENTICAL`
compares the rendered samples.

The build is a 32-bit C99 core that loads SpeechWorks's original voice data
(VIN/VDB/VCF) plus an in-process host for the front-end DLL, one per language
(`SWIttsFe-en-US.dll`, `-es-MX`, `-fr-CA`; the shipped set also has de-DE,
en-AU, en-GB, fr-FR, ja-JP and pt-BR, which are untested here). Every platform
runs that DLL through the `src/host_emu` x86 interpreter, which is why the same
source produces byte-identical WAVs on arm64 and x86_64 alike.

The repository also carries voices built by this project rather than by
SpeechWorks, currently `en-US/crsmara` and `en-US/crstom`. They are not part of
the table above and cannot be: there is no original rendering of them to compare
against. They are held to the container's own structural checks instead.

---

## What ships

| Surface | Status |
|---|---|
| CLI synth (`spfy_synth`, text to WAV) | yes, 100% audit on all 8 audited voices |
| en-US / es-MX / fr-CA voices | yes, byte-exact on each |
| Windows SAPI 5 voice DLL (32-bit and 64-bit) | yes |
| Desktop GUI (`spfy_gui`) | yes, every platform except static musl |
| SSML `<prosody>` (rate / pitch / volume) | yes |
| SSML `<phoneme>` / SAPI XML `<pron sym>` | yes |
| Inline phoneme escape (`\![.1pa.0tx]`) | yes |
| Word / sentence / bookmark events | yes |
| Linux / macOS / ARM builds, bit-exact to Windows | yes |
| Speechify 4 mode (`--s4`) | yes, opt-in, see below |
| 8 kHz and 16 kHz voice databases | yes, see below |

Tested SAPI consumers: Balabolka, Windows Narrator.

### Release artifacts

GitHub Actions builds ten artifacts on every push to `main` and attaches them to
a date-versioned release:

* Two Windows installers: `spfy-setup-<version>.exe` (x64) and
  `spfy-setup-<version>-x86.exe` (32-bit, down to Windows 7 SP1).
* Eight unix tarballs: `linux-x86`, `linux-x86_64`, `linux-armv7`,
  `linux-arm64`, `linux-x86_64-musl`, `linux-arm64-musl`, `macos-arm64`,
  `macos-x86_64`.

End users who only want the SAPI voice can take an installer from the
[Releases page](https://github.com/wagwan-piffting-blud/Speechify/releases) and
skip the build entirely.

The two musl tarballs are statically linked: no interpreter, no glibc floor, so
they run on Alpine and in `FROM scratch` containers. That is also why they are
the only two artifacts without a GUI, since WebKitGTK cannot be statically
linked.

### The GUI

Every desktop artifact except those two carries `spfy_gui`: pick a voice, type
or paste text, hear it, save the WAV, and install more voices without visiting
GitHub. It drives `spfy_synth` as a subprocess, so its audio is byte-identical
to the CLI's by construction. It is a webview app specifically so screen readers
see real controls. Details in [`spfy/gui/README.md`](spfy/gui/README.md).

How you start it depends on the platform, and each gets its native equivalent
rather than a bare binary:

* **Windows**, both installers. Start Menu entry, an optional desktop icon, a
  finish-page launch box, and an `App Paths` registration so Win+R and Start
  menu search find `spfy_gui` too.
* **macOS**. `Speechify.app` in the tarball. Drag it to Applications and it is
  in Launchpad and Spotlight. Self-contained, so it still works once moved out
  of the extracted folder.
* **Linux**. `bin/spfy_gui`, plus `./install-desktop.sh` to add it to this
  user's applications menu; `--uninstall` removes it. The `.desktop` file is
  written at run time because the tarball is relocatable and `Exec=` has to be
  an absolute path.

### Voices

Tom ships with every desktop build, so a fresh install speaks immediately,
offline, with no second step. On Windows that also means `regsvr32` finds a
voice during install and registers a working SAPI token, so Narrator, NVDA and
Balabolka see Speechify straight away rather than after a manual drop-and-refresh.

Every other voice is published as its own release asset, one zip per voice plus
one per language, on the rolling
[`voices`](https://github.com/wagwan-piffting-blud/Speechify/releases/tag/voices)
tag. Each zip already contains the `<lang>/<voice>/` folders, so unzipping into
`%USERPROFILE%\Documents\Speechify\` puts every file where the SAPI scan looks;
then run "Refresh SAPI Voices". Or let the CLI do it, below.

---

## Getting voices without leaving the terminal

```text
spfy_synth --list-available            what the release offers, and what you have
spfy_synth --list-available --json     the same, machine-readable
spfy_synth --install-voice crsmara     download, verify, unpack, done
```

`--install-voice` fetches the voice's zip from the rolling `voices` release,
checks its SHA-256 **before** unpacking, and lays it out under
`~/Documents/Speechify/<lang>/<voice>/`, exactly where `--list-voices` already
looks, so the next command can use it. `$SPFY_VOICE_DIR` overrides the
destination the same way it overrides the search.

The zip is deleted afterwards, and a failed or interrupted download leaves
nothing behind: the transfer writes `<name>.zip.part` and renames only on
success.

Verification happens before unpacking rather than after, because a truncated
archive unpacked first leaves a partial voice directory that looks complete
enough to load, and the failure then surfaces from inside the engine rather
than from the download that caused it.

This reads the same `voices.json` catalog `installer/updates/pack_voices.py`
publishes, not a second list. `--list-voices` stays offline and answers "what
can I use right now"; `--list-available` needs the network and answers "what
else is there". The GUI's "Get voices..." button drives these two commands as a
subprocess rather than reimplementing any of it, so the catalog fetch, the
checksum and the unzip have one implementation.

Both flags need a build with update support (`SPFY_HAVE_UPDATE_CHECK`); without
it they say so and exit 2. Unzipping uses vendored miniz. See
[`spfy/third_party/miniz/README.md`](spfy/third_party/miniz/README.md) for why
it is there and what was rejected.

---

## Updates

spfy tells you when a newer engine, or a rebuilt version of a voice you have
installed, is available. It checks at most once a week, and only when you
actually use it.

```text
spfy_synth --check-update      check right now and print the answer
spfy_synth --no-update-check   skip the automatic check for this run
spfy_update --status           what it knows: interval, last check, dismissals
spfy_update --disable          turn the automatic check off for this user
spfy_update --dismiss          stop reporting what is currently available
SPFY_NO_UPDATE_CHECK=1         off everywhere, including inside the SAPI voice
```

Uncheck "Check for engine and voice updates" during setup and the installer
writes `{app}\no_update_check`, which switches it off for every account on the
machine. Delete that file to switch it back on.

What it does, and what it deliberately does not:

* **Voices are compared by content, not by version string.** The manifest
  carries the size and SHA-256 of each voice file; the size is checked first
  (free) and the hash only when the sizes match, which is the rare case. A
  computed hash is cached against the file's size and mtime, so a 96 MB VDB is
  read once per rebuild, not once per check. Measured on CRS Tom: 0.57 s cold
  across its 153 MB of voice files, 0.019 s warm.
* **The SAPI DLL never checks anything itself.** It runs in-process inside
  Narrator and Balabolka, so on the first `Speak` it stats one small file and,
  at most once per process, starts a detached `spfy_update.exe`. No fetch, no
  hashing and no window ever happens on the screen reader's thread; the result
  arrives as a tray balloon, which takes no focus.
* **A `dev-` build is never told about an engine release.** A working copy is by
  definition ahead of the newest release, so comparing them can only produce a
  wrong answer. `spfy_synth --version` prints which kind of build you have.
* **Failure is silent.** No network, a 404, a truncated body or a manifest with
  a schema this build does not understand all end with nothing printed. The
  timestamp still moves, so an offline machine pays one failed connect a week
  rather than one on every synth.
* Nothing is downloaded or installed for you by the check. The notification is
  a URL. Downloading is the separate, explicit `--install-voice` above.
* **Only builds that ask for it have any of this in them.** The check lives
  behind `SPFY_HAVE_UPDATE_CHECK`, set by the `spfy_synth` and
  `spfy_synth_trace` targets alone. The WASM and Android builds compile the same
  `src/cli/spfy_synth.c` from CMakeLists of their own and get exactly the file
  as it was before the feature existed: no version header, no network code, no
  new symbols. `build32.bat`, `build.bat`, `build_emu.bat`, `build_hosted.bat`,
  `build_linux.sh` and `build_macos.sh` all go through `spfy/CMakeLists.txt` and
  pick it up automatically, stamped `dev-<sha>`.
* A 64-bit SAPI client (Narrator on x64) reaches the engine through
  `spfy_sapi64.dll`, which renders by spawning `spfy_synth.exe`. That copy has
  no console window, so it hands the check to the detached helper rather than
  printing into a pipe nobody reads, and returns immediately, because the shim
  is waiting on it to produce audio.

The manifest lives at
`https://github.com/wagwan-piffting-blud/Speechify/releases/download/updates/update.json`,
a rolling tag, so the URL baked into every binary ever shipped keeps working.
Point the checker somewhere else with `SPFY_UPDATE_URL`, including at a local
file: `spfy_update --url file:///C:/tmp/update.json --check`.

Publishing, for the maintainer: `installer/updates/make_manifest.py` writes
`update.json`, `installer/updates/pack_voices.py` builds the voice zips and the
`voices.json` catalog, and `installer/updates/verify_manifest_assets.py` checks
that every asset the manifest names is actually attached to the release.

---

## Quickstart

### Windows (msys2 mingw32)

```cmd
:: First time only - install the toolchain
pacman -S mingw-w64-i686-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja

:: Configure + build  (run from PowerShell, not cmd)
spfy\build32.bat

:: Synth a phrase
C:\tmp\spfy_build32\src\cli\spfy_synth.exe ^
  tom "The quick brown fox jumps over the lazy dog." out.wav

:: Register the SAPI voice (elevated PowerShell)
Start-Process 'C:\Windows\SysWOW64\regsvr32.exe' `
  -ArgumentList '/s','C:\tmp\spfy_build32\src\sapi\spfy_sapi.dll' `
  -Verb RunAs -Wait
```

After registration, "Speechify - tom", plus any other voice folder under
`%USERPROFILE%\Documents\Speechify\en-US\`, appears in Balabolka, Narrator and
similar.

### Linux / macOS

```sh
sudo apt install -y build-essential cmake ninja-build gcc-multilib python3

./spfy/build_linux.sh          # macOS: ./spfy/build_macos.sh

/tmp/spfy_build_linux32/src/cli/spfy_synth \
  tom "The quick brown fox jumps over the lazy dog." /tmp/out.wav
```

`gcc-multilib` is needed only for the 32-bit legs. SAPI is Windows-only and the
top-level CMakeLists gates it on `if(WIN32)`, so it is skipped automatically.

CI uses `spfy/ci/build_unix.sh` for every unix target, native and emulated
alike, so the legs cannot drift apart in flags or pass criteria. That script
also verifies: it synthesizes the reference phrase through `en-US/tom` and fails
the build unless the WAV's sha256 matches. The GUI is built separately by
`spfy/ci/build_gui_linux.sh`, which enforces the same glibc ceiling.

---

## CLI reference

`spfy_synth <voice> "<text>" <out.wav>` is the short form. The voice is a folder
name, matched case-insensitively, so `tom`, `Tom` and `TOM` all resolve to
`en-US/tom/tom.vin`, `tom8.vdb`, `tom.vcf`. A path to the voice directory
(`en-US/crstom`) is accepted too.

Naming the three files explicitly still works, and is what scripts should keep
using when they point at a voice outside the tree:

```cmd
spfy_synth.exe en-US\tom\tom.vin en-US\tom\tom8.vdb en-US\tom\tom.vcf ^
  "This is a radar indicated threat." out.wav
```

| Flag | Effect |
|---|---|
| `-f`, `--file <path>` | read input text from a file; the `"<text>"` positional is then omitted |
| `-q`, `--quiet` | suppress per-synth diagnostics (default) |
| `-s`, `--silent` | suppress everything but real errors |
| `-v`, `--verbose` | full FE and synth pipeline diagnostics |
| `-4`, `--s4` | Speechify 4 mode |
| `--no-s4` | force it off, ignoring `SPFY_4_MODE` |
| `--list-voices` | every voice the search can see, offline |
| `--list-available` | what the `voices` release offers, and what you already have |
| `--install-voice <name>` | download, verify and unpack one voice |
| `--json` | machine-readable form of the two list commands |
| `--version` | version and build kind (`dev-<sha>` or a release stamp) |
| `--check-update` | check now and print the answer |
| `--no-update-check` | skip the automatic check for this run |
| `--trace-stream` | emit the streaming trace used by the replay tools |

On Windows, pass non-ASCII input through `-f`, not as an argv string. `argv`
arrives in the ANSI code page, so a UTF-8 IPA `ph` attribute in an SSML
`<phoneme>` loses every non-ASCII byte before `main()` sees it and produces the
wrong phonemes, silently. `-f` reads UTF-8 and UTF-16 LE correctly.

### Voice search order

First hit wins:

| # | where |
|---|---|
| 1 | `$SPFY_VOICE_DIR` |
| 2 | the working directory, then its parents |
| 3 | the directory holding the binary, then its parents |
| 4 | `~/Documents/Speechify` (the installer's layout) |

A directory only counts if it contains a language folder, and a language folder
is recognised by shape (`en-US`, `es-MX`, `fr-CA`, and so on) rather than from a
list, so adding a language needs no code change. `--list-voices` prints
everything the search can see, and the same list is printed when a name does not
match.

`<voice>8.vdb` is preferred over `<voice>.vdb` whenever both exist, because a
16 kHz bank sitting beside the 8 kHz one is not an equivalent substitute.

### Voice database sample rates

SpeechWorks shipped every Speechify 3.0 voice at two rates; the User's Guide
gives both install lines side by side:

```text
rpm --install Speechify-Vox-en-US-tom-8kHz-3.0-0.i386.rpm
rpm --install Speechify-Vox-en-US-tom-16kHz-3.0-0.i386.rpm
```

They install into the same voice directory, so spfy accepts either and picks the
storage from the VDB's own `wFormatTag`:

| tag | storage | rate |
|---|---|---|
| 7 (`WAVE_FORMAT_MULAW`) | 1 byte/sample u-law | `tom8.vdb`, 8 kHz |
| 1 (`WAVE_FORMAT_PCM`) | 2 bytes/sample s16 | 16 kHz |

The tag is the only trustworthy field. `tom8.vdb` advertises `blockAlign` 2 and
`bitsPerSample` 16 while storing 1-byte u-law, so bytes/sample is derived from
the tag and never from those. Anything else is refused at load rather than
decoded into noise that still scores plausibly.

Output rate follows the VDB, and the WSOLA window follows with it: W is 80
samples at 8 kHz and 160 at 16 kHz, both 10 ms, so durations are identical in
milliseconds at either rate and only the sample counts change.

Warning: `tom16.vdb` in this repo is **not** a SpeechWorks file. It is an
AudioSR super-resolution of the 8 kHz database, so its content above 4 kHz is
generated, not recorded. It is fine for exercising the code path and worthless
as evidence about the voice.

A 9-positional legacy form also exists, taking explicit
`hpclass / vocab / fe_tables_a / fe_tables_b` paths between the voice triplet
and the text. Prefer the short form: vocab and the FE tables are embedded in the
binary, and hp_class is derived from the voice's own VIN, so the short form
works for every voice rather than just Tom.

---

## SSML and inline tags

### SSML

Works on every surface, the CLI, SAPI, the WASM build and the web demo, because
it is translated into the inline `\!` tags below before the front end ever sees
it (`spfy/src/common/ssml.c`), and that translation sits in the shared synth
path. It used to live only inside `spfy_sapi.c`, where SAPI does the XML parsing
itself; on a plain `spfy_synth` run the tags were not ignored, they were read
out loud.

WASM is the same code, not a reimplementation: `wasm/CMakeLists.txt` compiles
`src/cli/spfy_synth.c` into `spfy_synth_lib` with `SPFY_SYNTH_NO_MAIN`, and
`spfy_wasm_synth()` calls the same `spfy_synth_to_sink()` the CLI and the SAPI
shim do. The web demo passes the textarea through verbatim and plays the PCM
into `AudioContext.destination` with no gain stage, so nothing is masked
between the engine and the speaker. Verify with
`node wasm/tools/prosody_wasm_check.mjs` — it drives the shipped
`dist/spfy_wasm.wasm` and asserts **amplitude**, which the older
`ssml_wasm_check.mjs` never did: every assertion in that file is a sample
count, and volume does not change length, so a completely dead volume control
passed it.

| Element | Mapped to | Notes |
|---|---|---|
| `<speak>` | - | stripped; `xml:lang` and other attributes ignored |
| `<prosody rate>` | `\!wp` | `x-slow` to `x-fast`, `150%`, `+20%`, `1.5` |
| `<prosody pitch>` | `\!pp` | `x-low` to `x-high`, `+4st`, `+10%`. `Hz` is ignored, not guessed: an absolute target needs a base F0 that is not known this far upstream |
| `<prosody volume>` | `\!vp` | `silent` to `x-loud`, `+6dB`, `50%` |
| `<break time or strength>` | `\!pN` | `800ms`, `1.5s`; `none` to `x-strong` maps to 0 to 1000 ms; bare `<break/>` is 250 ms |
| `<emphasis level>` | `\![ToBI:...]` | `strong` to `L+H*`, `moderate` to `H*`, `reduced` to `NONE`. Re-emitted at every word in the span, because the accent tag is one-shot |
| `<phoneme alphabet ph>` | `<pron sym="...">` | `ipa` and `x-arpabet`; the alphabet is sniffed when the attribute is absent |
| `<say-as interpret-as>` | `\!tsc` / `\!tsa` / `\!ny0` | `characters`, `spell-out`, `digits`, `telephone`, `date` |
| `<sub alias>` | the alias | content dropped |
| `<p>` `<s>` | `.` | a phrase boundary whether or not the full stop was typed |
| `<voice>` | - | content kept, voice not switched: the engine is one voice per process |
| `<mark>` `<metadata>` `<desc>` `<lexicon>` | - | dropped |
| anything else | passed through | which is what keeps `<pron sym="...">` working |

XML comments, `<?xml?>`, CDATA and entities (`&amp;`, `&#233;`, `&#xE9;`) are
handled. Malformed input degrades to "strip the markup, keep the text" rather
than failing, so a half-pasted document still speaks.

`SPFY_NO_SSML=1` turns the whole pass off, for text that legitimately contains
something shaped like an SSML element. `SPFY_SSML_DUMP=1` prints the translated
text and the resulting DSP spans.

### Inline `\!` tags

Accepted in plain text on the `spfy_synth` path, alongside the FE's own escapes:

| Tag | Effect |
|---|---|
| `\![SPR ...]` | inline phoneme escape, passed to the FE untouched |
| `\!pN` | pause, passed through |
| `\!vpN` / `\!vdN` | volume, percent of the port value / of the server default, from the tagged word onward. `\!vp0` is silence. Both bases are 100 here — there is no API to set either — so the two spellings are currently equivalent |
| `\!rpN` / `\!rdN` | rate as a **selection** bias, from the tagged word onward |
| `\!wpN` / `\!wdN` | rate as a **time-scale on the rendered audio** |
| `\!ppN` / `\!pdN` | pitch, percent of base F0 (100 = unchanged); selection bias plus TD-PSOLA residual |
| `\![ToBI:...]` | one-shot pitch-accent override on the next word; the bare form `[ToBI:H*]` is also accepted |
| `\!s4m` (or `\s4m`) | Speechify 4 mode for this utterance only, and start of the utterance only |

`\!vp`, `\!rp`, `\!wp` and `\!pp` are stateful by design: they apply from where
they appear to the end of the utterance. Use `\!vpr`, `\!rpr`, `\!wpr` and
`\!ppr` to reset.

A space between the tag and its value (`\!vp 50`) and a value fused to the
next word (`\!vp50Hello`) are both accepted. Neither is the intended spelling,
but rejecting them was worse than either reading: the tag fell through to the
generic `\!`-swallow, which speaks the number in the first case and deletes the
word in the second — both of which look like "the tag does nothing".

⚠ Volume is applied as a per-unit gain **before** the WSOLA push, not at the
sink, and that has one measurable consequence. For any gain at or below 1.0 it
is invisible: scaling both arms of an unnormalised correlation cannot move its
argmax, and `\!vp50` is sample-for-sample the untagged render halved. Two cases
escape that. Above 1.0 the scaled buffer clips, and clipping *does* move the lag
search — `<prosody volume="x-loud">` comes out 64 samples longer than the same
text untagged. At exactly 0 the search runs on silence and ties, which is why
`volume="silent"` is 686 samples longer. The audio is right in both; only the
length moves.

It does **not** affect selection. USel runs to completion before a single
sample is decoded, and the gain touches decoded PCM only; the chosen path is
bit-identical at `\!vp0`, `\!vp10`, `\!vp50`, `\!vp200` and `\!vp400`, on a
dump that moves when one character of the text changes.

Pre-WSOLA is also what makes a *mid-utterance* volume change work. Every unit
boundary is a Hann crossfade that sums the previous chunk's windowed tail with
the new chunk's windowed head, so the samples at a join belong to **both**
units. Scaling each unit before the push means the join blends `g1*prev` with
`g2*next` and the gain ramps across the same ~5.6 ms window the audio does —
measured on `Hello world \!vp50 this is a test.`. A gain applied after the
push has no per-unit samples left to work on at a join: it has to pick one
value for the blended region and step at its edge.

So the placement is right for everything except the two shape-breaking cases
above. The fix for those is not to move the gain wholesale but to **split it**:
push at `min(g, 1)` — shape-preserving, exact boundaries, byte-identical to
today for every attenuation — and hand the remainder (the >1 excess, or the
mute at 0) to a sink-side span. Not done.

### `\!rp` and `\!rd`, speaking rate

`\!rpN` sets the rate to N percent of the port value, `\!rdN` to N percent of
the server default, and `\!rpr` / `\!rdr` reset to those values. Legal range is
33 to 300, per the User's Guide p.82. Duration comes out at about **100/N**:

| | `\!rp33` | `\!rp50` | `\!rp75` | `\!rp150` | `\!rp200` | `\!rp300` |
|---|---|---|---|---|---|---|
| ideal 100/N | 3.030 | 2.000 | 1.333 | 0.667 | 0.500 | 0.333 |
| vendor engine | 2.991 | 1.969 | 1.317 | 0.675 | 0.506 | 0.328 |
| spfy | 2.902 | 1.942 | 1.297 | 0.648 | 0.488 | 0.329 |

Measured on "The national weather service has issued a warning.", vendor on tom
and spfy on crstom, each relative to its own untagged render.

#### How it works, and why it is not a selection bias

The engine does **not** move the rate by picking different recordings; it
time-scales each selected unit inside the WSOLA join. `FUN_08ee2960` rewrites
the target the moment rate is armed, and the two branches differ:

```
factor = 100.0f / rate_level
pau:   target = factor * target      (the duration model's target)
else:  target = natural * factor     (the unit's OWN length)
```

So for a **non-pause** unit the duration model is not consulted at all — the
unit is stretched onto its own natural length times 100/N, which makes the
scale the constant N/100. Only **pauses** go through the duration target. Two
further details matter and both are reproduced here:

- **Plosives are never stretched.** `FUN_08ee15a0` classifies a label as a stop
  or affricate (`p b t d k g`, `ch`, `jh` — explicitly *not* `pau`, `dh`, `th`),
  and a unit that would be *lengthened* passes through at its natural length
  instead. Compression is still allowed, so the guard is one-sided. This is why
  the engine's slow speech does not smear the way a flat time-scale does.
- **Pauses always scale**, including an explicit `\!p`. `pau` is excluded from
  that guard twice over. A `\!p500` at `\!rp50` comes out 2.00x longer here
  against the vendor's 2.04x.

`\!rp` also **arms** target-matching for the whole utterance. Without any rate
tag the engine plays every unit at its natural length and never consults the
duration targets at all, so a rate tag changes the audio even when it asks for
no change: `\!rp100` and `\!rpr` both render differently from no tag, while
`\!vp100` is byte-identical. spfy matches that, which is also why the parity
gate still reads 221/221 byte-identical — nothing without a rate tag moves.

> ⚠ `\!rp` used to bias the CART duration target and hope selection delivered.
> It cannot: selection saturates near +9% in the slow direction, so `\!rp50`
> stretched by 1.08x instead of 2x. That was spfy's bug, not the tag's meaning.

#### `\!wp`, the literal escape hatch

`\!wp` is a flat WSOLA time-scale on the rendered audio, with no plosive guard
and no target matching. It takes any factor you ask for and is the right tool
when you want an exact duration rather than a natural-sounding one. It is not
an engine tag — the engine has no equivalent — so reach for `\!rp` first.

SSML `<prosody rate>` maps to `\!rp`. `SPFY_RATE` is unchanged and is still
`\!wp` applied to the whole utterance at once.

`\!pp` splits the same way without asking: it biases selection as far as the
corpus reaches (`spfy_synth_split_pitch`, +1.5 / -2.0 st on tom) and TD-PSOLA
covers the rest. Selection alone delivers +2.43 st of a requested +6 and only
+0.82 st of a requested -6; with the residual it is +5.21 and -6.28.

### The `\![ToBI:...]` accents

Every pitch accent the engine understands is reachable from the tag, and each
name means exactly what it means when the front end assigns it. The tag and the
FE share one table, so they cannot drift apart.

| Tag | Height bias | Notes |
|---|---|---|
| `\![ToBI:H*]` | 0 | the plain high accent; what the FE picks for most content words |
| `\![ToBI:L+H*]` | +2 | the highest accent available |
| `\![ToBI:!H*]` | -2 | downstepped high |
| `\![ToBI:H+!H*]` | -2 | identical in effect to `!H*` |
| `\![ToBI:L*+H]` | -3 | |
| `\![ToBI:L*]` | -5 | the lowest accent available |
| `\![ToBI:0]` or `\![ToBI:NONE]` | - | suppress the FE's accent entirely |

The bias is height, not timing. `L+H*` makes the accent taller; it does not move
where the peak sits. To move the peak later or earlier use
`SPFY_PROSODY_ALIGN_MS` (default -25; negative is earlier), but that is a global
setting, not per-word.

Boundary tones (`L-L%`, `L-H%`, `H-H%`) are not settable from this tag. They
live on a different channel, per syllable, from the FE, feeding both the f0tr
CART and the phrase-final fall rather than the per-word accent. An unrecognised
tag body is deliberately left in the text and spoken, so a typo is immediately
obvious rather than silently doing nothing.

### SAPI feature map

| Tag / state | Implementation |
|---|---|
| `<voice>` | SAPI CLSID switch, handled by the SAPI runtime |
| `<prosody rate>` | WSOLA frame-based time-stretch (post-process) |
| `<prosody pitch>` | Hybrid: corpus selection bias (about +/-1.5 to +/-2 st on Tom) plus TD-PSOLA for the residual |
| `<prosody volume>` | Per-sample scalar gain in the sink |
| `<break time>` | Zero-sample silence emit |
| `<mark name>` | `SPEI_TTS_BOOKMARK` event |
| `<phoneme alphabet ph>` | SAPI phone IDs to ARPAbet to SPR to inline FE escape |
| `<pron sym>` (SAPI XML) | Same path as `<phoneme>` |
| Word / sentence / bookmark boundaries | `SPEI_*` events with byte-accurate `ullAudioStreamOffset` |
| Host rate slider (`ISpVoice::SetRate`) | `ISpTTSEngineSite::GetRate()`, summed with `SPVSTATE.RateAdj` |
| Host volume slider (`ISpVoice::SetVolume`) | `ISpTTSEngineSite::GetVolume()`, multiplied with `SPVSTATE.Volume`. SAPI does **not** apply this for you — until the call existed the slider was inert, measured as byte-identical output at host volume 100 and 30 while `<volume level="30">` on the same harness gave exactly 0.30x |

For pitch specifically: when the requested shift fits Tom's recorded F0 range
(roughly -2 to +1.5 semitones around a 118 Hz median) it is handled entirely by
biasing the F0 target the Viterbi matches against, with no DSP. Past that the
selector hits the corpus ceiling and TD-PSOLA covers the residual.

### Hosts that split on `\!`

Balabolka does not pass tagged text through in one piece. It splits on `\!` and
issues the pieces as separate `Speak()` calls, so

```text
\!s4m \![ToBI:H*]This is a radar indicated threat.
```

reaches the engine as `"\!s4m \!"` and then `"[ToBI:H*]This is..."`, captured
with `SPFY_SAPI_DEBUG=1` in `%TEMP%\_sapi_dbg.log`. Three consequences, all
handled:

* **A tag cut in half is rejoined.** The engine keys on the split's signature, a
  Speak call ending in a dangling `\!`, drops it from that utterance and puts it
  back on the front of the next. That reconstructs the original text, so every
  tag then takes its normal path. Verified on `\![ToBI:]`, `\![SPR]`, `\!vp` and
  `\!rp`: the split form renders byte-identical to the unsplit one, and a second
  half arriving with no dangling predecessor is left alone. Keyed on the
  signature rather than on which tag it is, because which tags a host chops is
  not predictable: `\!vp` and `\![SPR]` are cut where `\!rp` is not.
* **`[ToBI:...]` is also accepted without the `\!`**, as a belt-and-braces path
  for a host that drops the prefix outright rather than splitting.
* **A tag with no words after it is held for the next utterance.** `"\!s4m \!"`
  has nothing to say, so scoping the mode to it would switch it off before the
  sentence arrived. It is spent on the next utterance that has words, then
  restored, so it still does not leak beyond that.

---

## Speechify 4 mode

Speechify 3.0.5 performs no pitch modification at all: the F0 you hear is
whatever the selected units already had. Speechify 4's intonation does not fall
out of the same data, so spfy can add a stage 3.0.5 never had, a target F0
contour applied to the chosen units by TD-PSOLA, driven by pitch marks measured
from the voice's own audio.

It is off by default and everything it touches is identity when off. See
[SPEECHIFY_4_FINDINGS.md](SPEECHIFY_4_FINDINGS.md) for what is and is not
claimed about it.

```cmd
spfy_synth.exe --s4 tom "This is a radar indicated threat." out.wav
```

`-4` is a synonym for `--s4`; `SPFY_4_MODE=1` does the same for callers that
cannot pass flags. `--no-s4` forces it off even when the variable is exported,
and beats the `\!s4m` tag too.

### It needs the voice's pitch marks

`<voice>8.pmindex` and `<voice>8.pmdata` beside the VDB. The stem is derived
from the VDB path (`tom8.vdb` gives `tom8`), or set `SPFY_PROSODY_PM` to point
elsewhere. Only Tom ships with marks, so S4 mode is Tom-only in practice.

### From a SAPI host: the `\!s4m` tag

A SAPI consumer such as Balabolka can only hand the engine text, so the switch
also exists in the text:

```text
\!s4m \![ToBI:H*]This is a radar indicated threat.
```

`\s4m` and `\!s4m` are both accepted. It works only at the very start of the
utterance; leading whitespace is fine, anything else before it is not. A tag
appearing anywhere else is left completely alone and gets spoken aloud, which is
deliberate, so a misplaced tag is immediately obvious rather than silently doing
nothing. `\s4model` is a word, not a tag.

It applies to that one utterance only. Every variable the mode writes is saved
beforehand and restored afterwards, so a tagged sentence does not change the
sentences after it, which matters in a host like Balabolka where one process
speaks many utterances and a sticky switch could not be turned off short of
restarting.

### The contour redistributes pitch rather than imposing a shape

S4 mode used to apply a -6 st declination across the phrase plus a 2.57 st
phrase-final fall. Both are now off by default, and that is the single biggest
change the mode has had.

The units are selected from real bulletin speech, so they already carry a
declination and already carry a final fall; the contour was adding a second one
on top of each. Measured from the engine's own mark dump, with the same phrases
rendered with the contour zeroed as the reference:

| phrase | span | Tom's own units | tail, own units | old `DECL_ST=-6` |
|---|---|---|---|---|
| radar | 1.50 s | -0.71 st/s | -0.79 st | -3.54 st/s, tail -2.52 st |
| tor | 3.70 s | -0.89 st/s | -0.77 st | -2.57 st/s, tail -3.74 st |
| svr | 5.51 s | -0.61 st/s | -1.05 st | -1.44 st/s, tail -3.07 st |

`DECL_ST` is a total over normalised position, so the same -6 st was crammed
into whatever length the phrase happened to be, making the shortest phrase the
steepest, at five times the rate the voice ever uses. Zero-meaning then split
the error both ways: the tail sat 2.5 to 3.7 st below Tom's median while the
onset was pushed to +3.9 st above it. Two listeners described the two ends
independently, as "robotic at the high end" and "as if he's at the end of a very
long sentence with no breaths taken". `SPFY_RATE` could not fix it, because
stretching the phrase stretches the curve with it.

With both off, the realised tail is -1.19 / -0.82 / -0.97 st against the units'
own -0.79 / -0.77 / -1.05. This is not the stage doing nothing: 94 to 97% of
marks are still warped. The accents, downstep and per-word offsets all still
run. What stopped is the contour arguing with the phrase's overall shape.

To get declination back, use `SPFY_PROSODY_DECL_RATE_ST_S` rather than
`DECL_ST`: it is semitones per second, so the slope stops depending on phrase
length, and `SPFY_PROSODY_DECL_MAX_ST` bounds what a long sentence can
accumulate. `-0.35` was the intermediate setting; `-0.20` is gentler.
`SPFY_PROSODY_FALL_ST=2.57` restores the old phrase-final fall.

For bulletin-length text a shallower ramp reads better than for a standalone
sentence, because the contour resets at every phrase boundary rather than once.
Start from `SPFY_PROSODY_DECL_ST=-3` with `SPFY_PROSODY_MAX_ST=4`.

### LP-PSOLA: how the pitch change is realised

S4 mode shifts pitch through the LP residual, not the waveform
(`SPFY_PSOLA_METHOD=lp`). `td` restores plain TD-PSOLA.

Plain TD-PSOLA has a known weakness: raising pitch means emitting more glottal
pulses than the recording contains, so grains get duplicated, and a grain
carries a pulse plus the formant ring after it. Packing grains closer packs the
rings together, the closed phase disappears, and the result is a pressed, buzzy
voice. Listeners here described it as "straining to hit the notes", and
asymmetrically: raising pitch strained, lowering it did not. That asymmetry is
inherent, because lowering pitch spreads pulses apart and lets more closed phase
through, which is what a real voice does.

A dose ladder judged by ear (0.2 / 0.5 / 1.0 / 3.0 st) came out cleanly
monotonic, the signature of an artifact proportional to how far grains move.
Everything that left that distance alone failed: sub-sample and blended grain
placement, asymmetric up-limiting, the 95 Hz floor, a dead zone, and unit
re-selection. Re-selection cannot help in relative mode at all, because the
shift is the contour, whatever unit sits in the slot.

LP-PSOLA runs the grain walk on the LP residual (order 10, fitted
pitch-synchronously at each glottal mark) and rebuilds the formants afterwards
with coefficients pinned to the original time axis, so formants stay put while
only the excitation moves. A duplicated residual pulse duplicates far less
signal, and the ring is regenerated rather than copied.

Measured: 0 fallbacks in 94 units, +9.8% synthesis time, within 0.9 dB of TD in
every band. By ear it removes the stutter while keeping the accent strength, so
the artifact and the prosody are no longer traded against each other.

LP mode declines to run when every ratio is exactly 1.0. That is deliberate:
with a zeroed contour the TD path reconstructs the input bit-for-bit and the
identity gate depends on it, whereas a filter pair round-trips only to
floating-point accuracy. An unstable fit or a diverging synthesis filter also
falls back to TD silently; `SPFY_PSOLA_LP_STATS=1` reports the rate.

### Sub-sample grain placement

TD-PSOLA moves each glottal pulse to a new position. The original code rounded
that position to a whole sample, and consecutive pulses round independently, so
the realised period differed from the requested one by up to a full sample. At
Tom's roughly 70-sample period that is a random +/-0.25 semitone on every
glottal cycle, heard as roughness. Measured on three bulletins at 8.7 to 9.4
cents mean, 25 cents at the 95th percentile.

S4 mode now places grains at their exact fractional position instead
(`SPFY_PSOLA_SMOOTH=1`, on by default in the mode). On a synthetic probe that
locates pulses in the rendered audio, jitter drops from 0.2926 samples to
0.0257; the instrument's own floor is 0.0259, so the error is gone rather than
reduced. `SPFY_PSOLA_SMOOTH=0` restores the original behaviour for A/B.

In listening, nobody could hear a difference. It is kept on because it removes a
real error and takes a confound out of the way of later work, not because it
fixed anything audible. If the roughly 11% extra synthesis time is not worth it,
set `SPFY_PSOLA_SMOOTH=0` and nothing else changes.

The cost is real and bounded. Moving a signal by a fraction of a sample requires
resampling it, and a half-sample delay has exactly zero gain at Nyquist for any
symmetric filter. On real renders that shows up as -0.6 to -1.2 dB between 3.8
and 4.0 kHz, a band holding about 0.5% of the energy; everything below 3.6 kHz
stays within +/-0.2 dB.

None of this touches the engine proper: the byte-exact audit still passes on
every voice, and the prosody stage with a zeroed contour is still bit-identical
to the stage being off, at every `SPFY_PSOLA_SMOOTH` level.

### Prosodic position is a target, and it is addressable

`The National Weather Service.` and `m The National Weather Service.` realise
the word "the" differently, and for a long time that looked unreachable. It is
not the front end: the FE tagging for `<the>` is byte-identical in both, with
the same POS `det`, stress `0`, phones `dh ix` and syllable mark.

What changes is the CART prosodic-position target, five features per half-phone,
named by the engine's own VCF weights:

| | VCF weight | meaning |
|---|---|---|
| `sp[0]` | `PHRASE_POS_MISMATCH_COST` | syllable position in phrase |
| `sp[1]` | `STRESS_MISMATCH_COST` | stress / syllable type |
| `sp[2]` | `SYLL_IN_WORD_MISMATCH_COST` | syllable within word |
| `sp[3]` | `WORD_IN_PHRASE_MISMATCH_COST` | word position in phrase |
| `sp[4]` | `PHONE_IN_SYL_MISMATCH_COST` | phone within syllable |

Putting any word in front of "the" moves two of them, `sp[0]` from 1 to 8 and
`sp[3]` from 5 to 2, because both are purely positional. Measured one at a time,
only `sp[3]` matters: forcing `sp[0]` alone changes nothing whatsoever, while
forcing `sp[3]` alone reproduces the entire effect.

`SPFY_SP_OVERRIDE` sets them directly. Half-phone indices, `;` between entries,
`-` keeps a component:

```text
SPFY_SP_OVERRIDE=2:-,-,-,2,-;3:-,-,-,2,-;4:-,-,-,2,-;5:-,-,-,2,-
```

Indices count half-phones over the whole phrase including the leading pau,
matching `SPFY_SP_TARGET_DUMP` and `SPFY_PROSODY_SLOT_DUMP`. Unset is a no-op,
so the byte-exact audit is untouched.

On this example that takes half-phone agreement with the `m` render from 20/38
to 34/38 without saying the "m". It does not reach 38/38 and cannot: in `m The`
the `dh` of "the" is joined to the `m` of "em", so part of the difference is
join context rather than target, and no target override reaches that. The `ix`
half matches exactly; the `dh` half does not. It also costs duration, with "the"
going from 65 ms to 78 ms against the 66 ms the `m` version produces, because
`sp` feeds the duration tree as well as the unit cost.

### Tuning it

The mode sets environment defaults, so any variable you set yourself wins:

```sh
SPFY_4_MODE=1 SPFY_PROSODY_DECL_ST=-3 spfy_synth ...
```

does exactly what it looks like. The values it installs are `SPFY4_*` `#define`s
at the top of `spfy/src/cli/spfy_synth.c`; edit them there to change what the
mode means.

---

## Setting environment variables

Every knob below is an environment variable. The three shells differ enough to
be worth stating once.

**cmd.exe.** `set` persists for the rest of the session; use a subshell or
`set VAR=` to clear it.

```cmd
set SPFY_4_MODE=1
spfy_synth.exe ...
set SPFY_4_MODE=
```

**PowerShell.** `$env:` assignment, and `Remove-Item` to clear. Note that `set`
is an alias for `Set-Variable` here and will not set an environment variable.

```powershell
$env:SPFY_4_MODE = "1"
& spfy_synth.exe ...
Remove-Item Env:\SPFY_4_MODE
```

**bash / zsh.** Prefix the command to scope the variable to that one run, which
is almost always what you want:

```sh
SPFY_4_MODE=1 SPFY_PROSODY_DECL_ST=-3 spfy_synth ...
```

A stale exported `SPFY_*` variable is the most common cause of "it worked
yesterday". The parity gate runs in a cleaned environment for exactly this
reason; if you are chasing a mismatch, clear your `SPFY_*` variables first.

---

## Parity gate

`master_spfy_parity.py` is the single gate: it audits selection and compares the
rendered samples in one run.

```sh
python spfy/test/oracle/master_spfy_parity.py \
  --exe /tmp/spfy_build_linux32/src/cli/spfy_synth
```

```powershell
python spfy\test\oracle\master_spfy_parity.py `
  --exe C:\tmp\spfy_build32\src\cli\spfy_synth.exe
```

Without `--voice` it audits Tom. `--all-voices` runs all eight and prints the
per-voice summary at the top of this file; it already includes Tom, so there is
no need to run the gate twice. Each voice follows its language's corpus
(`corpus.jsonl`, `corpus_es_MX.jsonl`, `corpus_fr_CA.jsonl`) automatically.
`--allow-missing-traces` audits what it can when a voice's traces are
incomplete, rather than refusing outright.

Exit code is 0 only when everything measured matched; 1 is a mismatch and 2 is a
missing or broken answer key.

The metric lines narrow from inputs to output, and each can pass while the next
fails. `PATH UID` once read 100% while the audio gate was failing. Read
`BYTE-IDENTICAL`; the rest tell you where a regression is, not whether one
exists.

`BYTE-IDENTICAL` requires the original engine, since it renders the reference
through `bin/spfy_dumpwav.exe`. Use `--no-audio` where the engine is
unavailable, but the selection metrics alone do not imply the audio matches.

Any engine change must leave `--all-voices` exiting 0: 221/221 on each en-US
voice, 100/100 on each es-MX and fr-CA one. Every feature described in this file
is environment-gated with an identity default specifically so that stays true.

The gate's audio stage strips `SPFY_*` from the child environment by design, so
a change hidden behind an env switch cannot be scored through it. Use
`env_bytes.py "KEY=VALUE" <voice...>`, which renders each corpus both ways and
diffs against `spfy/test/oracle/engine_ref/`.

---

## Architecture

```text
  text
   |
   v
+--------------------------------------+
| FE host (host_emu x86 interpreter)   |
|   Runs SWIttsFe-en-US.dll (32-bit)   |
|   Same on every platform and arch    |
+--------------------------------------+
   |  tagged FE output
   v
+--------------------------------------+
| Slot builder (build_graph + link)    |
|   Phrase / Word / Syl / Halfphone    |
+--------------------------------------+
   |
   v
+--------------------------------------+
| Unit selection (Viterbi DP)          |
|   PRSL pool query                    |
|   Per-cand TC (D/F0/SP/S/FLAG)       |
|   Anchor scoring (multi-unit spans)  |
|   Same-recording adjacency join cost |
|   HP histogram prune                 |
+--------------------------------------+
   |  chosen UID path
   v
+--------------------------------------+
| Prosody stage   OPTIONAL, off        |   <-- not in 3.0.5
|   F0 contour from the FE's ToBI marks|
|   LP-PSOLA (S4 default) or TD-PSOLA  |
|   on the VDB's own pitch marks;      |
|   duration untouched                 |
+--------------------------------------+
   |
   v
+--------------------------------------+
| WSOLA streamer                       |
|   Engine UID-batching                |
|   NCC lag search, Hann-windowed OLA  |
|   Optional whole-utterance rate      |
+--------------------------------------+
   |
   v
  WAV
```

The unit-selection scoring stack (D/F0/SP/S target costs, join cost, anchor
cost, HP prune) is bit-for-bit engine-faithful. The WSOLA streamer matches the
engine's "Plain WSOLA" mode, the path every voice here uses; the engine's PSOLA
branch is dead code for all of them.

Both stages are byte-exact rather than merely close. On the synthesis side that
meant reproducing the engine's per-phrase emission arithmetic (each phrase's
first push emits a `hop` prologue before its body; a zero-duration unit is still
a unit and still gets its crossfade) and its lag search's mixed precision: the
correlation accumulates in 80-bit x87 registers while the incumbent best is
stored as a float32, so two lags that tie in float32 need not tie in the
comparison the engine actually makes.

The prosody stage sits between selection and WSOLA and is the one box in that
diagram the original engine does not have.

---

## Env knobs

### Update check

```text
SPFY_NO_UPDATE_CHECK=1        no automatic check anywhere, SAPI included
SPFY_UPDATE_URL=<url>         manifest to poll; file:// works, which is how
                              the whole path is tested without a server
SPFY_UPDATE_INTERVAL_DAYS=N   override the stored interval (0 = every run)
SPFY_UPDATE_TIMEOUT=N         seconds for the whole transfer (5 inline in
                              spfy_synth, 20 in the detached helper)
SPFY_VOICE_DIR=<path>         where voices are searched for and installed
```

### Speechify 4 mode

```text
SPFY_4_MODE=1                 the whole recipe: accents on, the 95 Hz floor
                              with a 1 st knee, and no contour declination or
                              phrase-final fall
```

### Prosody stage

Only consulted when the stage is on. Every default is identity.

```text
SPFY_PROSODY_STAGE=1          master switch
SPFY_PROSODY_PM=<stem>        pitch-mark stem (<stem>.pmindex/.pmdata)

# Contour shape
SPFY_PROSODY_DECL_ST=N        declination over the phrase, semitones (-2).
                              A fixed SPAN, so its slope depends on how long
                              the phrase happens to be
SPFY_PROSODY_DECL_RATE_ST_S=N declination as semitones PER SECOND, so slope
                              does not depend on phrase length. Non-zero
                              overrides DECL_ST. 0 in S4 mode
SPFY_PROSODY_DECL_MAX_ST=N    bound on the span the rate may produce (6.0,
                              4.0 in S4 mode; 0 = unbounded)
SPFY_PROSODY_DECL_SHAPE=K     ramp exponent, st = decl * u^K  (1.0 = straight)
SPFY_PROSODY_ACCENT_ST=N      pitch-accent height (3.0)
SPFY_PROSODY_DOWNSTEP=F       each accent as a fraction of the last (0.70)
SPFY_PROSODY_DOWNSTEP_FLOOR=F floor under that decay (0.30; 1.0 in S4 mode,
                              i.e. no downstep decay at all)
SPFY_PROSODY_NUCLEAR=F        scale the FINAL accent separately (-1 = off)
SPFY_PROSODY_FALL_ST=N        phrase-final fall (2.57; 0 in S4 mode, because
                              the selected units already carry one)
SPFY_PROSODY_VALLEY_ST=N      low target between grouped accents (0 = off)
SPFY_PROSODY_WIDTH_MS=N       accent bump width (90)
SPFY_PROSODY_ALIGN_MS=N       accent peak alignment, negative = earlier (-25;
                              0 in S4 mode)
SPFY_PROSODY_ZEROMEAN=F       fraction of the contour mean removed (1.0)
SPFY_PROSODY_ZEROMEAN_ACC=F   same, for accents and fall separately
SPFY_PROSODY_LEVEL_ST=N       flat offset applied after zero-meaning

# Where it lands
SPFY_PROSODY_ABSOLUTE=F       0 = ride each unit's own pitch, 1 = impose base_hz
SPFY_PROSODY_BASE_HZ=N        the absolute reference, when ABSOLUTE > 0
SPFY_PROSODY_MAX_ST=N         limit on the shift applied to any mark (4.0;
                              12 in S4 mode, which is also the accepted
                              ceiling)
SPFY_PROSODY_F0_FLOOR_HZ=N    soft absolute floor on realised F0 (0 = off)
SPFY_PROSODY_F0_CEIL_HZ=N     soft absolute ceiling  (0 = off)
SPFY_PROSODY_F0_KNEE_ST=N     compression depth past either bound (2.0)

# How the pitch change is realised
SPFY_PSOLA_METHOD=lp|td       lp = LP-PSOLA, the grain walk runs on the LP
                              residual and formants are rebuilt afterwards
                              (S4 mode default). td = plain TD-PSOLA
SPFY_PSOLA_SMOOTH=N           0 = whole-sample grain placement (the original)
                              1 = sub-sample placement (S4 mode default)
                              2 = 1, plus blending of adjacent grains
SPFY_PROSODY_MAX_UP_ST=N      separate ceiling for UPWARD shifts (0 = same as
                              MAX_ST). Up and down are not the same operation
SPFY_PROSODY_DEADZONE_ST=N    smoothly suppress shifts below N semitones, which
                              cost artifact and buy little prosody (0 = off)
SPFY_PSOLA_LP_STATS=1         report per unit whether LP ran or fell back

# Selection (opt-in, see the warning below)
SPFY_PROSODY_RESELECT=1       post-Viterbi F0-aware substitution
SPFY_PROSODY_RESELECT_BAND=1  target the landing BAND rather than a point
SPFY_PROSODY_RESELECT_BAND_LO/_HI   band bounds (default: the F0 knee's)
SPFY_PROSODY_RESELECT_CROSS=N cost of leaving the neighbour's recording (2.0)

# Diagnostics
SPFY_PROSODY_MARK_DUMP=1      one line per pitch mark: uid, period, ratio,
                              output-sample anchor, target, and why
SPFY_PROSODY_SLOT_DUMP=1      per slot: uid, unit F0, contour st, landing Hz
SPFY_PROSODY_WARP_DUMP=1      per unit: requested vs applied shift
SPFY_PROSODY_F0_TABLE=<path>  every unit's F0 + recording + class
SPFY_PSOLA_GRAIN_DUMP=<path>  one line per synthesis grain (position, source
                              mark, window, ratio, sub-sample offset) plus a
                              per-unit coverage summary
SPFY_SP_TARGET_DUMP=1         per half-phone CART prosodic-position target
SPFY_SP_OVERRIDE=<spec>       force that target; see the prosodic-position
                              section above
SPFY_ETAG_DUMP=1              inline-tag resolver output + per-word map
```

`SPFY_PSOLA_SMOOTH=2` is a judgment call, not a defect fix. Level 1 removes a
measured error; level 2 blends each grain with its neighbour to break up the
exact repeats that pitch-raising produces, which trades a little buzz for a
little smearing. Nobody has shown it is better. Listen before using it.

`SPFY_PROSODY_RESELECT` changes which units are chosen. Both variants were
measured: the band form cuts the work the F0 knee has to do by about 27% using
only same-recording joins, but was judged by ear to add a slight microstutter,
and the permissive form (freely crossing recordings) audibly damages some words.
It is off by default and stays that way.

### Rate and pacing

```text
SPFY_RATE=F                   whole-utterance time-scale, applied AFTER
                              selection. >1 faster, <1 slower. Cannot change
                              which units are picked. 1.0 = pass-through
SPFY_INTERWORD_MS=N           inter-word silence (0 = engine-faithful)
SPFY_INTERWORD_FLOOR=N        dither level in those gaps (24)
SPFY_GAP_FADE_MS=N            crossfade width across an inserted gap (28)
```

### SSML

```text
SPFY_NO_SSML=1                skip the SSML translation pass entirely
SPFY_SSML_DUMP=1              print the translated text and the DSP spans
```

### Diagnostics

```text
SPFY_VERBOSE=1                full pipeline chatter (same as -v)
SPFY_SYNTH_DEBUG=1            per-half-phone JSON: ctx, sp, q5, durt/f0tr
                              mean+var, pool_n, cands. This is the line the
                              parity gate reads
SPFY_FULL_POOL_DUMP=1         emit the WHOLE candidate pool on that line
                              instead of the first 16. Mandatory for any
                              pool-membership question: the cap is a display
                              limit and pool_n reports the truth, so without
                              this a big pool reads as a 16-entry one
SPFY_TRACE_UNITS=1            per-push unit info to stderr
SPFY_WSOLA_VERBOSE=1          per-push lag + NCC diagnostic
SPFY_WSOLA_TRACE=1            per-WSOLA-push [wsolau]/[wsolat]/[wsolask]:
                              uid, run length and pau resizes, then the push's
                              OUTPUT SPAN, lag, pre, content, buf_n and
                              read/stop cursors, plus a line for any slot the
                              concat loop skipped. The output span is what
                              maps a first-differing sample onto one join
SPFY_WSOLA_DUMP_JOIN=<path>   append every join's two inputs as binary records
                              ("SWJ1", 9x u32 header, then hist_n + buf_n
                              int16). Lets the engine's chosen lag be SOLVED
                              for offline, by trying every candidate against
                              the reference, instead of inferred from a
                              length delta
SPFY_DEBUG_MISMATCH=1         per-slot chosen-vs-best diff dump
SPFY_DUMP_PATH=1              the DAG path (slot -> uid, kinds, pred/cand
                              counts) plus `hp N: uid=U` per half-phone
SPFY_PATH_DUMP=1              chosen UID per slot, one JSON line per phrase
SPFY_TC_DUMP=1                per-slot target costs, post-scorer/pre-prune
SPFY_JOIN_DUMP=1              per-edge join cost
SPFY_UID_DUMP=<path>|-        NDJSON: candidate pool, pick, and emit position
SPFY_UID_OVERRIDE=<path>      force specific units in, to hear a hypothesis
SPFY_WORD_EVENTS_FILE=<path>  word-event sidecar TSV (64-bit SAPI shim)
SPFY_HOST_TRACE=1             PE loader phase markers + TIB install info
```

`SPFY_JOIN_DUMP` and `SPFY_TC_DUMP` write to stderr. Parse merged output or
every edge reads as absent.

### SAPI

```text
SPFY_SAPI_DEBUG=1             SAPI DLL log to %TEMP%\_sapi_dbg.log
                              (must be set in the CONSUMER process env)
SPFY_SAPI_PHONE_DEBUG=1       append raw pPhoneIds to %TEMP%\_sapi_phone_log.txt
```

### Engine-faithfulness reverts

Kept for regression diagnosis. Each one restores a behaviour that was wrong;
setting any of them will break parity, which is the point. Every one is a no-op
unset, so the shipped path is the engine-faithful path.

Because the gate strips `SPFY_*`, score these with `env_bytes.py` rather than
through the gate itself. The per-voice deltas quoted below come from that tool
over all eight voices.

#### Concatenation / WSOLA

```text
SPFY_WSOLA_NO_FIRST_HOP=1     collapse a phrase's first push back into one
                              contiguous write. The engine emits `hop` samples
                              from buf[pre] and THEN starts the body at
                              pre+hop; the two coincide unless the unit is
                              shorter than 2*hop, where the collapsed form
                              emitted nothing at all      (es-MX -11 each)
SPFY_ZERO_DUR_SKIP=1          drop zero-duration units again. The engine still
                              builds their span and still emits the W blend
                              samples; only the body loop finds nothing
                                                          (javier -1)
SPFY_PAU_AVAIL_LEGACY=1       restore the guard that counted each earlier pau
                              splice twice, so the second pau in a run never
                              resized                     (jill -3)
SPFY_WSOLA_LAG_F32=1          round the lag search's accumulators to float32.
                              The engine keeps them in 80-bit x87 registers
                              and compares against a float32 incumbent, so
                              float-everywhere manufactures ties the engine
                              does not have    (jill -1, aicraig/aimara -2/-3)
SPFY_WSOLA_LEGACY=1           the old streaming OLA instead of the engine path
SPFY_WSOLA_NO_PREROLL=1       over-read after the unit instead of before it
SPFY_WSOLA_NO_OVERREAD=1      decode no look-ahead past the unit
SPFY_WSOLA_NO_SILENCE_FADE=1  hard-cut inserted gaps instead of fading
SPFY_NO_RUN_BATCH=1           revert engine UID-batching to pair-only
```

#### Selection

```text
SPFY_HP_MAX=<n>               override HALFPHONE_CAND_MAX_UNITS. The half-
                              phone histogram prune's cap is a VCF parameter
                              (config +0x48, pushed into FUN_08e88830 beside
                              THRESH and SLOPE); 50 is only the constructor
                              DEFAULT. =50 restores the old hardcoded literal
                                                          (aimara2 -8)
SPFY_NO_BOUNDARY_PIN=1        preselect the first and last half-phone of a
                              phrase instead of pinning them. The engine hands
                              those two slots exactly one unit, index 0 and
                              n_units-1, on 983 of 983 phrases across five
                              voices                      (felix: 11 slots)
SPFY_NO_PRSL_BOTH_FALLBACK=1  stop the context substitution at one side. On a
                              double miss the engine drops BOTH contexts and
                              takes the centre-class group
                                                          (aicraig/aimara -1)
SPFY_ANCHOR_NO_TAIL=1         truncate an anchor to its half-phone span again,
                              losing the units it carries beyond it
                                                          (paulina -1)
SPFY_SPR_NO_FINAL_ACCENT=1    let a phrase-final boundary tone replace the
                              pitch accent in an inline SPR literal instead of
                              riding alongside it (`.1,H*;L-L%`). Byte-neutral,
                              but costs tom and jill 12 SLOT each
SPFY_ANCHOR_PRUNE_LEGACY=1    anchor histogram prune: divide by norm+best and
                              round rather than divide by norm and truncate
SPFY_ANCHOR_NO_D_CLAMP=1      skip a candidate's tail units instead of
                              clamping the walk index at last_hp
SPFY_C7C_LEGACY=1             force the c80 run-counter branch regardless of
                              the voice's GET_RID_OF_PATH_F0
SPFY_MAX_CANDS=<n>            re-impose a preselect pool clamp (none by
                              default; the old 512 made engine candidates
                              unreachable at any cost)
SPFY_NO_PRSL_92_FALLBACK=1    disable the one-sided context substitution too
SPFY_NO_HP_PRUNE=1            disable HP histogram prune
SPFY_NO_HP_EARLY_EXIT=1       disable FUN_08e88de0 running-min early exit
SPFY_HP_EARLY_EXIT_VAL=<f>    override that early exit's slack (default is
                              HALFPHONE_CAND_PRUNE_THRESH, config +0x4c)
SPFY_HP_BIN_LROUND=1          revert HP_PRUNE truncation-binning fix
SPFY_NO_HP_PRUNE_BIN39_GUARD=1  always filter, ignoring the break-bin<39 gate
SPFY_NO_HP_SORT_UID_TIE=1     sort survivors by cost only (no uid tie-break)
SPFY_D_IDX_TARGET=1           revert D-span indexing to target_idx
SPFY_NO_UNIT_PHONE_CTX=1      stop reading q3/q4 from the unit table
SPFY_NO_ANCHOR_DURT_WALK=1    skip the anchor's durt walk entirely
SPFY_ANCHOR_VOICING_GATE=1    re-enable voicing gate in anchor init
SPFY_NO_ANCHOR_HEAD_C6C=1     revert c6c head-vs-tail anchor fix
SPFY_PAU_SKIP=1               skip uid 0 / the silence sentinel outright
SPFY_PAU_FULL_DUR=1           emit recorded pauses at full length, ignoring
                              the FE's target
```

`SPFY_ANCHOR_D_TGT_DECOMP=1` (with `SPFY_ANCHOR_D_GROUP=N`) is a research
switch, not a revert: it selects the decompiled anchor target index, which is a
net loss even with the question features correct (tom -5, paulina -2, javier -2,
felix -1). The default stays legacy.

#### Front end

```text
SPFY_PSA_SYL_FROM_RESYL=1     revert to local syllabifier (vs tree walk)
SPFY_NO_SYL_INITIAL_VOWEL=1   revert syllabifier initial-vowel fix
SPFY_FE_LIAISON_LEGACY=1      revert the fr-CA liaison handling
SPFY_FE_HOST_NO_LEXICAL_OVERRIDE=1   disable no-refine lexicon
```

#### Prosody stage (only meaningful when the stage is on)

```text
SPFY_PROSODY_PM_PAU=0         read `pau` units' marks as pitch (they are not)
SPFY_PROSODY_NAT_GUARD=0      stop discarding implausible pitch marks
SPFY_PROSODY_PM_LEADIN=0      treat each unit's lead-in offset as a period
```

---

## Layout

```text
spfy/
  CMakeLists.txt
  build32.bat  build_linux.sh  build_macos.sh   local builds
  ci/
    build_unix.sh                   the one script every CI unix leg runs
    build_gui_linux.sh              the GUI, native or cross, same ceiling
  include/spfy/                     public C API
  gui/                              Tauri desktop app (see its own README)
  wasm/                             Emscripten target + web front end
  third_party/miniz/                vendored zip + inflate, for voice install
  src/
    common/       obfuscation (XOR), riff, file_io, env, log, voice_find,
                  ssml (the SSML-to-inline-tag translator)
    voice/        VIN/VDB/VCF loaders, unit/feat tables, ccos, voice_runtime
    cart/         CART evaluator (durt + f0tr)
    text_norm/    text normalisation
    g2p/          grapheme-to-phoneme
    usel/         hash, PRSL, costs (S/D/SP/F0/FLAG), build_graph, link_graph,
                  slot_ctx, anchor_score, viterbi DP
    prosody/      pmarks, contour, psola_unit, reselect     (the S4 stage)
    wsola/        ulaw, wav writer/sink, WSOLA streamer, span DSP
    dsp/          pitch_shift (TD-PSOLA), time_stretch (WSOLA frame-based)
    fe_host/      FE driver + parse
    fe_internal/  in-house FE, for A/B against the hosted one
    fe/           earlier hand-written FE stages, still linked for shared
                  utilities
    host_emu/     x86 interpreter that runs the FE DLL on every platform
    synth/        spfy_voice_t + per-call synth library
    sapi/         Windows SAPI 5 voice DLL (32-bit + 64-bit)
    update/       update check and voice install, split so the SAPI DLL links
                  only what it needs: spfy_update_trigger (paths, state,
                  spawn) and spfy_update_core (fetch, SHA-256, manifest,
                  notify, upd_voice)
    cli/          spfy_synth, spfy_dump_voice, replay tools, and others
  test/
    oracle/    221-entry en-US corpus + 100-entry es-MX and fr-CA corpora,
               engine_ref/ answer-key WAVs, traces_master*/ per voice, and
               master_spfy_parity.py, the gate
    diff/      WAV / per-cand-total diff utilities
    unit/      C unit tests
    ssml_translate_test.py  ssml_effect_test.py   SSML gates
```

### CLIs

| CLI | Purpose |
|---|---|
| `spfy_synth` | text (or inline-SPR phonemes) to WAV; also lists and installs voices |
| `spfy_gui` | the desktop app; drives `spfy_synth` as a subprocess |
| `spfy_update` | check for a newer engine or rebuilt voices (`--status`, `--disable`, `--dismiss`) |
| `spfy_dump_voice` | introspect a VIN/VDB/VCF (units, ccos, prsl, and so on) |
| `spfy_dump_f0` | per-voice F0 byte distribution |
| `spfy_f0_flatten` | flatten a WAV's F0 to a constant, for A/B listening |
| `spfy_fe_text2tagged` | text to the FE's tagged output (`--pipe` for one line per stdin). Plain-text path only: it does not expand inline `\![...]` escapes, which `spfy_synth` handles itself |
| `spfy_prosody_test` | exercise the contour model without synthesizing |
| `spfy_pitch_shift` | A/B the TD-PSOLA pitch shifter on a WAV |
| `spfy_time_stretch` | A/B the WSOLA time-stretch on a WAV |
| `spfy_concat` | (legacy) concat oracle-chosen units without WSOLA |
| `spfy_*_replay` | replay captured engine traces through the C port |

---

## Cost stack reference

All scoring uses long-double accumulators with a final cast to `float`, matching
MSVC 7.1 / 2003 x87 80-bit semantics.

```text
D-cost  = | (1/stddev) * (unit_mem[+0x12] - durt_mean) |^2 * DUR_WEIGHT
F0-cost = MISSING_F0_COST                                    if voicing[hp_class]==0
        = w_f0_miss                                          if stored_f0 == 0
        = | (1/stddev) * (stored_f0 - f0tr_mean) |^2 * ABS_F0_WEIGHT
SP-cost = sum(k=0..4) weight[k] * matrix[k][target_feat[k]][cand_byte[k]]
S-cost  = ccos_weight * sum(slot=0..3)
              ccos[hp_class*4+slot][s_remap[target.ctx[s]]][s_remap[cand.ctx[s]]]
FLAG    = cand.context_cost * 0.25 * 0.01
JOIN    = 0                                        if curr.uid == prev.uid + 1 && curr.flag_b
        = hash_value                               if hash hit
        = JOIN_COST_OFFSET + smooth_curve(idx)     if the voiced-join precondition is met
        = miss_offset + f0_edge_penalty            otherwise
```

The cache-miss price is not a flat `miss_offset`. `FUN_08e8b620` adds a gated
F0-edge penalty on top, fired only when
`curr.f0_end > 20 && prev.c80 < 15 && prev.c7c > 20`, and whether `prev.c80` can
ever fall below 15 is decided by the per-voice VCF flag `GET_RID_OF_PATH_F0`
(config +0x94):

```c
if (curr.f0_mid < 21) { c7c = prev.c7c;    c80 = cfg94 ? 100 : prev.c80 + 1; }
else                  { c7c = curr.f0_mid; c80 = cfg94 ? 0   : curr.f0_start; }
```

tom and felix set the flag, so their `c80` is pinned to 100 and the penalty can
never fire; a flat 1000.0 is correct for them. The es-MX voices omit it, and
there the engine charges 1000.18 where a flat 1000.0 was wrong. Reading it
per-voice is what unlocked es-MX selection parity.

Engine-truth values for Tom, from the VCF and runtime capture:

| Constant | Value |
|---|---|
| `DUR_WEIGHT` | `0.3` |
| `ABS_F0_WEIGHT` | `0.2` |
| `MISSING_F0_COST` (`w_f0_miss`) | `5.0` |
| `JOIN_COST_OFFSET` | `0.2` |
| `gate_weight` (smooth-curve) | `0.6` |
| `miss_offset` | `1000.0` |
| SP weights | `[0.05, 0.05, 0.05, 0.05, 0]` |

Notable: the VCF param `DUR_WEIGHT` scores `unit_mem[+0x12]`, which is
`f0_context`, not duration, despite the name. The engine's "duration" cost is
really scoring the unit's contextual-pitch byte against the f0tr CART
prediction.

---

## Linux build: how it works

The FE is a 32-bit Windows PE. Rather than load it natively, every platform runs
it through the `src/host_emu` x86 interpreter; the native PE loader was retired
2026-07-22. That is what lets arm64, armv7 and x86_64 all produce byte-identical
output, because no target has to execute x86 code natively.

Two ABI details cost the most time back when the loader was native, and still
document how the DLL behaves:

1. **MSVCRT `_iob` is FILE-structs-inline, not an array of pointers.** MSVCR71
   exports `_iob` as 32-byte `FILE` structs; `stdin`/`stdout`/`stderr` expand to
   `&_iob[0..2]`, pointers into that array. A pointer array runs off the end
   into BSS and `getObject(2)` starts returning garbage.
2. **`getObject` is `__cdecl`, not `__stdcall`.** Its exported thunk ends in a
   plain `ret`, so the caller cleans the stack. Misdeclaring it worked on a
   small test binary (gcc used EBP-relative locals, invariant to post-call ESP)
   and corrupted the full synth's frame, where gcc went ESP-relative. A single
   `ret` versus `ret N` tells the truth; verify against the disassembly.

`SPFY_STRICT_FP` pins x87 80-bit semantics. Measured 2026-07-22: it is not
load-bearing. ARM matches bit-for-bit without it, because the selection path is
integer and the audio path is IEEE-stable.

---

## Frida hook policy

Function-entry hooks only. Mid-function `Interceptor.attach` inside
`SWIttsUSelUnitSelection`'s x87 loops destabilises the engine stochastically:
accumulated trampoline trips perturb the x87 stack and it eventually
access-violates. The same hook ran clean on 1735 probes one day and crashed at
512 the next.

If hot-path data is needed, use Frida Stalker instead, which is slower but
robust. Retired hot-path hooks are kept in `viz/frida_hooks/` behind DANGER
banners; do not re-add them to `run_frida_capture.py`'s `HOOK_JS` map without a
Stalker rewrite.

---

## Reverse-engineering lessons

The bugs that took the longest:

1. **`feat.filename` is keyed by `stored_id`, not position.** Sort the `feat`
   chunk's filename entries by `stored_id` at load time. Naive positional lookup
   passes boundary tests, because first and last match by coincidence, and
   silently misroutes mid-corpus units to the wrong recordings. This was the
   dominant cause of "Tom but garbled".
2. **`unit_mem[+0x12]` is `f0_context`, not `dur_like`.** Replacing
   `cand.dur_like` with `cand.f0_context` in the D-cost doubled the aggregate
   match overnight.
3. **The four ccos candidate bytes are signed (`MOVSX`); SP/D/FLAG are unsigned
   (`MOVZX`).** Tom's silence sentinel `0xff` is `-1` in the ccos path; read as
   unsigned our score was +18.85 off, as signed it was bit-exact.
4. **`durt` scores `f0_context`; `f0tr` scores `f0_start`, not `f0_mid`; the
   `*_var` fields are precisions (1/sd), not variances.** Field names in this
   format are not to be trusted without a disassembly check.
5. **A constant that fits every voice you hold may be a DEFAULT.** The half-phone
   prune's cap was hardcoded at 50 and commented as an engine global. 50 is the
   config constructor's default (`mov [esi+0x48], 0x32`);
   `tts.voiceCfg.HALFPHONE_CAND_MAX_UNITS` overrides it, and `FUN_08e88de0`
   pushes it into the prune beside `PRUNE_THRESH` and `PRUNE_SLOPE`, two
   parameters already read from the VCF. Six of eight voices omit the key, so
   the literal looked universal. When a hardcoded number has a same-named
   `tts.voiceCfg.` string in the DLL, read the loader.
6. **Precision can be asymmetric within one comparison.** The WSOLA lag search
   accumulates in 80-bit x87 registers but stores its incumbent best as a
   float32, then compares the register against that float32. Computing in float
   throughout is not a rounding nicety: it manufactures ties the engine never
   has, and the tie-break decides the lag, which shifts every sample after it.

---

## See also

* [SPEECHIFY_4_FINDINGS.md](SPEECHIFY_4_FINDINGS.md) - what differed between
  Speechify 3 and 4, and what is deliberately not claimed
* [spfy/gui/README.md](spfy/gui/README.md) - the desktop app
* [AGENTS.md](AGENTS.md) - working rules for this repository
* [spfy/test/oracle/README.md](spfy/test/oracle/README.md) - oracle harness usage
* [spfy/test/oracle/TRACE_SCHEMA.md](spfy/test/oracle/TRACE_SCHEMA.md) - JSONL trace format
* [spfy/third_party/miniz/README.md](spfy/third_party/miniz/README.md) - why miniz is vendored
* [installer/updates/pack_voices.py](installer/updates/pack_voices.py) - builds the voice zips and the `voices.json` catalog
* [reveng/README_TECHNICAL.md](reveng/README_TECHNICAL.md) - format spec for VIN/VDB/VCF
* [reveng/DLL_ANALYSIS.md](reveng/DLL_ANALYSIS.md) - engine pipeline + function maps
