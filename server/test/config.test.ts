import { afterEach, describe, expect, it, vi } from "vitest";

import { loadConfig } from "../src/config";

describe("loadConfig", () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it("aplica defaults e remove barra final de PUBLIC_BASE_URL", () => {
    const config = loadConfig({
      DATABASE_URL: "postgresql://user:pass@localhost:5432/db",
      PUBLIC_BASE_URL: "https://sho.rt/",
    });

    expect(config).toEqual({
      databaseUrl: "postgresql://user:pass@localhost:5432/db",
      port: 5000,
      frontendOrigin: "*",
      publicBaseUrl: "https://sho.rt",
    });
  });

  it("trata PUBLIC_BASE_URL vazia como ausente e valida PORT", () => {
    const config = loadConfig({
      DATABASE_URL: "postgresql://user:pass@localhost:5432/db",
      PUBLIC_BASE_URL: "   ",
      PORT: "8080",
    });

    expect(config.publicBaseUrl).toBeUndefined();
    expect(config.port).toBe(8080);
  });

  it("encerra o processo quando DATABASE_URL não está definida", () => {
    const exit = vi.spyOn(process, "exit").mockImplementation(((code?: number) => {
      throw new Error(`exit:${code}`);
    }) as never);
    const error = vi.spyOn(console, "error").mockImplementation(() => undefined);

    expect(() => loadConfig({})).toThrow("exit:1");

    expect(exit).toHaveBeenCalledWith(1);
    expect(error).toHaveBeenCalledWith(expect.stringContaining("configuração inválida"));
  });
});
