// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const MARKUP = `
  <main>
    <h1>Encurtador</h1>
    <form id="form">
      <input type="url" id="url" />
      <button type="submit" id="botao">Encurtar</button>
    </form>
    <div id="resultado"></div>
    <div id="erro"></div>
  </main>
`;

const successBody = {
  code: "abc1234",
  short_url: "https://sho.rt/abc1234",
  expires_at: "2026-01-31T12:00:00.000Z",
};

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function stubClipboard(writeText: (value: string) => Promise<void>) {
  Object.defineProperty(navigator, "clipboard", {
    configurable: true,
    value: { writeText: vi.fn(writeText) },
  });
}

async function loadApp() {
  vi.resetModules();
  document.body.innerHTML = MARKUP;
  await import("../src/main");
}

function submit(value: string) {
  const input = document.getElementById("url") as HTMLInputElement;
  input.value = value;
  document.getElementById("form")!.dispatchEvent(new Event("submit", { cancelable: true }));
}

describe("interface principal", () => {
  beforeEach(() => {
    stubClipboard(async () => undefined);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("mostra o link curto, a expiração e o botão de copiar", async () => {
    const fetchMock = vi.fn().mockResolvedValue(jsonResponse(successBody, 201));
    vi.stubGlobal("fetch", fetchMock);
    await loadApp();

    submit("  https://example.com/artigo  ");

    await vi.waitFor(() => {
      expect(document.querySelector("#resultado a")?.textContent).toBe("https://sho.rt/abc1234");
    });
    expect(document.querySelector("#resultado a")?.getAttribute("href")).toBe(
      "https://sho.rt/abc1234",
    );
    expect(document.querySelector("#resultado .info")?.textContent).toContain("Expira em:");
    expect(document.querySelector("#resultado .copiar")?.textContent).toContain("Copiar link");
    expect(fetchMock).toHaveBeenCalledWith(
      "/shorten",
      expect.objectContaining({ body: JSON.stringify({ url: "https://example.com/artigo" }) }),
    );
  });

  it("exibe a mensagem de erro da API e reabilita o botão", async () => {
    vi.stubGlobal(
      "fetch",
      vi
        .fn()
        .mockResolvedValue(jsonResponse({ error: "URL inválida. Deve começar com http" }, 400)),
    );
    await loadApp();

    submit("example.com");

    await vi.waitFor(() => {
      expect(document.getElementById("erro")?.textContent).toBe(
        "URL inválida. Deve começar com http",
      );
    });
    expect((document.getElementById("botao") as HTMLButtonElement).disabled).toBe(false);
  });

  it("exibe mensagem genérica quando a rede falha", async () => {
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new Error("offline")));
    await loadApp();

    submit("https://example.com");

    await vi.waitFor(() => {
      expect(document.getElementById("erro")?.textContent).toBe(
        "Não foi possível conectar ao servidor.",
      );
    });
  });

  it("copia o link e restaura o texto do botão", async () => {
    const writeText = vi.fn(async () => undefined);
    stubClipboard(writeText);
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(jsonResponse(successBody, 201)));
    await loadApp();
    submit("https://example.com");

    const button = await vi.waitFor(() => {
      const element = document.querySelector<HTMLButtonElement>("#resultado .copiar");
      expect(element).not.toBeNull();
      return element!;
    });

    vi.useFakeTimers();
    button.click();
    await vi.advanceTimersByTimeAsync(0);

    expect(writeText).toHaveBeenCalledWith("https://sho.rt/abc1234");
    expect(button.textContent).toContain("Copiado");

    await vi.advanceTimersByTimeAsync(2000);
    expect(button.textContent).toContain("Copiar link");
  });

  it("avisa quando a cópia falha", async () => {
    stubClipboard(async () => {
      throw new Error("sem permissão");
    });
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(jsonResponse(successBody, 201)));
    await loadApp();
    submit("https://example.com");

    const button = await vi.waitFor(() => {
      const element = document.querySelector<HTMLButtonElement>("#resultado .copiar");
      expect(element).not.toBeNull();
      return element!;
    });

    button.click();

    await vi.waitFor(() => {
      expect(button.textContent).toContain("Falha ao copiar");
    });
  });
});
