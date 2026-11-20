-- ============================================================
-- Schema do Encurtador de Links — PostgreSQL (Supabase)
-- Instalação nova: execute este arquivo no SQL Editor.
-- Banco existente (schema antigo sem `code`): use `npm run db:init`
-- ou `npm run db:backfill`, que aplicam a migração com backfill.
-- ============================================================

-- 1. Tabela principal
CREATE TABLE IF NOT EXISTS urls (
    id           BIGSERIAL    PRIMARY KEY,
    code         TEXT         NOT NULL,
    original_url TEXT         NOT NULL,
    created_at   TIMESTAMPTZ  NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ  NOT NULL
);

-- 2. Código curto único (7 caracteres base62, aleatório)
CREATE UNIQUE INDEX IF NOT EXISTS idx_urls_code ON urls (code);

-- 3. Índice para limpeza eficiente de links expirados e checagem de expiração
CREATE INDEX IF NOT EXISTS idx_urls_expires_at ON urls (expires_at);

-- ============================================================
-- Notas sobre Row Level Security (RLS)
-- ============================================================
-- NÃO ative RLS na tabela urls.
-- O backend se conecta como role 'postgres' via connection string direta,
-- não usa a API REST pública do Supabase (anon/service key).
-- RLS é relevante apenas para acesso via API REST; ativá-lo sem policies
-- bloquearia todas as operações do backend.
