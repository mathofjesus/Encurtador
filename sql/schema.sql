-- ============================================================
-- Schema do Encurtador de Links — Supabase (PostgreSQL)
-- Executar no: Supabase Dashboard → SQL Editor → New Query
-- ============================================================

-- 1. Tabela principal
CREATE TABLE IF NOT EXISTS urls (
    id           BIGSERIAL    PRIMARY KEY,
    original_url TEXT         NOT NULL,
    created_at   TIMESTAMPTZ  NOT NULL DEFAULT now(),
    expires_at   TIMESTAMPTZ  NOT NULL
);

-- 2. Índice para limpeza eficiente de links expirados (DELETE por expires_at)
--    e para a verificação de expiração na rota GET /<code>.
CREATE INDEX IF NOT EXISTS idx_urls_expires_at ON urls (expires_at);

-- ============================================================
-- Limpeza automática com pg_cron
-- (disponível no Supabase free e paid tiers)
-- ============================================================

-- 3. Ativar a extensão pg_cron
CREATE EXTENSION IF NOT EXISTS pg_cron;

-- 4. Agendar limpeza diária às 03:00 UTC
--    Remove todas as linhas com expires_at no passado.
--    O índice idx_urls_expires_at torna este DELETE eficiente.
SELECT cron.schedule(
    'limpar-urls-expiradas',          -- nome único do job
    '0 3 * * *',                       -- cron: todo dia às 03:00 UTC
    $$DELETE FROM urls WHERE expires_at < now()$$
);

-- Para verificar que o job foi criado:
-- SELECT jobid, jobname, schedule, command, active FROM cron.job;

-- Para remover o job (se necessário):
-- SELECT cron.unschedule('limpar-urls-expiradas');

-- ============================================================
-- Notas sobre Row Level Security (RLS)
-- ============================================================
-- NÃO ative RLS na tabela urls.
-- O backend Flask se conecta como role 'postgres' via connection string
-- direta, não usa a API REST pública do Supabase (anon/service key).
-- RLS é relevante apenas para acesso via API REST do Supabase.
-- Ativá-lo sem policies bloquearia todas as operações do backend.
