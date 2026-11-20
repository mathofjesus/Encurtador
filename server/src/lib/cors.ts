import type { CorsOptions } from "cors";

/**
 * FRONTEND_ORIGIN aceita `*`, uma origem única ou uma lista separada por vírgula.
 */
export function buildCorsOptions(frontendOrigin: string): CorsOptions {
  const value = frontendOrigin.trim();
  if (value === "" || value === "*") {
    return { origin: "*" };
  }
  const origins = value
    .split(",")
    .map((origin) => origin.trim())
    .filter(Boolean);
  return { origin: origins };
}
