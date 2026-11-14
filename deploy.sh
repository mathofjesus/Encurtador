#!/usr/bin/env bash
# Script de Deploy e Atualização — Hostinger VPS (Debian + Docker Compose + Alpine)
# Execução: ./deploy.sh

set -e

echo "=== [1/4] Verificando Docker e Docker Compose no Debian ==="
if ! command -v docker &> /dev/null; then
    echo "Docker não encontrado. Instalando no Debian..."
    apt-get update && apt-get install -y docker.io docker-compose-v2 git
fi

echo "=== [2/4] Verificando arquivo .env ==="
if [ ! -f .env ]; then
    echo "ERRO: Arquivo .env não encontrado!"
    echo "Crie o arquivo .env a partir do .env.example:"
    echo "  cp .env.example .env"
    echo "  nano .env"
    exit 1
fi

echo "=== [3/4] Atualizando código e construindo imagens Alpine ==="
git pull origin main || echo "Aviso: git pull não executado ou já atualizado."
docker compose down || true
docker compose up -d --build

echo "=== [4/4] Status dos containers Alpine ==="
docker compose ps

echo ""
echo "✅ Aplicação em execução com sucesso na Hostinger VPS!"
echo "Teste com: curl -I http://localhost/health"
