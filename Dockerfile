# ─── Dependências (cache de camada) ───────────────────────────────────
FROM node:24-alpine AS deps
WORKDIR /app
COPY package.json package-lock.json ./
COPY server/package.json server/package.json
COPY web/package.json web/package.json
RUN npm ci

# ─── Build (API + frontend) ───────────────────────────────────────────
FROM deps AS build
WORKDIR /app
COPY tsconfig.base.json ./
COPY server server
COPY web web
RUN npm run build

# ─── Runtime ──────────────────────────────────────────────────────────
FROM node:24-alpine AS runtime
ENV NODE_ENV=production \
    PORT=5000
WORKDIR /app

COPY package.json package-lock.json ./
COPY server/package.json server/package.json
COPY web/package.json web/package.json
RUN npm ci --omit=dev && npm cache clean --force

COPY --from=build /app/server/dist server/dist
COPY --from=build /app/web/dist web/dist

# Mesmo usuário não-root do Node (uid 1000)
USER node

EXPOSE 5000

# Processo único: escala por réplicas do Compose, não por workers.
CMD ["node", "server/dist/server.js"]
