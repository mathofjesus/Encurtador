import type { Pool } from "pg";

import { generateCode } from "../lib/code";

function isUniqueViolation(error: unknown): boolean {
  return (
    typeof error === "object" &&
    error !== null &&
    "code" in error &&
    (error as { code?: unknown }).code === "23505"
  );
}

/**
 * Migração idempotente do schema antigo (sem `code`) para o novo:
 * 1. adiciona a coluna nullable;
 * 2. cria o índice único (NULLs são permitidos múltiplos);
 * 3. preenche códigos novos para linhas existentes;
 * 4. torna a coluna NOT NULL.
 *
 * As URLs e expirações originais são preservadas; os links antigos deixam de existir.
 */
export async function applyCodeMigration(pool: Pool): Promise<number> {
  await pool.query("ALTER TABLE urls ADD COLUMN IF NOT EXISTS code TEXT");
  await pool.query("CREATE UNIQUE INDEX IF NOT EXISTS idx_urls_code ON urls (code)");

  const { rows } = await pool.query<{ id: string | number }>(
    "SELECT id FROM urls WHERE code IS NULL",
  );

  let updated = 0;
  for (const row of rows) {
    for (let attempt = 0; attempt < 10; attempt += 1) {
      try {
        await pool.query("UPDATE urls SET code = $1 WHERE id = $2 AND code IS NULL", [
          generateCode(),
          row.id,
        ]);
        updated += 1;
        break;
      } catch (error) {
        if (!isUniqueViolation(error) || attempt === 9) {
          throw error;
        }
      }
    }
  }

  await pool.query("ALTER TABLE urls ALTER COLUMN code SET NOT NULL");
  return updated;
}
