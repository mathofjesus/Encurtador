import type { Pool } from "pg";
import { describe, expect, it } from "vitest";

import { applyCodeMigration } from "../src/db/migrate";

interface Row {
  id: number;
  code: string | null;
}

class FakePool {
  readonly rows: Row[];
  readonly queries: { text: string; values?: unknown[] }[] = [];
  collisions = 0;
  failWith?: Error;

  constructor(rows: Row[]) {
    this.rows = rows;
  }

  async query(text: string, values?: unknown[]): Promise<{ rows: Row[]; rowCount?: number }> {
    this.queries.push({ text, values });
    const sql = text.trim().toUpperCase();

    if (sql.startsWith("SELECT ID FROM URLS WHERE CODE IS NULL")) {
      return { rows: this.rows.filter((row) => row.code === null) };
    }

    if (sql.startsWith("UPDATE URLS SET CODE")) {
      if (this.failWith) {
        throw this.failWith;
      }
      if (this.collisions > 0) {
        this.collisions -= 1;
        throw Object.assign(new Error("duplicate key"), { code: "23505" });
      }
      const [code, id] = values as [string, number];
      const row = this.rows.find((candidate) => candidate.id === id);
      if (row) {
        row.code = code;
      }
      return { rows: [], rowCount: 1 };
    }

    return { rows: [] };
  }
}

function asPool(pool: FakePool): Pool {
  return pool as unknown as Pool;
}

describe("applyCodeMigration", () => {
  it("adiciona a coluna, preenche apenas as linhas sem código e mantém as demais", async () => {
    const pool = new FakePool([
      { id: 1, code: "aaaaaaa" },
      { id: 2, code: null },
      { id: 3, code: null },
    ]);

    const updated = await applyCodeMigration(asPool(pool));

    expect(updated).toBe(2);
    expect(pool.rows.every((row) => typeof row.code === "string" && row.code.length === 7)).toBe(
      true,
    );
    expect(pool.rows[0]?.code).toBe("aaaaaaa");
    expect(pool.queries.map((query) => query.text.trim().split(/\s+/)[0])).toContain("ALTER");
  });

  it("cria o índice único antes do backfill e finaliza com NOT NULL", async () => {
    const pool = new FakePool([]);

    await applyCodeMigration(asPool(pool));

    const sql = pool.queries.map((query) => query.text.trim());
    expect(sql).toContain("ALTER TABLE urls ADD COLUMN IF NOT EXISTS code TEXT");
    expect(sql).toContain("CREATE UNIQUE INDEX IF NOT EXISTS idx_urls_code ON urls (code)");
    expect(sql).toContain("ALTER TABLE urls ALTER COLUMN code SET NOT NULL");
  });

  it("repete a geração quando o código colide", async () => {
    const pool = new FakePool([{ id: 1, code: null }]);
    pool.collisions = 3;

    const updated = await applyCodeMigration(asPool(pool));

    expect(updated).toBe(1);
    expect(pool.rows[0]?.code).toMatch(/^[0-9A-Za-z]{7}$/);
  });

  it("propaga erros que não são de unicidade", async () => {
    const pool = new FakePool([{ id: 1, code: null }]);
    pool.failWith = new Error("permissão negada");

    await expect(applyCodeMigration(asPool(pool))).rejects.toThrow("permissão negada");
  });
});
