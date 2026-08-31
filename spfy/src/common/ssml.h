/* SSML -> embedded-tag translation, below every front end.
 *
 * WHY A TRANSLATOR AND NOT A PARSER. The engine already owns a complete,
 * tested inline-markup pipeline: spfy_etags_resolve() in spfy_synth.c reads
 * the Speechify User's Guide `\!` tags (volume, rate, spellout, year, ToBI)
 * into per-character maps, and build_inline_mixed_tagged() splices `\![SPR]`,
 * `\!pN` pauses and `<pron sym=...>` into one flowing tagged utterance. All
 * of that is reached from the SHARED synth path, so it already works in the
 * CLI, in SAPI, in the WASM build and in the web demo.
 *
 * What did NOT reach that path was SSML. `<prosody>`, `<break>`, `<emphasis>`
 * and friends were handled only by SAPI (which parses the XML itself and
 * hands the engine SPVSTATE fragments) and, behind SPFY_FE_INTERNAL=1, by
 * src/text_norm/text_norm.c. On a default `spfy_synth` run the tags were not
 * merely ignored -- they were SPOKEN:
 *
 *     spfy_synth tom '<speak>Hello <prosody rate="x-slow">slow world
 *                     </prosody>.<break time="800ms"/> Done.</speak>'
 *     -> "speakhello prosody rate equals eks slow greater than slow world
 *         slash prosody break time equals eight hundred milliseconds ..."
 *
 * So this module rewrites SSML into the dialect the engine already speaks,
 * and everything downstream is unchanged. One new tag family was needed --
 * `\!pp<N>` / `\!pd<N>` for pitch, alongside the guide's `\!vp`/`\!rp` --
 * because volume and rate had per-character maps and pitch did not.
 *
 * NOT a validating XML parser. Malformed input degrades to "strip the markup,
 * keep the text", which is what every other TTS front end does and what a
 * user pasting half a document expects.
 */

#ifndef SPFY_COMMON_SSML_H
#define SPFY_COMMON_SSML_H

#ifdef __cplusplus
extern "C" {
#endif

/* Does `text` carry an SSML element this translator understands?
 *
 * Deliberately NARROW. It matches a known element name followed by a real tag
 * boundary, so prose containing `a < b` or `<pron sym="...">` is left alone --
 * `<pron>` in particular is an EXISTING engine feature handled downstream by
 * build_inline_mixed_tagged(), and swallowing it here would break it. */
int spfy_ssml_detect(const char *text);

/* Rewrite SSML into the engine's embedded-tag dialect.
 *
 * Returns a malloc'd NUL-terminated string the caller frees, or NULL on
 * allocation failure (the caller should then fall back to the input text
 * rather than fail the synth). */
char *spfy_ssml_to_etags(const char *ssml);

/* Percent <-> semitones, the conversion the `\!pp`/`\!pd` pitch tag rides on.
 * Exposed because the producer (this file) and the consumer (spfy_synth.c's
 * etag_pitch -> hp_pitch_st step) must agree exactly, and a silent drift
 * between two copies of `12 * log2(p / 100)` is invisible in the audio. */
int spfy_ssml_pct_to_semitones(int pct);
int spfy_ssml_semitones_to_pct(int st);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_COMMON_SSML_H */
