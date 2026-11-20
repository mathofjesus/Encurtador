import { Pool } from "pg";

/**
 * Cria o pool de conexões.
 *
 * - Supabase transaction pooler (porta 6543) não aceita prepared statements nomeados:
 *   nunca passe `name` em `pool.query`.
 * - TLS: `DATABASE_SSL=true|false` manda; sem override, usa SSL automaticamente para
 *   Supabase (host *.supabase.co ou porta 6543) e respeita `sslmode` da URL.
 * - `max: 5` mantém o mesmo dimensionamento do backend anterior.
 */
export function createPool(connectionString: string, sslOverride?: boolean): Pool {
  const url = new URL(connectionString);
  const hasSslMode = url.searchParams.has("sslmode");
  const looksLikeSupabase = url.hostname.includes("supabase") || url.port === "6543";

  let ssl: false | { rejectUnauthorized: boolean } | undefined;
  if (sslOverride === true) {
    ssl = { rejectUnauthorized: false };
  } else if (sslOverride === false || hasSslMode) {
    ssl = false;
  } else {
    ssl = looksLikeSupabase ? { rejectUnauthorized: false } : false;
  }

  return new Pool({
    connectionString,
    max: 5,
    idleTimeoutMillis: 30_000,
    connectionTimeoutMillis: 10_000,
    ssl,
  });
}
