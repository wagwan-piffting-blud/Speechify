#ifndef SPFY_VB_VCF_H
#define SPFY_VB_VCF_H

#include <stddef.h>
#include <stdint.h>

/* The voice's two config files, WRITTEN rather than copied.
 *
 * ⭐ WHY THIS EXISTS. Every arm shipped a byte-for-byte copy of jill.vcf. It
 * never appeared in a donor audit because it is not part of the VIN or VDB,
 * yet it sets the weight of every cost term the selector uses -- a larger
 * influence on the voice than `ccos`, which the audit does flag.
 *
 * ⚠ WHAT CHANGES AND WHAT DOES NOT. A `.vcf` is ISO-8859-1 XML behind a 2:1
 * nibble cipher, so "generating" it means writing the plaintext we hold in
 * vb_vcf_enus.c and enciphering it. That makes every weight legible, diffable
 * and overridable from the command line. It does NOT make the numbers ours:
 * they are the donor's tuning, now a NAMED DEFAULT instead of an opaque blob.
 * Saying otherwise would be the exact thing the ownership rule is against.
 *
 * The identity lives in the SIDECAR, `<voice>8.xml`, which is plain XML.
 * Measured: jill's and crsmara's differ only in `tts.server.port` and
 * `tts.voice.name`. */

/* Generated (vb_genvcf.py). Plaintext XML; the cipher is applied on write. */
const char *spfy_vb_vcf_plaintext(void);
const char *spfy_vb_sidecar_template(void);

/* One `tts.voiceCfg.KEY=VALUE` override, applied to the plaintext before it is
 * enciphered. `key` is matched WITHOUT the `tts.voiceCfg.` prefix. */
typedef struct {
    const char *key;
    const char *value;
} spfy_vb_vcf_set;

/* Writes `<dir>/<voice>.vcf`. Returns SPFY_E_FORMAT if an override names a
 * key the template does not contain -- a typo that silently did nothing would
 * be indistinguishable from a weight that is simply inert. */
int spfy_vb_vcf_write(const char *dir, const char *voice,
                      const spfy_vb_vcf_set *sets, size_t n_sets,
                      size_t *n_applied);

/* Writes `<dir>/<voice><format>.xml`, the sidecar the engine reads first. */
int spfy_vb_sidecar_write(const char *dir, const char *voice,
                          int format, const char *language, int port);

#endif
