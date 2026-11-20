import { describe, expect, it } from "vitest";

import { BASE62, CODE_LENGTH, CODE_REGEX, generateCode, isValidCode } from "../src/lib/code";

describe("generateCode", () => {
  it("gera exatamente 7 caracteres base62", () => {
    const code = generateCode();

    expect(code).toMatch(CODE_REGEX);
    expect(code).toHaveLength(CODE_LENGTH);
    for (const char of code) {
      expect(BASE62).toContain(char);
    }
  });

  it("não repete o mesmo código em sequência", () => {
    expect(generateCode()).not.toBe(generateCode());
  });
});

describe("isValidCode", () => {
  it.each(["aB3xY9z", "0000000", "ZZZZZZZ"])("aceita %s", (code) => {
    expect(isValidCode(code)).toBe(true);
  });

  it.each(["", "abc", "abcdefgh", "abcdef-", "abc def", "ábcdefg"])("rejeita %j", (code) => {
    expect(isValidCode(code)).toBe(false);
  });
});
