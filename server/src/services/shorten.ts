import { generateCode } from "../lib/code";
import type { UrlRepository } from "../types";

export const LINK_TTL_DAYS = 30;
export const MAX_CODE_ATTEMPTS = 5;

const DAY_IN_MS = 24 * 60 * 60 * 1000;

export class CodeGenerationError extends Error {
  constructor() {
    super("Não foi possível gerar um código único.");
    this.name = "CodeGenerationError";
  }
}

function isUniqueViolation(error: unknown): boolean {
  return (
    typeof error === "object" &&
    error !== null &&
    "code" in error &&
    (error as { code?: unknown }).code === "23505"
  );
}

export interface CreatedShortUrl {
  code: string;
  expiresAt: Date;
}

/**
 * Cria a URL curta com um código aleatório persistido.
 * Em colisão do índice único (`23505`), tenta novamente com um novo código.
 */
export async function createShortUrl(
  repo: UrlRepository,
  originalUrl: string,
  now: Date,
  generate: () => string = generateCode,
): Promise<CreatedShortUrl> {
  const expiresAt = new Date(now.getTime() + LINK_TTL_DAYS * DAY_IN_MS);

  for (let attempt = 0; attempt < MAX_CODE_ATTEMPTS; attempt += 1) {
    const code = generate();
    try {
      await repo.insert({ code, originalUrl, expiresAt });
      return { code, expiresAt };
    } catch (error) {
      if (!isUniqueViolation(error)) {
        throw error;
      }
    }
  }

  throw new CodeGenerationError();
}
