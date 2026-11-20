import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

import request from "supertest";
import { afterAll, describe, expect, it } from "vitest";

import { createApp } from "../src/app";
import { FakeUrlRepository } from "./fake-repository";

const NOW = new Date("2026-01-01T12:00:00.000Z");

function setup(overrides: Partial<Parameters<typeof createApp>[0]> = {}) {
  const repo = new FakeUrlRepository();
  const app = createApp({ repo, clock: () => NOW, ...overrides });
  return { app, repo };
}

describe("GET /health", () => {
  it("responde ok", async () => {
    const { app } = setup();

    const response = await request(app).get("/health");

    expect(response.status).toBe(200);
    expect(response.body).toEqual({ status: "ok" });
  });
});

describe("POST /shorten", () => {
  it("cria um link curto e devolve 201", async () => {
    const { app, repo } = setup();

    const response = await request(app)
      .post("/shorten")
      .send({ url: "  https://example.com/artigo  " });

    expect(response.status).toBe(201);
    expect(response.body.code).toMatch(/^[0-9A-Za-z]{7}$/);
    expect(response.body.short_url).toMatch(new RegExp(`/${response.body.code}$`));
    expect(response.body.expires_at).toBe("2026-01-31T12:00:00.000Z");
    expect(repo.rows[0]?.originalUrl).toBe("https://example.com/artigo");
  });

  it("usa PUBLIC_BASE_URL quando configurado", async () => {
    const { app } = setup({ publicBaseUrl: "https://sho.rt" });

    const response = await request(app).post("/shorten").send({ url: "https://example.com" });

    expect(response.body.short_url).toBe(`https://sho.rt/${response.body.code}`);
  });

  it.each([
    {},
    { url: "" },
    { url: "example.com" },
    { url: "ftp://example.com" },
    { url: 42 },
    { url: null },
  ])("rejeita corpo inválido %j com 400", async (body) => {
    const { app, repo } = setup();

    const response = await request(app).post("/shorten").send(body);

    expect(response.status).toBe(400);
    expect(response.body.error).toContain("URL inválida");
    expect(repo.rows).toHaveLength(0);
  });

  it("rejeita JSON malformado com 400", async () => {
    const { app } = setup();

    const response = await request(app)
      .post("/shorten")
      .set("Content-Type", "application/json")
      .send('{"url": ');

    expect(response.status).toBe(400);
    expect(response.body).toEqual({ error: "JSON inválido." });
  });

  it("tenta novamente quando o código colide", async () => {
    const { app, repo } = setup();
    repo.collisions = 1;

    const response = await request(app).post("/shorten").send({ url: "https://example.com" });

    expect(response.status).toBe(201);
    expect(repo.rows).toHaveLength(1);
  });
});

describe("GET /:code", () => {
  async function shorten(app: ReturnType<typeof createApp>) {
    const response = await request(app)
      .post("/shorten")
      .send({ url: "https://example.com/destino" });
    return response.body.code as string;
  }

  it("redireciona com 302 para a URL original", async () => {
    const { app } = setup();
    const code = await shorten(app);

    const response = await request(app).get(`/${code}`);

    expect(response.status).toBe(302);
    expect(response.headers.location).toBe("https://example.com/destino");
  });

  it("responde 404 para código inexistente", async () => {
    const { app, repo } = setup();

    const response = await request(app).get("/abc1234");

    expect(response.status).toBe(404);
    expect(response.body).toEqual({ error: "Link não encontrado." });
    expect(repo.findCalls).toContain("abc1234");
  });

  it.each(["abc", "abcdefgh", "abcdef-", "abc def"])(
    "responde 404 sem consultar o banco para %j",
    async (code) => {
      const { app, repo } = setup();

      const response = await request(app).get(`/${encodeURIComponent(code)}`);

      expect(response.status).toBe(404);
      expect(repo.findCalls).toHaveLength(0);
    },
  );

  it("responde 410 para link expirado", async () => {
    const repo = new FakeUrlRepository();
    const expired = new Date("2026-01-01T11:59:59.000Z");
    const app = createApp({ repo, clock: () => NOW });
    await repo.insert({
      code: "abc1234",
      originalUrl: "https://example.com",
      expiresAt: expired,
    });

    const response = await request(app).get("/abc1234");

    expect(response.status).toBe(410);
    expect(response.body.error).toContain("expirou");
  });

  it("responde 404 para caminhos desconhecidos com vários segmentos", async () => {
    const { app } = setup();

    const response = await request(app).get("/foo/bar");

    expect(response.status).toBe(404);
    expect(response.body).toEqual({ error: "Não encontrado." });
  });
});

describe("CORS", () => {
  it("libera qualquer origem por padrão", async () => {
    const { app } = setup();

    const response = await request(app).get("/health").set("Origin", "https://qualquer.com");

    expect(response.headers["access-control-allow-origin"]).toBe("*");
  });

  it("reflete apenas as origens configuradas", async () => {
    const { app } = setup({ frontendOrigin: "https://meu.site,https://outro.site" });

    const allowed = await request(app).get("/health").set("Origin", "https://meu.site");
    const denied = await request(app).get("/health").set("Origin", "https://malicioso.com");

    expect(allowed.headers["access-control-allow-origin"]).toBe("https://meu.site");
    expect(denied.headers["access-control-allow-origin"]).toBeUndefined();
  });
});

describe("erros inesperados", () => {
  it("responde 500 em JSON quando o repositório falha", async () => {
    const repo = new FakeUrlRepository();
    repo.findByCode = async () => {
      throw new Error("banco indisponível");
    };
    const app = createApp({ repo, clock: () => NOW });

    const response = await request(app).get("/abc1234");

    expect(response.status).toBe(500);
    expect(response.body).toEqual({ error: "Erro interno do servidor." });
  });
});

describe("frontend estático", () => {
  let staticDir: string | undefined;

  afterAll(async () => {
    if (staticDir) {
      await rm(staticDir, { recursive: true, force: true });
    }
  });

  it("serve index.html em /", async () => {
    staticDir = await mkdtemp(path.join(tmpdir(), "encurtador-"));
    await writeFile(path.join(staticDir, "index.html"), "<h1>Encurtador</h1>");
    const { app } = setup({ staticDir });

    const response = await request(app).get("/");

    expect(response.status).toBe(200);
    expect(response.text).toContain("Encurtador");
  });
});
