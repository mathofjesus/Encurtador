import { describe, expect, it } from "vitest";

import { buildCorsOptions } from "../src/lib/cors";
import { isValidHttpUrl } from "../src/lib/url";

describe("isValidHttpUrl", () => {
  it.each(["http://example.com", "https://example.com/a?b=c"])("aceita %s", (url) => {
    expect(isValidHttpUrl(url)).toBe(true);
  });

  it.each(["", "example.com", "ftp://example.com", " HTTP://example.com", "http:/example.com"])(
    "rejeita %j",
    (url) => {
      expect(isValidHttpUrl(url)).toBe(false);
    },
  );
});

describe("buildCorsOptions", () => {
  it("libera todas as origens com *", () => {
    expect(buildCorsOptions("*").origin).toBe("*");
    expect(buildCorsOptions("  ").origin).toBe("*");
  });

  it("converte lista separada por vírgula em array", () => {
    expect(buildCorsOptions("https://a.com, https://b.com").origin).toEqual([
      "https://a.com",
      "https://b.com",
    ]);
  });
});
