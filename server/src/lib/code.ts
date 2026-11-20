import { randomInt } from "node:crypto";

export const BASE62 = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
export const CODE_LENGTH = 7;

/** Códigos válidos têm exatamente 7 caracteres base62. */
export const CODE_REGEX = /^[0-9A-Za-z]{7}$/;

/**
 * Gera um código base62 aleatório.
 * `crypto.randomInt` é uniforme — não use `randomBytes() % 62`, que introduz viés.
 */
export function generateCode(): string {
  let code = "";
  for (let i = 0; i < CODE_LENGTH; i += 1) {
    code += BASE62[randomInt(0, BASE62.length)];
  }
  return code;
}

export function isValidCode(code: string): boolean {
  return CODE_REGEX.test(code);
}
