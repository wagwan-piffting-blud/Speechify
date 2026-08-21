# Speechify/spfy Changelog

## 2026-08-21

- CRS Mara is now available for use in both Speechify and spfy (yes, it works in both engines just fine, though don't expect byte-identical parity with this specific voice!). This is a new voice that is partially made with real CRS-era direct synthesis from the NOAA Weather Radio archived webpages on Internet Archive, and partially with synthesis from [StyleTTS 2](https://github.com/yl4579/styletts2), a neural TTS engine known for being able to generate high-quality speech. It is a more natural-sounding voice than the original AI Mara, and is intended to be a replacement for it. Give it a try and let me know what you think! Feedback is always welcome. CRS Tom is Coming Soon™. The old AI Mara voice has been deleted, if you REALLY want it back, you can find it in the git history, but it is now considered **deprecated**.

---

## 2026-08-12

- Speechify 4 mode now works on **every** platform, not just the Windows CLI and SAPI. In the WASM builds, `\!s4m` used to turn itself on, and then synthesize as plain 3.0.5, which is the worst possible failure mode: no error, just plain, incorrect audio. Two things were missing on both: the loaded VDB's path was never recorded, so the prosody stage had nothing to derive the pitch-mark filenames from, and the pitch marks themselves (`tom8.pmdata` / `tom8.pmindex`) were never shipped with the voice. Both are fixed, and the pitch marks are staged as *optional* files so voices that have none still ship.

- **The emulator can now switch language mid-process.** `spfy_dll_emu_boot` returned early whenever *anything* was already booted, so loading a second voice in another language silently kept running through the **first** voice's front end - Spanish text phonemized by the English FE, no error anywhere. It now records which image is mapped and re-boots on a different one; `mem_init` / `cpu_reset` / `win32_reset` already reset every piece of guest state, so only the guard had to go. A re-boot frees all guest memory, so the previous FE must be closed first - `spfy_voice_free()` does, and that ordering is now the documented contract.

- Add and ship a new and improved AI Mara voice. This is very similar to the existing AI Mara cloned voice, but uses Jill as a base instead of Tom, and has a much more natural and realistic-sounding pitch contour and timbre. The new AI Mara voice is available for both Speechify and spfy usage. Give it a try and let me know what you think! Feedback is always welcome. The old AI Mara voice is still available for use, but it is now considered deprecated.

---

## 2026-08-11

- Initial release of the Speechify project changelog. This is similar to other changelogs I maintain in my other repositories (like EAS Tools).

- Comment cleanup across the spfy re-implementation engine source. Reverse-engineering references (Ghidra addresses, DLL struct offsets) and hazard notes are **kept verbatim**; commentary that restated the code has been removed and long prose truncated to its point.

- Introduction of byte-for-byte parity with the original Speechify engine in spfy. _MOST_ changes in the changelog will revolve around this engine, as it is the only one we have official source code for (because, well, we made it ourselves). The original Speechify 3.0.5 engine is, and will always remain, available for software preservation purposes.

- Introduction of Speechify 4 mode in spfy, which adds pitch modification to the synthesized speech via LP-PSOLA (Linear Predictive Pitch Synchronous Overlap and Add), which aims to produce a more natural-sounding (and closer to NOAA Weather Radio) voice. This mode is **still in active development** and is not yet considered stable enough for "mock" use. Note that audio produced in Speechify 4 mode may not be correct-sounding for certain, untested phrases or whole bulleting. If something sounds completely incorrect (specific phrases/words) or audio synthesis fails entirely/is broken under Speechify 4 mode, please report it in the repository issue tracker. More information on Speechify 4 mode is available in [SPEECHIFY_4_FINDINGS.md](SPEECHIFY_4_FINDINGS.md).

- spfy now loads **16 kHz voice databases** as well as 8 kHz ones. SpeechWorks seemingly shipped Speechify 3.0 at both rates, and the engine has always had a native 16 kHz path. Storage is chosen from the VDB's `wFormatTag`: 7 for 1-byte u-law, 1 for 16-bit PCM, and never from `blockAlign`/`bitsPerSample`, which the 8 kHz database misreports. Output rate and the WSOLA window follow the database, so durations are identical in milliseconds at either rate. Unsupported formats are now refused at load instead of being decoded into noise. Byte-for-byte parity on the 8 kHz path is unchanged with the current corpus.

- Update hash to parity with Speechify 3.0.5 engine for the reference text "The quick brown fox jumps over the lazy dog." The new hash is `86dde7edb10eb9246ae997f70742cc2f1320de30f5fcb87412a052596bae0bdb`. This hash is used to verify that the engine produces the same output as the original Speechify engine for this reference text.

- Update WASM build to succeed under CI.
