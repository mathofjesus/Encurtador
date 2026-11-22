"""
Limpeza manual de URLs expiradas.

Alternativa ao pg_cron quando a extensão não está disponível.
Pode ser agendado como:
  - Cron Job na Railway (em serviço separado)
  - Crontab no servidor
  - GitHub Actions com schedule

Uso:
    DATABASE_URL=<sua_url> python cleanup.py

Exemplo de agendamento como Cron Job na Railway:
    Command: python cleanup.py
    Schedule: 0 3 * * *  (todo dia às 03:00 UTC)
"""

import os
import sys

import psycopg

DATABASE_URL = os.environ.get("DATABASE_URL")
if not DATABASE_URL:
    print("ERRO: variável DATABASE_URL não definida.", file=sys.stderr)
    sys.exit(1)

# prepare_threshold=None: obrigatório para Supabase transaction pooler
with psycopg.connect(DATABASE_URL, prepare_threshold=None) as conn:
    cur = conn.execute("DELETE FROM urls WHERE expires_at < now()")
    conn.commit()
    print(f"Limpeza concluída: {cur.rowcount} URLs expiradas removidas.")
