/* Logging tests.
 *
 * The output goes to a pipe rather than stderr so a test can read exactly the
 * bytes a collector would receive, not a rendering of them.
 */
#include <criterion/criterion.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "observability/log.h"

static void open_pipe(int fds[2])
{
    cr_assert_eq(pipe(fds), 0);

    /* Non-blocking, or draining an empty pipe after a filtered record would
     * block forever. */
    int fl = fcntl(fds[0], F_GETFL, 0);
    cr_assert_neq(fl, -1);
    cr_assert_neq(fcntl(fds[0], F_SETFL, fl | O_NONBLOCK), -1);
}

static size_t drain(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
        if (total >= cap - 1)
            break;
    }
    buf[total] = '\0';
    return total;
}

static int count_char(const char *s, char c)
{
    int n = 0;
    for (; *s; s++)
        if (*s == c)
            n++;
    return n;
}

Test(log, a_record_is_one_json_object_per_line)
{
    int fds[2];
    open_pipe(fds);
    log_t *l = log_new(fds[1], LOG_DEBUG);
    cr_assert_not_null(l);

    log_record_t rec = {0};
    rec.level = LOG_INFO;
    rec.event = "request";
    rec.method = "GET";
    rec.path = "/abc1234";
    rec.status = 301;
    rec.duration_us = 412;
    rec.client = "203.0.113.7";
    log_write(l, &rec);

    char buf[1024];
    size_t n = drain(fds[0], buf, sizeof(buf));
    cr_assert_gt(n, 0, "nothing was written");
    cr_assert_eq(buf[n - 1], '\n', "a record must end in a newline: %s", buf);
    cr_assert_eq(count_char(buf, '\n'), 1, "not one line: %s", buf);

    cr_assert_not_null(strstr(buf, "\"ts\":"), "no timestamp: %s", buf);
    cr_assert_not_null(strstr(buf, "\"level\":\"info\""), "%s", buf);
    cr_assert_not_null(strstr(buf, "\"event\":\"request\""), "%s", buf);
    cr_assert_not_null(strstr(buf, "\"method\":\"GET\""), "%s", buf);
    cr_assert_not_null(strstr(buf, "\"path\":\"/abc1234\""), "%s", buf);
    cr_assert_not_null(strstr(buf, "\"status\":301"), "%s", buf);
    cr_assert_not_null(strstr(buf, "\"duration_us\":412"), "%s", buf);
    cr_assert_not_null(strstr(buf, "\"client\":\"203.0.113.7\""), "%s", buf);

    log_free(l);
    close(fds[0]);
    close(fds[1]);
}

Test(log, absent_optional_fields_are_omitted_not_null)
{
    int fds[2];
    open_pipe(fds);
    log_t *l = log_new(fds[1], LOG_DEBUG);

    log_record_t rec = {0};
    rec.level = LOG_WARN;
    rec.event = "cache_unavailable";
    rec.duration_us = -1; /* the documented sentinel */
    log_write(l, &rec);

    char buf[1024];
    drain(fds[0], buf, sizeof(buf));

    cr_assert_not_null(strstr(buf, "\"event\":\"cache_unavailable\""), "%s", buf);
    cr_assert_null(strstr(buf, "\"method\""), "an absent field appeared: %s", buf);
    cr_assert_null(strstr(buf, "\"path\""), "%s", buf);
    cr_assert_null(strstr(buf, "\"status\""), "%s", buf);
    cr_assert_null(strstr(buf, "\"duration_us\""), "%s", buf);
    cr_assert_null(strstr(buf, "\"client\""), "%s", buf);
    cr_assert_null(strstr(buf, "null"), "absent fields must be omitted, not null: %s",
                   buf);

    log_free(l);
    close(fds[0]);
    close(fds[1]);
}

Test(log, records_below_the_minimum_level_are_dropped)
{
    int fds[2];
    open_pipe(fds);
    log_t *l = log_new(fds[1], LOG_WARN);

    char buf[1024];
    log_record_t rec = {0};
    rec.level = LOG_INFO;
    rec.event = "quiet";
    rec.duration_us = -1;
    log_write(l, &rec);
    cr_assert_eq(drain(fds[0], buf, sizeof(buf)), 0,
                 "an info record reached a warn-only log: %s", buf);

    rec.level = LOG_ERROR;
    rec.event = "loud";
    log_write(l, &rec);
    cr_assert_gt(drain(fds[0], buf, sizeof(buf)), 0, "an error record was dropped");

    log_free(l);
    close(fds[0]);
    close(fds[1]);
}

Test(log, strings_from_a_request_are_escaped)
{
    int fds[2];
    open_pipe(fds);
    log_t *l = log_new(fds[1], LOG_DEBUG);

    /* A quote would end the JSON string early and let the client write further
     * fields; a newline would forge a second record. */
    log_record_t rec = {0};
    rec.level = LOG_INFO;
    rec.event = "request";
    rec.path = "/a\"b\\c";
    rec.duration_us = -1;
    log_write(l, &rec);

    char buf[1024];
    drain(fds[0], buf, sizeof(buf));
    cr_assert_not_null(strstr(buf, "/a\\\"b\\\\c"), "not escaped: %s", buf);
    cr_assert_eq(count_char(buf, '\n'), 1, "an injected newline split the record: %s",
                 buf);

    log_free(l);
    close(fds[0]);
    close(fds[1]);
}

Test(log, a_control_byte_becomes_an_escape)
{
    int fds[2];
    open_pipe(fds);
    log_t *l = log_new(fds[1], LOG_DEBUG);

    log_record_t rec = {0};
    rec.level = LOG_INFO;
    rec.event = "request";
    rec.path = "/x\ty"; /* a literal tab */
    rec.duration_us = -1;
    log_write(l, &rec);

    char buf[1024];
    drain(fds[0], buf, sizeof(buf));
    cr_assert_not_null(strstr(buf, "/x\\ty"), "tab not escaped: %s", buf);
    cr_assert_eq(count_char(buf, '\t'), 0, "a raw tab reached the output");

    log_free(l);
    close(fds[0]);
    close(fds[1]);
}

Test(log, null_arguments_are_safe)
{
    int fds[2];
    open_pipe(fds);
    log_t *l = log_new(fds[1], LOG_DEBUG);

    log_record_t rec = {0};
    rec.level = LOG_INFO;
    rec.event = "request";
    rec.duration_us = -1;

    log_write(NULL, &rec);
    log_write(l, NULL);
    log_set_level(NULL, LOG_INFO);

    char buf[256];
    cr_assert_eq(drain(fds[0], buf, sizeof(buf)), 0, "a null call wrote something");

    log_free(l);
    close(fds[0]);
    close(fds[1]);
}

Test(log, level_names_round_trip)
{
    cr_assert_str_eq(log_level_name(LOG_DEBUG), "debug");
    cr_assert_str_eq(log_level_name(LOG_INFO), "info");
    cr_assert_str_eq(log_level_name(LOG_WARN), "warn");
    cr_assert_str_eq(log_level_name(LOG_ERROR), "error");

    cr_assert_eq(log_level_from_string("ERROR"), LOG_ERROR);
    cr_assert_eq(log_level_from_string("debug"), LOG_DEBUG);
    /* A typo is a typo, not a reason to emit nothing. */
    cr_assert_eq(log_level_from_string(NULL), LOG_INFO);
    cr_assert_eq(log_level_from_string("nonsense"), LOG_INFO);
}
