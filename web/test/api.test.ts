import { afterEach, describe, expect, it, vi } from "vitest";

import { ApiError, shortenUrl } from "../src/api";

function mockFetch(response: Response | Error) {
  const fetchMock =
    response instanceof Error
      ? vi.fn().mockRejectedValue(response)
      : vi.fn().mockResolvedValue(response);
  vi.stubGlobal("fetch", fetchMock);
  return fetchMock;
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

describe("shortenUrl", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("envia POST e devolve o resultado", async () => {
    const fetchMock = mockFetch(
      jsonResponse(
        {
          code: "abc1234",
          short_url: "https://sho.rt/abc1234",
          expires_at: "2026-01-31T12:00:00.000Z",
        },
        201,
      ),
    );

    const result = await shortenUrl("https://example.com");

    expect(result.code).toBe("abc1234");
    expect(fetchMock).toHaveBeenCalledWith("/shorten", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url: "https://example.com" }),
    });
  });

  it("prefixa a base URL quando informada", async () => {
    const fetchMock = mockFetch(
      jsonResponse({ code: "abc1234", short_url: "x", expires_at: "2026-01-31T12:00:00.000Z" }),
    );

    await shortenUrl("https://example.com", "https://api.example.com");

    expect(fetchMock).toHaveBeenCalledWith("https://api.example.com/shorten", expect.anything());
  });

  it("usa a mensagem de erro devolvida pela API", async () => {
    mockFetch(jsonResponse({ error: "URL inválida." }, 400));

    await expect(shortenUrl("example.com")).rejects.toThrow(new ApiError("URL inválida."));
  });

  it("usa mensagem padrão quando o erro não tem JSON", async () => {
    mockFetch(new Response("oops", { status: 500 }));

    await expect(shortenUrl("https://example.com")).rejects.toThrow("Erro ao encurtar o link.");
  });

  it("rejeita resposta 2xx com formato inesperado", async () => {
    mockFetch(jsonResponse({ ok: true }));

    await expect(shortenUrl("https://example.com")).rejects.toThrow("Resposta inválida");
  });

  it("traduz falha de rede", async () => {
    mockFetch(new Error("network down"));

    await expect(shortenUrl("https://example.com")).rejects.toThrow(
      "Não foi possível conectar ao servidor.",
    );
  });
});
