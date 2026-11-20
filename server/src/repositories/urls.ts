import type { Pool } from "pg";

import type { NewUrl, UrlRecord, UrlRepository } from "../types";

interface UrlRow {
  id: string | number;
  code: string;
  original_url: string;
  expires_at: Date;
}

export class PgUrlRepository implements UrlRepository {
  constructor(private readonly pool: Pool) {}

  async insert({ code, originalUrl, expiresAt }: NewUrl): Promise<void> {
    await this.pool.query("INSERT INTO urls (code, original_url, expires_at) VALUES ($1, $2, $3)", [
      code,
      originalUrl,
      expiresAt,
    ]);
  }

  async findByCode(code: string): Promise<UrlRecord | null> {
    const { rows } = await this.pool.query<UrlRow>(
      "SELECT id, code, original_url, expires_at FROM urls WHERE code = $1",
      [code],
    );
    const row = rows[0];
    if (!row) {
      return null;
    }
    return {
      id: Number(row.id),
      code: row.code,
      originalUrl: row.original_url,
      expiresAt: row.expires_at,
    };
  }
}
