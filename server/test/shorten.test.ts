import { describe, expect, it } from "vitest";

import { CodeGenerationError, createShortUrl, LINK_TTL_DAYS } from "../src/services/shorten";
import { FakeUrlRepository } from "./fake-repository";

const NOW = new Date("2026-01-01T12:00:00.000Z");

describe("createShortUrl", () => {
  it("cria a URL com expiração de 30 dias", async () => {
    const repo = new FakeUrlRepository();

    const result = await createShortUrl(repo, "https://example.com", NOW, () => "abc1234");

    expect(result.code).toBe("abc1234");
    expect(result.expiresAt.toISOString()).toBe("2026-01-31T12:00:00.000Z");
    expect(LINK_TTL_DAYS).toBe(30);
    expect(repo.rows).toHaveLength(1);
    expect(repo.rows[0]?.originalUrl).toBe("https://example.com");
  });

  it("tenta outro código quando o índice único acusa colisão", async () => {
    const repo = new FakeUrlRepository();
    repo.collisions = 2;
    const codes = ["aaaaaaa", "bbbbbbb", "ccccccc"];
    let index = 0;

    const result = await createShortUrl(repo, "https://example.com", NOW, () => codes[index++]!);

    expect(result.code).toBe("ccccccc");
    expect(repo.rows).toHaveLength(1);
  });

  it("desiste após 5 colisões", async () => {
    const repo = new FakeUrlRepository();
    repo.collisions = 10;

    await expect(
      createShortUrl(repo, "https://example.com", NOW, () => "abc1234"),
    ).rejects.toBeInstanceOf(CodeGenerationError);
  });

  it("propaga erros que não são de unicidade", async () => {
    const repo = new FakeUrlRepository();
    repo.insert = async () => {
      throw new Error("conexão caiu");
    };

    await expect(createShortUrl(repo, "https://example.com", NOW)).rejects.toThrow("conexão caiu");
  });
});
