/* Incremental HTTP/1.1 request parser.
 *
 * The single most important property: http_parse is called repeatedly with a
 * growing buffer as bytes arrive, and it must return NEED_MORE until a whole
 * request is present. TCP does not respect message boundaries, so a parser
 * that assumes one read yields one request breaks against any slow client.
 *
 * Contract:
 *   NEED_MORE  keep the bytes, call again after the next read
 *   COMPLETE   r is filled in; r->consumed says how many bytes it used
 *   MALFORMED  refuse the request; the reactor answers 400 and closes
 *
 * Anything over a cap is MALFORMED rather than truncated. A truncated path is
 * a request for a different resource, which is worse than a rejected one.
 */
#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include "http/http.h"

/* Parses or advances a request. See the contract above. */
int http_parse(http_request_t *r, const char *data, size_t len);

/* Case-insensitive header lookup, or NULL when absent. */
const char *http_get_header(const http_request_t *r, const char *name);

#endif /* HTTP_PARSER_H */
