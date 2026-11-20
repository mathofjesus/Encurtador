-- ============================================================
-- Migração: adiciona `code` à tabela `urls` existente.
--
-- Passo 1 (este arquivo) é idempotente e pode ser executado
-- antes do backfill. O backfill dos códigos é feito pelo script
-- Node (que gera base62 aleatório e respeita o índice único):
--
--     npm run db:backfill
--
-- O script executa os mesmos passos no fim (SET NOT NULL).
-- Alternativa: `npm run db:init` faz tudo de uma vez.
-- ============================================================

ALTER TABLE urls ADD COLUMN IF NOT EXISTS code TEXT;

-- Vários NULL são permitidos em índice único, então é seguro
-- criá-lo antes do backfill (evita corrida entre códigos gerados).
CREATE UNIQUE INDEX IF NOT EXISTS idx_urls_code ON urls (code);

-- Conferir linhas ainda sem código:
-- SELECT count(*) FROM urls WHERE code IS NULL;
