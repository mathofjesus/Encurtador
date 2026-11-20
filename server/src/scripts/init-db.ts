import { loadConfig } from "../config";
import { applyCodeMigration } from "../db/migrate";
import { createPool } from "../db/pool";

async function main(): Promise<void> {
  const config = loadConfig();
  const pool = createPool(config.databaseUrl, config.databaseSsl);

  try {
    await pool.query(`
      CREATE TABLE IF NOT EXISTS urls (
        id           BIGSERIAL    PRIMARY KEY,
        code         TEXT         NOT NULL,
        original_url TEXT         NOT NULL,
        created_at   TIMESTAMPTZ  NOT NULL DEFAULT now(),
        expires_at   TIMESTAMPTZ  NOT NULL
      )
    `);
    await pool.query("CREATE INDEX IF NOT EXISTS idx_urls_expires_at ON urls (expires_at)");

    const updated = await applyCodeMigration(pool);
    if (updated > 0) {
      console.log(`Backfill: ${updated} URL(s) existente(s) receberam um novo código.`);
    }
    console.log("Banco inicializado com sucesso.");
  } finally {
    await pool.end();
  }
}

main().catch((error: unknown) => {
  console.error("Falha ao inicializar o banco:", error);
  process.exit(1);
});
