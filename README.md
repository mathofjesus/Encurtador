# Encurtador de Links

Encurtador de URLs com backend Node.js + TypeScript, frontend Vite + TypeScript e PostgreSQL (Supabase). Deploy principal via Docker Compose (API + Nginx) em VPS; o frontend também pode ser publicado na Vercel.

## Stack

- **API**: Node.js 24, Express 5, `pg` (node-postgres), Zod
- **Frontend**: Vite 8 + TypeScript (vanilla, sem framework)
- **Banco**: PostgreSQL (Supabase transaction pooler na porta 6543)
- **Infra**: Docker Compose (API + Nginx), frontend estático embutido na imagem do Nginx
- **Testes**: Vitest + Supertest + jsdom

## Requisitos

- Node.js 24 (veja `.nvmrc` / `mise.toml`)
- npm 11+
- PostgreSQL acessível (local, container ou Supabase) — só para rodar a API de verdade
- Docker (opcional, para o deploy)

## Setup local

```bash
npm ci
cp .env.example .env        # ajuste DATABASE_URL
npm run db:init             # cria a tabela (ou rode sql/schema.sql no Supabase)
npm run dev                 # API em :5000 e frontend em :5173
```

- Frontend: http://localhost:5173 (proxy para `/shorten` e `/health`)
- API: http://localhost:5000 (`/health`, `POST /shorten`, `GET /:code`)

## Scripts

| Comando                                  | O que faz                                     |
| ---------------------------------------- | --------------------------------------------- |
| `npm run dev`                            | API (`tsx watch`) + Vite em paralelo          |
| `npm run dev:server` / `npm run dev:web` | Apenas um dos lados                           |
| `npm run build`                          | Compila API (`tsc`) e frontend (`vite build`) |
| `npm start`                              | Sobe a API compilada                          |
| `npm test`                               | Vitest em server e web                        |
| `npm run test:coverage`                  | Cobertura com thresholds (85% linhas/funções) |
| `npm run typecheck`                      | `tsc --noEmit` nos dois workspaces            |
| `npm run lint` / `npm run format`        | ESLint / Prettier                             |
| `npm run db:init`                        | Cria tabela + aplica migração de `code`       |
| `npm run db:backfill`                    | Preenche códigos em linhas antigas sem `code` |
| `npm run db:cleanup`                     | Remove URLs expiradas                         |

## Variáveis de ambiente

| Variável          | Obrigatória | Descrição                                                               |
| ----------------- | ----------- | ----------------------------------------------------------------------- |
| `DATABASE_URL`    | Sim         | Connection string do Postgres/Supabase                                  |
| `PORT`            | Não (5000)  | Porta interna da API                                                    |
| `FRONTEND_ORIGIN` | Não (`*`)   | CORS: `*`, uma origem ou lista separada por vírgula                     |
| `PUBLIC_BASE_URL` | Não         | Domínio público usado em `short_url`; sem isso usa o Host da requisição |
| `DATABASE_SSL`    | Não         | `true`/`false`; sem definir, ativa SSL automaticamente para Supabase    |

A API valida o ambiente no boot e encerra com mensagem clara se `DATABASE_URL` faltar.

## Banco de dados

- **Instalação nova**: `sql/schema.sql` (tabela `urls` com `code` único + índices).
- **Banco vindo da versão Flask**: `npm run db:init` (ou `db:backfill`) adiciona a coluna `code`, gera códigos e preserva as URLs/expirações existentes. **Os links antigos deixam de funcionar** (404), por decisão de migração.
- Limpeza automática opcional: `sql/cron.sql` (pg_cron) ou agende `npm run db:cleanup` no cron do host.
- **Não ative RLS** em `urls`: o backend conecta direto como `postgres` e sem policies tudo quebraria.

## Testes

```bash
npm test
npm run test:coverage
```

- `server/test`: geração de códigos, validação de URL/CORS, serviço de encurtamento (colisão/retry), rotas via Supertest com repositório em memória, migração/backfill e config.
- `web/test`: cliente HTTP e interface (jsdom), incluindo copiar link e estados de erro.
- Não há teste dependente de banco real: a suíte roda sem Postgres.

## Docker e deploy

```bash
cp .env.example .env
docker compose up -d --build
docker compose exec app node server/dist/scripts/init-db.js   # primeira vez / migração
```

- `Dockerfile`: multi-stage (build TS + Vite → runtime `node:24-alpine`, usuário `node`).
- `web/Dockerfile.nginx`: build do Vite embutido na imagem `nginx:alpine`.
- `deploy.sh`: valida `.env`, faz `git pull origin main`, rebuild e espera `/health`.
- O Nginx serve o build estático e repassa o resto para `app:5000`.

## Vercel (frontend)

- Projeto na raiz do repositório (config em `vercel.json`), build `npm run build -w web`, saída `web/dist`.
- Defina `VITE_API_BASE_URL=https://sua-api` nas Environment Variables.
- No backend, inclua a origem da Vercel em `FRONTEND_ORIGIN`.

## API

```http
GET  /health                 → {"status":"ok"}
POST /shorten  {"url": "https://..."}
                             → 201 {"code","short_url","expires_at"}
GET  /<code>                 → 302 para a URL original | 404 | 410 (expirado)
```

- Códigos: 7 caracteres base62 aleatórios (`crypto.randomInt`), persistidos na tabela com índice único.
- TTL: 30 dias.
- `POST /shorten` exige `http://` ou `https://`; retorna 400 com `{"error": ...}` caso contrário.
