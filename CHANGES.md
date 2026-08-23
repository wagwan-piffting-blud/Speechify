# Speechify/spfy Changelog

## 2026-08-22

- **spfy now tells you when there is a newer engine, or a rebuilt version of a voice you already have.** It checks at most once a week, and only when you actually use the engine - there is no background service, no scheduled task and nothing running when you are not synthesizing. `spfy_synth --check-update` asks immediately; `spfy_update --status` shows what it knows; `spfy_update --disable`, `--no-update-check` or `SPFY_NO_UPDATE_CHECK=1` turn it off, as does clearing the "Check for engine and voice updates" box in the installer (which switches it off for every account on that machine).

  - Voices are compared by **content**, not by a version string: the manifest carries each file's size and SHA-256, sizes are checked first and the hash only when they match, and a computed hash is cached against the file's size and mtime - so a 96 MB VDB is read once per rebuild, not once per check. That is what makes "CRS Mara and CRS Tom will keep being rebuilt" a thing the update check can actually notice.

  - **The SAPI voice never checks for updates itself.** It runs in-process inside Narrator and Balabolka, so on the first `Speak` it stats one small file and, at most once per process, starts a detached helper. No network call, no hashing and no window ever happens on a screen reader's thread, and the result arrives as a tray balloon that takes no focus. Verified: rendering "The quick brown fox jumps over the lazy dog." through Tom with the check firing is byte-identical to rendering it with the check disabled, and both match the CI reference hash.

- **Voices are now published as release assets**, one zip per voice plus one per language, on the rolling `voices` tag. Per voice rather than per language because a rebuilt CRS Tom is then a 107 MB download instead of the 413 MB the whole en-US bundle costs. Every zip already contains the `<lang>\<voice>\` folders, so unzipping it into `%USERPROFILE%\Documents\Speechify\` puts each file exactly where the SAPI scan looks. See [installer/updates/README.md](installer/updates/README.md).

- **Paulina is downloadable again without git-lfs.** Her 264 MB VDB is over GitHub's per-file push limit, which is why `es-MX/paulina/` is not in the repo - but that limit applies to files pushed *into* a repository, not to release assets, which are not git objects. Her zip is packed on a machine that has the files and published like any other; `installer/updates/external_voices.json` (1.7 KB of hashes, committed) is what lets CI carry the entry forward without repacking her. A language bundle that would be incomplete without her is not rebuilt at all, and the publish fails outright if any manifest URL names an asset nobody uploaded.

- Binaries now know what version they are. `spfy_synth --version` prints the calver CI stamped in, or `dev-<sha>` for a build made by hand - and a `dev-` build is never told about an engine release, because a working copy is by definition ahead of the newest one.

- Overall engine and WASM deployment updates. Also backport the Inno installer for spfy to as far back as Windows 7. The Inno installer is the recommended way to install spfy/Speechify voices on Windows overall.

- Fix build error in WASM/Windows builds due to the recent changes made last commit. Non-CI local testing can only catch so much...

- CRS Tom is now available for use in both Speechify and spfy. See the note below for CRS Mara for more info.

- CRS Mara has been fixed to work in both Speechify and spfy reliably and more correctly than the previous build. [See the rationale for the fix here](https://github.com/wagwan-piffting-blud/Speechify/blob/a64b342f5230ccd0724ef7713496c8103e90bea9/en-US/crsmara/README.md#L228-L256). In short, the previous build of CRS Mara was not fully compatible with the Speechify base engine due to spfy_voice_vb generating a malformed container. The new build of CRS Mara has been fixed to be fully compatible with the Speechify base engine and should work correctly in both Speechify and spfy. CRS Tom revealed this issue when it was being built, and the fix has been applied to both CRS Mara and CRS Tom. If you downloaded CRS Mara before this fix, you will need to re-download the VIN **ONLY** to get the fix. If the voice crashes in Speechify, that is why. Tom should not have this issue (nor should Mara any longer), but if either do, please report it in the repository issue tracker. Thank you!

---

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
