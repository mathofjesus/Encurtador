/* Structured logging: one JSON object per line.
 *
 * JSON rather than a formatted sentence because the intended consumer is a log
 * collector, and a collector parses fields. "GET /abc1234 -> 301 in 0.4ms" has
 * to be re-parsed with a regex that breaks the first time a path contains a
 * space; the same event as an object does not.
 *
 * The writer is synchronous and single-threaded. The reactor is one thread; if
 * requests ever fan out to workers, this needs a lock, and it is called out here
 * so it is not discovered under load.
 *
 * Nothing here allocates per call beyond a small scratch buffer, and a write
 * failure is dropped rather than reported. A logging path that can fail the
 * request it is logging is worse than a missing line.
 */
#ifndef OBSERVABILITY_LOG_H
#define OBSERVABILITY_LOG_H

#include <stddef.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_LEVEL_COUNT
} log_level_t;

typedef struct log log_t;

/* Writes to fd. min_level is the lowest level that reaches the output; records
 * below it are dropped before any formatting happens. Returns NULL only on
 * allocation failure. */
log_t *log_new(int fd, log_level_t min_level);

/* Safe on NULL. Does not close fd: the caller opened it. */
void log_free(log_t *l);

void log_set_level(log_t *l, log_level_t min_level);
log_level_t log_level_from_string(const char *s);

/* "debug", "info", "warn", "error"; never NULL. */
const char *log_level_name(log_level_t level);

/* One record. Every string field is optional: NULL omits the key rather than
 * writing a JSON null, so an absent method does not look like a literal null to
 * a collector. event is the only required field.
 *
 * status 0 omits the key (no HTTP status is 0). duration_us is microseconds and
 * must be set to -1 to omit it; 0 is a legitimate (very fast) duration. */
typedef struct {
    log_level_t level;
    const char *event;
    const char *method;
    const char *path;
    int status;
    long long duration_us;
    const char *client;
    const char *error;
} log_record_t;

/* Appends one line, including the trailing newline. Safe on NULL l or NULL rec:
 * the record is dropped. Strings are escaped; control bytes become \u00XX. */
void log_write(log_t *l, const log_record_t *rec);

#endif /* OBSERVABILITY_LOG_H */
