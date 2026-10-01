/* Minimal JSON parser for request bodies.
 *
 * Scope is deliberately small: objects, strings, integers. The API only ever
 * accepts {"url": string, "custom_code": string, "ttl_days": integer}, and a
 * parser that understands nothing else is a parser with fewer bugs.
 *
 * The contract that matters: json_parse returns NULL on anything it does not
 * fully understand. It never guesses, never returns partial results, and never
 * reads past len. Input is attacker-controlled, so "reject what I cannot prove
 * is valid" is the only safe default.
 */
#ifndef UTIL_JSON_H
#define UTIL_JSON_H

#include <stddef.h>

typedef struct json_obj json_obj_t;

/* Parses a complete JSON object. Trailing non-whitespace is an error, so a
 * truncated body padded with junk cannot pass. Returns NULL on any malformed
 * input, unsupported type, or allocation failure. */
json_obj_t *json_parse(const char *src, size_t len);

void json_free(json_obj_t *o);

/* Returns the string value for key, or NULL when absent, of another type, or
 * when o is NULL. The pointer stays valid until json_free. */
const char *json_get_str(const json_obj_t *o, const char *key);

/* Writes the integer value for key. Returns 0 on success, -1 when the key is
 * absent or holds a non-integer. */
int json_get_int(const json_obj_t *o, const char *key, long *out);

/* Number of keys in the object, for tests and logging. */
size_t json_count(const json_obj_t *o);

#endif /* UTIL_JSON_H */
