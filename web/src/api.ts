export interface ShortenResult {
  code: string;
  short_url: string;
  expires_at: string;
}

export class ApiError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ApiError";
  }
}

function isShortenResult(value: unknown): value is ShortenResult {
  if (typeof value !== "object" || value === null) {
    return false;
  }
  const record = value as Record<string, unknown>;
  return (
    typeof record.code === "string" &&
    typeof record.short_url === "string" &&
    typeof record.expires_at === "string"
  );
}

function extractError(value: unknown): string | undefined {
  if (typeof value === "object" && value !== null && "error" in value) {
    const message = (value as { error?: unknown }).error;
    if (typeof message === "string") {
      return message;
    }
  }
  return undefined;
}

/**
 * Chama POST /shorten.
 * `baseUrl` vazio usa rotas relativas (VPS/local); na Vercel, defina VITE_API_BASE_URL.
 */
export async function shortenUrl(url: string, baseUrl = ""): Promise<ShortenResult> {
  let response: Response;
  try {
    response = await fetch(`${baseUrl}/shorten`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url }),
    });
  } catch {
    throw new ApiError("Não foi possível conectar ao servidor.");
  }

  const data: unknown = await response.json().catch(() => null);

  if (!response.ok) {
    throw new ApiError(extractError(data) ?? "Erro ao encurtar o link.");
  }

  if (!isShortenResult(data)) {
    throw new ApiError("Resposta inválida do servidor.");
  }

  return data;
}
