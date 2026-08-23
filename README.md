# Speechify 3.0.5 - 2003 Speechify TTS Engine Reverse-Engineering/re-implementation

## Regarding "spfy"

See [SPFY_README.md](SPFY_README.md) for details on the Speechify re-implementation engine, written in C, named "spfy". Byte-exact by default with Speechify 3.0.5. It is also the engine used in the Speechify 4 reverse-engineering work, which is described in [SPEECHIFY_4_FINDINGS.md](SPEECHIFY_4_FINDINGS.md).

---

## Installation Instructions (for _almost_ any computer from the last 20 years or so)

1. Download the latest spfy release. This can be found [at this link](https://github.com/wagwan-piffting-blud/Speechify/releases/latest).
2. Run the installer for your platform. The different versions and what platforms they're for are in the table below:

| Platform | Installer |
|----------|-----------|
| Linux (ARM64, i.e. Raspberry Pi 4 / 5 / Zero 2 W on 64-bit Raspberry Pi OS, Orange Pi 5, Radxa Rock, AWS Graviton or Ampere servers, Asahi Linux on an Apple Silicon Mac) | spfy-linux-arm64-20xx.xx.xx.tar.gz |
| Linux (ARMv7, i.e. Raspberry Pi 2 / 3 / Zero 2 W running **32-bit** Raspberry Pi OS, BeagleBone Black, older armhf single-board machines) | spfy-linux-armv7-20xx.xx.xx.tar.gz |
| Linux (x86, i.e. 32-bit Debian or Ubuntu on Pentium 4 / Core Duo / Atom netbook-era hardware, or a 32-bit VM) | spfy-linux-x86-20xx.xx.xx.tar.gz |
| Linux **musl**, 64-bit x86 (i.e. Alpine Linux, a `FROM scratch` container, or any distro too old for the glibc builds above) | spfy-linux-x86_64-musl-20xx.xx.xx.tar.gz |
| Linux **musl**, ARM64 (i.e. Alpine on a Raspberry Pi, or an arm64 Alpine container) | spfy-linux-arm64-musl-20xx.xx.xx.tar.gz |
| Linux (x86_64, i.e. Debian 12+, Ubuntu 22.04+, RHEL 9+, Fedora, Arch on any ordinary 64-bit PC or VPS; also WSL2) | spfy-linux-x86_64-20xx.xx.xx.tar.gz |
| macOS (ARM64, i.e. any Apple Silicon Mac - M1 through M4 - on macOS 11 Big Sur or newer) | spfy-macos-arm64-20xx.xx.xx.tar.gz |
| macOS (x86_64, i.e. any Intel Mac from roughly 2013 onwards, on macOS 11 Big Sur or newer) | spfy-macos-x86_64-20xx.xx.xx.tar.gz |
| Windows (x86, i.e. 32-bit Windows 7 through 11; also installable on 64-bit Windows if you want a 32-bit-only install) | spfy-setup-20xx.xx.xx-x86.exe |
| Windows (x86_64, i.e. 64-bit Windows 7 through 11, including Windows Server 2008 R2 and later) | spfy-setup-20xx.xx.xx.exe |

**What each build actually requires**:

* **The two musl tarballs need nothing at all.** They are statically linked against musl, so there is no interpreter, no `libc.so`, and no version floor: they run on Alpine, on a `FROM scratch` container, and on glibc distros far older than anything CI could build on. If you are unsure which Linux download to take, or the glibc one refuses to start, take a musl build. They are byte-for-byte identical in output to the glibc builds - the reference-WAV check is run against each.
* **The glibc Linux tarballs** need **glibc 2.34 or newer** as measured, with **2.36** as the guaranteed ceiling - so Debian 12 (bookworm), Raspberry Pi OS bookworm, Ubuntu 22.04 and newer, RHEL 9 and Fedora 35+ all run these as-is. All four targets build inside a `debian:bookworm` container, and CI reads the linked binary back and **fails the build** if anything references a symbol newer than 2.36, so this cannot quietly regress the next time a runner image is updated. Each build prints its own figure ("highest glibc symbol referenced").
* **macOS** builds declare **macOS 11.0 (Big Sur) or newer** on both arches, verified from the binary's Mach-O load commands. Big Sur covers Apple Silicon from day one and Intel Macs back to roughly 2013.
* **Windows** requires **Windows 7 or newer**. The x64 installer refuses to run on 32-bit Windows; the x86 one runs on both.
* The **unix tarballs are the command-line engine only** (`bin/spfy_synth` plus `bin/spfy_update`). SAPI is a Windows COM interface, so "Refresh SAPI Voices", Balabolka and Narrator are Windows-only - on Linux and macOS you drive `spfy_synth` directly, as in the example below.

3. Download any voices you want to use. The voices can be downloaded individually, or in packs per FE language [at this page](https://github.com/wagwan-piffting-blud/Speechify/releases/tag/voices).
4. Unpack the voices to your Speechify Documents folder (usually `C:\Users\{your_username}\Documents\Speechify\{lang_code}\{voice_name}`). On Linux or macOS, unpack to your home directory or wherever you want, and set the `SPFY_VOICE_DIR` environment variable to point to the folder containing the voices.

For Windows users ONLY:

5. Run "Refresh SAPI voices" from the Start Menu to make the voices available in SAPI and Balabolka.
6. Run your SAPI client of choice (Balabolka, TTSApp, etc.) and select the voice you want to use. You can also use the `spfy_synth.exe` command-line tool to synthesize audio directly from spfy. NOTE: If you use spfy_synth.exe, you MUST use it from OUTSIDE the Program Files folder, as it will not work from inside that folder due to Windows permission restrictions. Specify the full path to it from the command line running from another folder (i.e. your Desktop, Documents, etc.), like so (note the quotes around the path to the executable and all other arguments, this is because of the space in "Program Files" and to ensure everything is passed correctly to the executable):

```sh
cd C:\Users\{your_username}\Desktop
"C:\Program Files\Speechify\bin\spfy_synth.exe" "tom" "Hello, world!" "hello_world.wav"
```

Other platforms (Linux, macOS) do not have this restriction, so you can run `spfy_synth` from anywhere, so long as you point it to the correct voice directory (either via `SPFY_VOICE_DIR` or by specifying the full paths to the voice file triplet in the order vin, vdb, vcf).

---

## The spfy_dumpwav.exe Command Line Tool (for the original Speechify.exe 3.0.5 engine)

`spfy_dumpwav.exe` is a lightweight command-line synthesis tool that talks directly to the Speechify server. It does not require Balabolka, SAPI, or any GUI, just the running `Speechify.exe` backend. It supports text-to-speech, phoneme input/output, and format conversion.

**Note: The Speechify server (`bin/Speechify.exe`) MUST be running before using this tool. If it is not, it will fail.**

### Basic Synthesis, using the current voice configured in `config\SWIttsConfig.xml`

```sh
spfy_dumpwav.exe "Hello, world!" output.wav
spfy_dumpwav.exe --16k "Hello, world!" output_16k.wav
```

The default output is 8kHz 16-bit PCM WAV. Use `--16k` for 16kHz output.

### Phoneme Timing Output

```sh
spfy_dumpwav.exe --phonemes "The weather today." output.wav
```

Creates `output.wav` plus `output.phn` with per-phoneme timing:

```text
0       192     pau     0
192     368     dh      0
368     776     ix      0
776     1256    w       1
1256    2040    eh      1
...
```

Format: `start_sample  end_sample  phoneme  stress` (tab-separated).

### Phoneme Input (SPR Format)

Synthesize directly from phoneme codes using Speechify's SPR (Symbolic Phonetic Representation) format:

```sh
spfy_dumpwav.exe --pron ".1hE.0lo" output.wav
```

SPR codes are case-sensitive single characters. You can also mix text and inline phonemes:

```sh
spfy_dumpwav.exe "I went to \![.1pa.0tx.0wa.0tu.0mi] county." output.wav
```

### Text-to-Phoneme (G2P)

Get the engine's phoneme breakdown for any text without producing audio:

```sh
spfy_dumpwav.exe --g2p "Pottawattamie"
```

Outputs both ARPAbet and SPR representations.

### Offline operations (no server, no socket, no `SWItts.dll`)

Four flags are pure text transforms and run before the client touches anything. They work with Speechify stopped:

```sh
spfy_dumpwav.exe --bal2spr "p aa 1 t ax w aa t uw m iy"
spfy_dumpwav.exe --spr2bal ".1pa.0tx.0wa.0tu.0mi"
spfy_dumpwav.exe --expand 'Take cover <break time="0.5s"/> now.'
spfy_dumpwav.exe --help
```

Balabolka format uses space-separated ARPAbet codes with stress markers after vowels. SPR format uses single-character symbols with syllable/stress markers.

`--expand` prints the `\!` codes a text lowers to, through the same expander synthesis uses - the way to see why a `<pron>`, `<prosody>` or `<say-as>` did not come out as expected, without a server and without waiting for audio. It accepts `-f FILE`.

WARNING: `--g2p` is **not** offline. The grapheme-to-phoneme tables live in the server's front end, so it needs Speechify running.

### SPR Symbol Reference

| Type | SPR Symbol = ARPAbet |
|------|---------------------|
| Vowels | `a`=aa `A`=ae `H`=ah `c`=ao `W`=aw `x`=ax `Y`=ay `i`=iy `I`=ih `e`=ey `E`=eh `R`=er `u`=uw `U`=uh `o`=ow `X`=ix `O`=oy |
| Consonants | `p b t d k g f v s z m n l r w y` (same as ARPAbet) |
| Consonants | `C`=ch `J`=jh `T`=th `D`=dh `S`=sh `Z`=zh `G`=ng `N`=en `F`=dx `h`=hh |
| Stress | `1`=primary `2`=secondary `0`=none |
| Syllable | `.` (period marks syllable start) |

### All Options

| Flag | Description |
|------|-------------|
| `--phonemes` | Write `.phn` phoneme timing file alongside WAV |
| `--pron "..."` | Synthesize from SPR phoneme string (no text needed) |
| `--g2p` | Print phoneme sequence for text (no audio output) |
| `--16k` | Use 16kHz output (default: 8kHz) |
| `--rawdump` | Dump raw callback bytes to stderr (diagnostic) |
| `--bal2spr "..."` | Convert Balabolka phonemes to SPR format - **offline** |
| `--spr2bal "..."` | Convert SPR phonemes to Balabolka format - **offline** |
| `--expand "..."` | Print the `\!` codes a text expands to - **offline**, accepts `-f FILE` |
| `--help`, `-h` | Usage and the SPR symbol table - **offline** |

---

## Note on "CRS Mara"

"CRS Mara" (and "CRS Tom") are fully custom voices created by me using Claude Code and uses the Speechify TTS engine, which has been fully reverse-engineered (see the `reveng/` folder). They are not official SpeechWorks voices, but they are included in this Speechify 3.0 package. The voices are based on the original "Mara" voice that was available in older versions of Speechify that are now presumed lost media, but they have been generated to work with this version of the Speechify TTS engine. If you know where the True Mara voice is located (usually on Microsoft Speech Server 2004 Beta 1/2), please [contact me](https://wagspuzzle.space/mara) as I would love to add it to this package and not use CRS Mara at all. Same with Craig/AI Craig.

## Credits

DLL patching work code done by Wags (@wags2piffting on Discord, or [visit my website](https://wagspuzzle.space/)), and spfy_dumpwav.exe code made with the help of Claude Code. Original voice data and technology by SpeechWorks International. Credits to SpeechWorks International for creating the TTS engine, and whoever the original creator of the Speechify VM is (previously the only way to run Speechify Tom/Jill). Now we can _all_ enjoy not only Tom, but other Speechify voices on modern Windows systems. As well, credits to the Balabolka team for making a great TTS frontend that works well with various TTS engines.

## GenAI Disclosure Notice: Portions of this repository have been generated using Generative AI tools (Claude Code, GitHub Copilot, Google Gemini).
