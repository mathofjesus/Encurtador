-- ============================================================
-- Limpeza automática com pg_cron (opcional)
-- Execute no Supabase SQL Editor se a extensão estiver disponível.
-- Alternativa sem pg_cron: agendar `npm run db:cleanup` no cron do host.
-- ============================================================

CREATE EXTENSION IF NOT EXISTS pg_cron;

SELECT cron.schedule(
    'limpar-urls-expiradas',
    '0 3 * * *',
    $$DELETE FROM urls WHERE expires_at < now()$$
);

-- Verificar: SELECT jobid, jobname, schedule, command, active FROM cron.job;
-- Remover:   SELECT cron.unschedule('limpar-urls-expiradas');
