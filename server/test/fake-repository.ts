import type { NewUrl, UrlRecord, UrlRepository } from "../src/types";

/** Repositório em memória para testar rotas sem Postgres. */
export class FakeUrlRepository implements UrlRepository {
  readonly rows: UrlRecord[] = [];
  collisions = 0;
  findCalls: string[] = [];
  nextId = 1;

  async insert({ code, originalUrl, expiresAt }: NewUrl): Promise<void> {
    if (this.collisions > 0) {
      this.collisions -= 1;
      throw Object.assign(new Error("duplicate key"), { code: "23505" });
    }
    if (this.rows.some((row) => row.code === code)) {
      throw Object.assign(new Error("duplicate key"), { code: "23505" });
    }
    this.rows.push({ id: this.nextId, code, originalUrl, expiresAt });
    this.nextId += 1;
  }

  async findByCode(code: string): Promise<UrlRecord | null> {
    this.findCalls.push(code);
    return this.rows.find((row) => row.code === code) ?? null;
  }
}
