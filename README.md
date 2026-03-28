# Speechify 3 Voices on Modern Windows (tested on Windows 11 25H2 x64)

## Installation Instructions
Step 1: Choose a place to keep your Speechify install. This can be anywhere on your computer, but it's best to keep it in a dedicated folder. For example, you might create a folder called "Speechify" in your user Documents directory. This is what I did personally and works well.

Steps 2 and 3 (For Balabolka/TTS App/SAPI users): Right-click the "BalRegisterVoice.bat" file included in this folder and select "Run as Administrator" to do all the manual setup steps automatically. You should see a command prompt window with some helpful messages pop up. Once it finishes successfully, you can close the command prompt window or hit any key to exit. MAKE SURE YOU RUN THIS AS ADMINISTRATOR, OR IT WILL FAIL SILENTLY. NOTE: If the batch file fails, you can open a command prompt, navigate to this folder (`cd \Users\USERNAME\Documents\Speechify`, for example), and run the batch file from there. This way, you can see any error messages that may help diagnose the issue. If you do run into any issues, contact @wags2piffting on Discord or visit [https://wagspuzzle.space/contact/](https://wagspuzzle.space/contact/). However, most users report no issues when running the batch file as administrator CORRECTLY.

Step 4: Run Speechify.exe. This is the backend server to make Speechify work at all. You can find it in the `bin` folder in this repository. **This step is REQUIRED to use ANY Speechify voices.** You can create an autorun/Task Scheduler entry for it if you want it to start automatically with Windows. Running it silently is possible, but outside the scope of this README. If you get a Windows firewall prompt asking for permission to allow Speechify.exe to communicate on private/public networks, make sure to allow it on at least private networks (home/work). This is necessary for the TTS frontend (e.g., Balabolka) to communicate with the Speechify backend server.

Step 5: Open your TTS frontend (e.g., Balabolka) and select the "Speechify Tom" entry as your voice. You should now be able to use Speechify 3.0 to convert text to speech. You can also use the command line tool "spfy_dumpwav.exe" to dump audio files without the overhead of the Balabolka GUI (example: `spfy_dumpwav.exe "This is the text you want the voice to say" output.wav`). IMPORTANT NOTE: The registry keys only _say_ "Tom", but all other Speechify voices work under the Tom registry key. You just need to edit the "SWIttsConfig.xml" file in the config folder to switch voices. Have fun using Speechify voices on modern Windows!

## Notes
- Make sure to run Speechify.exe **every time** you want to use Speechify voices. You can set it to run automatically at startup if you prefer (look up a guide on Task Scheduler in Windows).
- If you encounter any issues, double-check that you have followed all the steps correctly. Admin access is REQUIRED for the batch file to work, and you must run Speechify.exe for the voices to work at all due to the server/client architecture of Speechify.
- This setup is specifically tested on Windows 11 25H2 x64, but it should work on many other versions of Windows as well. However, they have not been tested, so your mileage may vary.
- To switch voices, simply edit the "SWIttsConfig.xml" file in the config folder and change the "tts.voice.name" and "tts.voice.language" parameters to your desired voice and language. __DO NOT CHANGE ANY OTHER PARAMETERS IN THIS FILE__. Then, restart Speechify.exe for the changes to take effect. You MUST restart the backend server for the changes to apply, as it only reads the config file on startup. The available voices (and their languages) are:

  - Tom (en-US)
  - AI Mara (en-US)
  - Jill (en-US)
  - Felix (fr-CA)
  - Javier (es-MX)
  - Paulina (es-MX)

Demos of what each voice sounds like are available in the "demos" folder in this repository.

---

## The spfy_dumpwav.exe Command Line Tool

`spfy_dumpwav.exe` is a lightweight command-line synthesis tool that talks directly to the Speechify server. It does not require Balabolka, SAPI, or any GUI, just the running `Speechify.exe` backend. It supports text-to-speech, phoneme input/output, and format conversion.

**Note:** The Speechify server (`bin/Speechify.exe`) must be running before using this tool.

### Basic Synthesis

```
spfy_dumpwav.exe "Hello, world!" output.wav
spfy_dumpwav.exe --16k "Hello, world!" output_16k.wav
```

The default output is 8kHz 16-bit PCM WAV. Use `--16k` for 16kHz output.

### Phoneme Timing Output

```
spfy_dumpwav.exe --phonemes "The weather today." output.wav
```

Creates `output.wav` plus `output.phn` with per-phoneme timing:

```
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

```
spfy_dumpwav.exe --pron ".1hE.0lo" output.wav
```

SPR codes are case-sensitive single characters. You can also mix text and inline phonemes:

```
spfy_dumpwav.exe "I went to \![.1pa.0tx.0wa.0tu.0mi] county." output.wav
```

### Text-to-Phoneme (G2P)

Get the engine's phoneme breakdown for any text without producing audio:

```
spfy_dumpwav.exe --g2p "Pottawattamie"
```

Outputs both ARPAbet and SPR representations.

### Phoneme Format Conversion

Convert between Balabolka/Balcon phoneme format and SPR format (no server needed):

```
spfy_dumpwav.exe --bal2spr "p aa 1 t ax w aa t uw m iy"
spfy_dumpwav.exe --spr2bal ".1pa.0tx.0wa.0tu.0mi"
```

Balabolka format uses space-separated ARPAbet codes with stress markers after vowels. SPR format uses single-character symbols with syllable/stress markers.

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
| `--bal2spr "..."` | Convert Balabolka phonemes to SPR format |
| `--spr2bal "..."` | Convert SPR phonemes to Balabolka format |
| `--16k` | Use 16kHz output (default: 8kHz) |
| `--rawdump` | Dump raw callback bytes to stderr (diagnostic) |

### Building from Source

Requires Microsoft Visual C++ (any version with `cl.exe`):

```
cd bin
cl spfy_dumpwav.c /Fe:spfy_dumpwav.exe
```

The only dependency is `swi_min.h` (included) and `SWItts.dll` (in the bin folder).

This step should not be required for most users, however, as I have included a precompiled binary in the `bin` folder, but it's here if you want to build it yourself or make your own modifications to it. The source code is also included in the `bin` folder as "spfy_dumpwav.c". This tool is open-source and licensed under the GNU GPL 3 (see [LICENSE](./LICENSE)), so feel free to modify and use it as you see fit.

---

## Note on "AI Mara"

"AI Mara" is a fully custom voice created by me using Claude Code and uses the Speechify TTS engine, which has been fully reverse-engineered (see the `reveng/` folder). It is not an official SpeechWorks voice, but it is included in this Speechify 3.0 package. The voice is based on the original "Mara" voice that was available in older versions of Speechify that are now presumed lost media, but it has been generated to work with this version of the Speechify TTS engine. If you know where the True Mara voice is located (usually on Speech Server 2004 Beta 1/2), please [contact me](https://wagspuzzle.space/mara) as I would love to add it to this package and not use the AI Mara at all.

## Credits
DLL patching work code done by Wags (@wags2piffting on Discord, or visit my website at https://wagspuzzle.space/), and spfy_dumpwav.exe code made with the help of Claude Code. Original voice data and technology by SpeechWorks International. Credits to SpeechWorks International for creating the TTS engine, and whoever the original creator of the Speechify VM is (previously the only way to run Speechify Tom/Jill). Now we can _all_ enjoy not only Tom, but other Speechify voices on modern Windows systems. As well, credits to the Balabolka team for making a great TTS frontend that works well with various TTS engines.

## GenAI Disclosure Notice: Portions of this repository have been generated using Generative AI tools (Claude Code, GitHub Copilot, Google Gemini).
