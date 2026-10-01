-- Schema for stored URLs.
--
-- Partitioned by creation time so that expiry is a DROP TABLE rather than a
-- DELETE, which is the only reason to partition at all: the retention
-- requirement is ten years, and ten years of rows in one heap means an index
-- that grows without bound and a vacuum that never finishes.
--
-- The partition ranges are NOT in this file. A literal date checked in here
-- silently rots, and the first insert after it passes fails with "no partition
-- of relation urls found for row" — a failure that looks like a bug in the
-- service. pg_run_schema generates the previous, current and next month's
-- partitions from the current date instead.

CREATE TABLE IF NOT EXISTS urls (
    id         BIGSERIAL,
    code       VARCHAR(12) NOT NULL,
    url        TEXT        NOT NULL,
    created_at TIMESTAMPTZ  NOT NULL DEFAULT now(),
    expires_at TIMESTAMPTZ,

    -- The partition key must be part of the primary key of a partitioned table.
    -- Postgres rejects anything else, which is not a style preference.
    PRIMARY KEY (id, created_at)
) PARTITION BY RANGE (created_at);

-- Uniqueness is enforced on (code, created_at), not on code alone, and could
-- not be otherwise: a unique index on a partitioned table must include the
-- partition key. So the database guarantees a code is unique *within* a
-- partition, and the application guarantees it across partitions by retrying on
-- collision. tests/test_pg.c asserts both halves of that statement, including
-- that Postgres refuses CREATE UNIQUE INDEX ON urls (code).
CREATE UNIQUE INDEX IF NOT EXISTS urls_code_created_idx ON urls (code, created_at);

-- A lookup by code has to probe every partition, so each partition needs its own
-- index on code. This one exists to make that probe cheap rather than a scan.
CREATE INDEX IF NOT EXISTS urls_code_idx ON urls (code);

-- Supports the cleanup sweep without scanning rows that never expire.
CREATE INDEX IF NOT EXISTS urls_expires_idx ON urls (expires_at)
    WHERE expires_at IS NOT NULL;