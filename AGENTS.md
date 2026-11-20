# AGENTS.md

Encurtador de links: monorepo npm workspaces com API Express 5 + TypeScript (`server/`), frontend Vite + TS (`web/`) e PostgreSQL (Supabase). Deploy via Docker Compose (API + Nginx) em VPS; o frontend também é publicado na Vercel. Código, mensagens e UI em português — mantenha o idioma.

## Comandos

```bash
npm ci                     # instala todos os workspaces (root)
cp .env.example .env       # DATABASE_URL é obrigatória
npm run db:init            # cria tabela + migração (ou sql/schema.sql no Supabase)
npm run dev                # API :5000 + Vite :5173 (proxy /shorten e /health)
npm run dev:server         # só a API (tsx watch)
npm test                   # Vitest: server + web
npm run test:coverage      # thresholds 85% (linhas/funções)
npm run lint && npm run typecheck && npm test && npm run build   # verificação completa
```

- Teste único: `npm run test -w server -- test/app.test.ts` (ou `-t "nome do teste"`).
- `npm run db:cleanup` remove expirados; `npm run db:backfill` preenche `code` em linhas antigas.
- Deploy: `./deploy.sh` (valida `.env`, `git pull origin main`, rebuild e espera `/health`).

## Pegadinhas

- **Env é validado no boot** (`server/src/config.ts`, Zod): sem `DATABASE_URL` o processo chama `process.exit(1)`. Só `server.ts`/scripts carregam config; `createApp()` é injetável e testável sem env.
- **Códigos são aleatórios e persistidos** (`urls.code`, 7 chars base62, `crypto.randomInt`). Colisão é tratada por retry em `23505` — não troque para `randomBytes() % 62` (viés) nem remova o índice único. Links gerados pelo backend Flask antigo **não resolvem mais** (decisão de migração); o backfill só preserva as URLs no banco.
- **Pooler do Supabase (6543)**: nunca use `name` em `pool.query` (prepared statements nomeados quebram no Supavisor). `max: 5`.
- **SSL**: `createPool` ativa TLS automaticamente para host `*.supabase.co` ou porta 6543; `DATABASE_SSL=true|false` força o comportamento. Postgres local via Docker (`host.docker.internal`) **não** deve receber SSL.
- **`trust proxy: 1`** no Express e `X-Forwarded-*` no Nginx: sem isso `short_url` sai com host/protocolo internos. `PUBLIC_BASE_URL` sobrescreve a base.
- **Rota catch-all `GET /:code`**: só aceita exatamente 7 caracteres `[0-9A-Za-z]`; o resto cai no 404 JSON. Novas rotas públicas precisam de nomes que não colidam com códigos.
- **Frontend real é `web/`** (build em `web/dist`). O Nginx serve o build embutido na imagem; a API tem fallback via `express.static` para quando acessada direto. Não existe mais `templates/`.
- **Não ative RLS em `urls`** (ver `sql/schema.sql`): o backend conecta como `postgres`, sem policies.
- `expires_at` sai como ISO com `Z` (JS); o front usa `new Date()` e formata em pt-BR. Expiração responde **410**, redirect é **302** de propósito.
- Node é 24 (`.nvmrc`/Docker). `npm ci --omit=dev` no runtime; `tsx` é devDependency — em produção rode `node server/dist/scripts/<script>.js`.

## Estrutura

- `server/src/app.ts` — `createApp({ repo, clock, cors, publicBaseUrl, staticDir })`; rotas e middlewares.
- `server/src/services/shorten.ts` — TTL de 30 dias + retry de colisão.
- `server/src/repositories/urls.ts` — queries SQL; interface em `types.ts` permite fake nos testes.
- `server/src/db/migrate.ts` — migração idempotente do schema antigo (add `code`, backfill, unique, NOT NULL).
- `server/src/scripts/` — `init-db`, `backfill-codes`, `cleanup`.
- `web/src/api.ts` — `shortenUrl(url, baseUrl)`; base vem de `VITE_API_BASE_URL` (vazio = relativo).
- `vercel.json` na raiz (projeto = raiz do repo, build `-w web`, saída `web/dist`).
- `nginx/default.conf` — `try_files` → estático → proxy `app:5000`.
