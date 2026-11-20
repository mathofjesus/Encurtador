import path from "node:path";

import cors from "cors";
import express from "express";
import { z } from "zod";

import { buildCorsOptions } from "./lib/cors";
import { isValidCode } from "./lib/code";
import { isValidHttpUrl } from "./lib/url";
import { errorHandler } from "./middleware/error";
import { createShortUrl } from "./services/shorten";
import type { UrlRepository } from "./types";

export interface AppOptions {
  repo: UrlRepository;
  clock?: () => Date;
  frontendOrigin?: string;
  publicBaseUrl?: string;
  /** Diretório do build do Vite; quando ausente, `/` responde 404 JSON. */
  staticDir?: string;
}

const shortenSchema = z.object({ url: z.string() });

const INVALID_URL_MESSAGE = "URL inválida. Deve começar com http:// ou https://";

export function createApp(options: AppOptions): express.Express {
  const { repo, clock = () => new Date(), frontendOrigin = "*", publicBaseUrl } = options;

  const app = express();
  app.disable("x-powered-by");
  // Nginx envia X-Forwarded-*; sem isso, req.protocol/host ficam internos.
  app.set("trust proxy", 1);

  app.use(cors(buildCorsOptions(frontendOrigin)));
  app.use(express.json({ limit: "10kb" }));

  if (options.staticDir) {
    app.use(express.static(path.resolve(options.staticDir)));
  }

  app.get("/health", (_req, res) => {
    res.json({ status: "ok" });
  });

  app.post("/shorten", async (req, res) => {
    const parsed = shortenSchema.safeParse(req.body);
    const url = parsed.success ? parsed.data.url.trim() : "";

    if (!isValidHttpUrl(url)) {
      res.status(400).json({ error: INVALID_URL_MESSAGE });
      return;
    }

    const { code, expiresAt } = await createShortUrl(repo, url, clock());
    const host = req.get("host") ?? `localhost:${process.env.PORT ?? 5000}`;
    const baseUrl = publicBaseUrl ?? `${req.protocol}://${host}`;

    res.status(201).json({
      code,
      short_url: `${baseUrl}/${code}`,
      expires_at: expiresAt.toISOString(),
    });
  });

  // Catch-all de códigos: qualquer coisa com 7 caracteres base62 é tratada como link.
  app.get("/:code", async (req, res) => {
    const { code } = req.params;

    if (!isValidCode(code)) {
      res.status(404).json({ error: "Link não encontrado." });
      return;
    }

    const row = await repo.findByCode(code);
    if (!row) {
      res.status(404).json({ error: "Link não encontrado." });
      return;
    }

    if (row.expiresAt.getTime() <= clock().getTime()) {
      res.status(410).json({ error: "Este link expirou e não está mais disponível." });
      return;
    }

    // 302 de propósito: o navegador sempre passa pelo servidor.
    res.redirect(302, row.originalUrl);
  });

  app.use((_req, res) => {
    res.status(404).json({ error: "Não encontrado." });
  });

  app.use(errorHandler);

  return app;
}
