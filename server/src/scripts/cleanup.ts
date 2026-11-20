import { loadConfig } from "../config";
import { createPool } from "../db/pool";

/** Remove URLs expiradas. Pode ser agendado no cron do host ou via pg_cron (sql/cron.sql). */
async function main(): Promise<void> {
  const config = loadConfig();
  const pool = createPool(config.databaseUrl, config.databaseSsl);

  try {
    const result = await pool.query("DELETE FROM urls WHERE expires_at < now()");
    console.log(`Limpeza concluída: ${result.rowCount ?? 0} URL(s) expirada(s) removida(s).`);
  } finally {
    await pool.end();
  }
}

main().catch((error: unknown) => {
  console.error("Falha na limpeza:", error);
  process.exit(1);
});
