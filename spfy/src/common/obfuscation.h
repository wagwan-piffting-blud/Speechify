#ifndef SPFY_COMMON_OBFUSCATION_H
#define SPFY_COMMON_OBFUSCATION_H

#include <stddef.h>
#include <stdint.h>

/* SWIttsRiffEncryption modes (from SWIttsEngineUtil.dll):
 *   NONE = 0  -- payload is plain RIFF
 *   CE   = 1  -- every byte byte-flipped against constant 0xCE
 *
 * Confirmed via DLL disassembly: instruction at 0x06b42197 byte-flips
 * each payload byte against 0xCE. Symmetric: same op obfuscates and
 * deobfuscates. */
#define SPFY_OBFUSCATION_BYTE 0xCE

void spfy_unobfuscate_ce      (uint8_t *buf, size_t n);
void spfy_unobfuscate_ce_copy (uint8_t *dst, const uint8_t *src, size_t n);

#endif
