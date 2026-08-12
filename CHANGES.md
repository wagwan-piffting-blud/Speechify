# Speechify Changelog

2026-08-11:

- Initial release of the Speechify project changelog. This is similar to other changelogs I maintain in my other repositories (like EAS Tools).

- Comment cleanup across the spfy re-implementation engine source. Reverse-engineering references (Ghidra addresses, DLL struct offsets) and hazard notes are **kept verbatim**; commentary that restated the code has been removed and long prose truncated to its point.

- Introduction of byte-for-byte parity with the original Speechify engine in spfy. _MOST_ changes in the changelog will revolve around this engine, as it is the only one we have official source code for (because, well, we made it ourselves). The original Speechify 3.0.5 engine is, and will always remain, available for software preservation purposes.

- Introduction of Speechify 4 mode in spfy, which adds pitch modification to the synthesized speech via LP-PSOLA (Linear Predictive Pitch Synchronous Overlap and Add), which aims to produce a more natural-sounding (and closer to NOAA Weather Radio) voice. This mode is **still in active development** and is not yet considered stable enough for "mock" use. Note that audio produced in Speechify 4 mode may not be correct-sounding for certain, untested phrases or whole bulleting. If something sounds completely incorrect (specific phrases/words) or audio synthesis fails entirely/is broken under Speechify 4 mode, please report it in the repository issue tracker. More information on Speechify 4 mode is available in [SPEECHIFY_4_FINDINGS.md](SPEECHIFY_4_FINDINGS.md).

- spfy now loads **16 kHz voice databases** as well as 8 kHz ones. SpeechWorks seemingly shipped Speechify 3.0 at both rates, and the engine has always had a native 16 kHz path. Storage is chosen from the VDB's `wFormatTag`: 7 for 1-byte u-law, 1 for 16-bit PCM, and never from `blockAlign`/`bitsPerSample`, which the 8 kHz database misreports. Output rate and the WSOLA window follow the database, so durations are identical in milliseconds at either rate. Unsupported formats are now refused at load instead of being decoded into noise. Byte-for-byte parity on the 8 kHz path is unchanged with the current corpus.
