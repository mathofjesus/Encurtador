#!/usr/bin/env bash
# Script de Deploy e Atualização — VPS (Debian + Docker Compose)
# Execução: ./deploy.sh
set -euo pipefail

echo "=== [1/4] Verificando Docker e Docker Compose ==="
if ! command -v docker &>/dev/null; then
    echo "Docker não encontrado. Instalando no Debian..."
    apt-get update && apt-get install -y docker.io docker-compose-v2 git curl
fi

echo "=== [2/4] Verificando arquivo .env ==="
if [ ! -f .env ]; then
    echo "ERRO: Arquivo .env não encontrado!"
    echo "Crie o arquivo .env a partir do .env.example:"
    echo "  cp .env.example .env"
    echo "  nano .env"
    exit 1
fi

echo "=== [3/4] Atualizando código e construindo imagens ==="
git pull origin main || echo "Aviso: git pull não executado ou já atualizado."
docker compose down || true
docker compose up -d --build

echo "=== [4/4] Aguardando health check ==="
for _ in $(seq 1 30); do
    if curl -fsS http://localhost/health >/dev/null 2>&1; then
        break
    fi
    sleep 2
done

if ! curl -fsS http://localhost/health >/dev/null 2>&1; then
    echo "ERRO: /health não respondeu. Logs:"
    docker compose logs --tail=50 app
    exit 1
fi

docker compose ps
echo ""
echo "✅ Aplicação em execução com sucesso!"
