#!/usr/bin/env bash
# Script de Deploy e Atualização — AWS Lightsail (Debian 13)
# Execução: ./deploy.sh

set -e

echo "=== [1/4] Verificando arquivo .env ==="
if [ ! -f .env ]; then
    echo "ERRO: Arquivo .env não encontrado!"
    echo "Crie o arquivo .env a partir do .env.example antes de continuar:"
    echo "  cp .env.example .env"
    echo "  nano .env"
    exit 1
fi

echo "=== [2/4] Atualizando repositório (git pull) ==="
git pull origin main || echo "Aviso: git pull não executado ou já atualizado."

echo "=== [3/4] Construindo e subindo containers (Docker Compose) ==="
docker compose down || true
docker compose up -d --build

echo "=== [4/4] Verificando status dos serviços ==="
docker compose ps

echo ""
echo "✅ Aplicação em execução com sucesso!"
echo "Teste localmente com: curl http://localhost/health"
