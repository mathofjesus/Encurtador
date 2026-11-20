import { z } from "zod";

export interface Config {
  databaseUrl: string;
  databaseSsl?: boolean;
  port: number;
  frontendOrigin: string;
  publicBaseUrl?: string;
}

const envSchema = z.object({
  DATABASE_URL: z
    .string({ error: "Defina a variável de ambiente DATABASE_URL." })
    .min(1, "Defina a variável de ambiente DATABASE_URL."),
  DATABASE_SSL: z.enum(["true", "false"]).optional(),
  PORT: z.coerce.number().int().positive().default(5000),
  FRONTEND_ORIGIN: z.string().default("*"),
  PUBLIC_BASE_URL: z.string().trim().optional(),
});

/** Valida o ambiente no bootstrap; sem DATABASE_URL o processo encerra explicitamente. */
export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const parsed = envSchema.safeParse(env);

  if (!parsed.success) {
    console.error("ERRO FATAL: configuração inválida.");
    for (const issue of parsed.error.issues) {
      console.error(`  - ${issue.path.join(".") || "env"}: ${issue.message}`);
    }
    process.exit(1);
  }

  const publicBaseUrl = parsed.data.PUBLIC_BASE_URL?.replace(/\/+$/, "");

  return {
    databaseUrl: parsed.data.DATABASE_URL,
    databaseSsl:
      parsed.data.DATABASE_SSL === undefined ? undefined : parsed.data.DATABASE_SSL === "true",
    port: parsed.data.PORT,
    frontendOrigin: parsed.data.FRONTEND_ORIGIN,
    publicBaseUrl: publicBaseUrl || undefined,
  };
}
