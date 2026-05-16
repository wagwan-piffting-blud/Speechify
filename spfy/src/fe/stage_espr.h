#ifndef SPFY_FE_STAGE_ESPR_H
#define SPFY_FE_STAGE_ESPR_H

#include "fe.h"

/* Stage 6 (alt): ESPR text emitter.
 *
 * The FE-USel boundary in the original Speechify engine is text-based
 * Enhanced Speechify Phonetic Representation (ESPR). The engine's
 * ESPRparser class consumes this text and runs USel + WSOLA. Our
 * Path-B FE re-implementation emits ESPR directly, so its output can
 * be handed to the original engine's SWIttsSpeakEx (with content type
 * `application/x-swi-espr`) for end-to-end text->WAV.
 *
 * ESPR grammar (decoded from ESPRparser methods in SWIttsEngine.dll):
 *
 *   ESPR_doc    ::=  '%%'?  '#'?  '{'  PhraseBody  '}'  '%%'?
 *   PhraseBody  ::=  Pause*  ( Word | Pause )*
 *   Pause       ::=  'pau(' [int ','] 'p' float ')'
 *   Word        ::=  '<' NormText [VolRate] PhraseType GramCat SylPhones '>'
 *   NormText    ::=  <chars-up-to-space>
 *   VolumeRate  ::=  '(' Tag (',' Tag)* ')'    Tag = 'v'<n> | 'r'<n>
 *   SylPhones   ::=  '[' Item* ']'
 *   Item        ::=  Phoneme | Syllable | F0Value
 *   Phoneme     ::=  ARPAbetName '(p' float ')'
 *   Syllable    ::=  '.' int (',' vowel_chars)? (';' coda_chars)?
 *   F0Value     ::=  '(' float ',' float ')'
 *
 * Wrapper escape codes (per .rdata at SWIttsEngine.dll):
 *   \!SWIespr0 ... \!SWIespr1   = ESPR block markers
 *
 * Phoneme names are CMU/ARPAbet 2-letter codes (aa/ae/ah/.../th/sh/ng).
 * Our LTS emits 1-char SAMPA-style IDs; we translate at emit time.
 *
 * Limitations of first iteration (deferred to follow-ups):
 *   - PhraseType / GramCat output omitted (those parsers' grammar
 *     wasn't decoded; parser may tolerate empty, may need stubs).
 *   - VolumeRate only emitted when prosody hints request it.
 *   - F0 contour values not yet emitted (TBD when CART trees are
 *     consulted for F0 targets).
 */

/* Emit ESPR text for the parsed-and-prosody-tagged delta into a
 * caller-supplied buffer. Returns SPFY_OK on success or SPFY_E_OOB
 * if the buffer would overflow (caller should retry with larger).
 *
 * On success, *out_len is set to the number of bytes written
 * (excluding NUL terminator). The buffer is NUL-terminated.
 */
int  spfy_fe_espr_emit(const spfy_fe_t *fe,
                       const char       *original_text,
                       spfy_fe_delta_t  *delta,
                       char             *buf,
                       size_t            buf_n,
                       size_t           *out_len);

#endif
