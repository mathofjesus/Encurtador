import path from "node:path";

import { createApp } from "./app";
import { loadConfig } from "./config";
import { createPool } from "./db/pool";
import { PgUrlRepository } from "./repositories/urls";

const config = loadConfig();
const pool = createPool(config.databaseUrl, config.databaseSsl);
const staticDir = path.resolve(process.cwd(), "web/dist");

const app = createApp({
  repo: new PgUrlRepository(pool),
  frontendOrigin: config.frontendOrigin,
  publicBaseUrl: config.publicBaseUrl,
  staticDir,
});

const server = app.listen(config.port, () => {
  console.log(`Encurtador API ouvindo em http://0.0.0.0:${config.port}`);
});

async function shutdown(signal: string): Promise<void> {
  console.log(`Recebido ${signal}, encerrando...`);
  server.close(async () => {
    await pool.end();
    process.exit(0);
  });
}

process.on("SIGTERM", () => void shutdown("SIGTERM"));
process.on("SIGINT", () => void shutdown("SIGINT"));
