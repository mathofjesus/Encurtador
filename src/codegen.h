/* Base62 short codes.
 *
 * A code is a fixed-width, zero-padded rendering of the row id. That makes the
 * length a consequence of arithmetic rather than taste:
 *
 *   62^6 = 56,800,235,583   too small for ten years at 100M/day (365e9)
 *   62^7 = 3,521,614,606,192 comfortably larger
 *
 * so CODE_MIN_LENGTH is 7. The padding is deliberate: every code has the same
 * length, which keeps index locality and lets a prefix range delete a whole
 * partition's worth of codes at once.
 *
 * The scheme is one-to-one. Encoding then decoding returns the original id, and
 * that is what lets the redirect path answer from the id alone.
 */
#ifndef CODEGEN_H
#define CODEGEN_H

#include <stddef.h>

#define CODE_ALPHABET \
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define CODE_BASE 62

/* Seven is the minimum ten years of 100M codes/day requires. */
#define CODE_MIN_LENGTH 7
/* Twelve covers 62^12 ids with room to spare and bounds the DB column. */
#define CODE_MAX_LENGTH 12
/* A client-chosen code only has to be memorable, so it may be shorter than a
 * generated one. Four characters is the floor: below that a scan would hit a
 * collision quickly. */
#define CODE_CUSTOM_MIN_LENGTH 4

/* Writes the padded code for id into out, which must hold at least
 * CODE_MAX_LENGTH + 1 bytes. Returns the number of characters written, or -1
 * when out is too small. Writes nothing on failure: a truncated code would be
 * a valid-looking code for a different id. */
int code_encode(unsigned long long id, char *out, size_t out_len);

/* Decodes a code back to its id. Returns 0 on success, -1 for a NULL or empty
 * code, a character outside the alphabet, or a value that overflows
 * unsigned long long. */
int code_decode(const char *code, unsigned long long *out);

/* Whether a code is well-formed as a *generated* code: between
 * CODE_MIN_LENGTH and CODE_MAX_LENGTH characters, all from the alphabet. Used
 * to validate a code arriving from a URL before it ever reaches a database. */
int code_is_valid(const char *code);

/* Whether a string is acceptable as a *custom* code chosen by a client.
 * Custom codes may be shorter than generated ones, from CODE_CUSTOM_MIN_LENGTH
 * up. Kept separate from code_is_valid because mixing the two either rejects
 * legitimate custom codes or lets malformed generated ones through. */
int code_is_valid_custom(const char *code);

#endif /* CODEGEN_H */
