import { loadConfig } from "../config";
import { applyCodeMigration } from "../db/migrate";
import { createPool } from "../db/pool";

/** Preenche códigos para linhas antigas sem derrubar a tabela. */
async function main(): Promise<void> {
  const config = loadConfig();
  const pool = createPool(config.databaseUrl, config.databaseSsl);

  try {
    const updated = await applyCodeMigration(pool);
    console.log(`Backfill concluído: ${updated} URL(s) receberam um novo código.`);
  } finally {
    await pool.end();
  }
}

main().catch((error: unknown) => {
  console.error("Falha no backfill:", error);
  process.exit(1);
});
