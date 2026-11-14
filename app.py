"""
Encurtador de Links — Backend Flask
Deploy: Railway (gunicorn -w 2 -b 0.0.0.0:$PORT app:app)
"""

import hashlib
import hmac
import os
import string
import sys
from datetime import datetime, timedelta, timezone

import psycopg
from flask import Flask, request, redirect, abort, jsonify
from flask_cors import CORS
from psycopg_pool import ConnectionPool

# ─── Configuração ────────────────────────────────────────────────────

app = Flask(__name__)

# Variáveis obrigatórias — falham explicitamente na inicialização
DATABASE_URL = os.environ.get("DATABASE_URL")
if not DATABASE_URL:
    print("ERRO FATAL: Defina a variável de ambiente DATABASE_URL.", file=sys.stderr)
    sys.exit(1)

SECRET_KEY = os.environ.get("ENCURTADOR_KEY", "").encode("utf-8")
if not SECRET_KEY:
    print("ERRO FATAL: Defina a variável de ambiente ENCURTADOR_KEY.", file=sys.stderr)
    sys.exit(1)

# Origem do frontend na Vercel — CORS restrito (nunca "*")
FRONTEND_ORIGIN = os.environ.get(
    "FRONTEND_ORIGIN", "https://<SEU-PROJETO>.vercel.app"
)
CORS(app, origins=[FRONTEND_ORIGIN])

# ─── Pool de conexões ────────────────────────────────────────────────
# Por que pool em vez de conexão por requisição?
#   - Evita overhead de TLS + autenticação a cada request.
#   - Com 2 workers Gunicorn, min=1/max=5 por worker é suficiente.
#   - prepare_threshold=None é OBRIGATÓRIO para Supabase transaction pooler
#     (Supavisor, porta 6543) — sem isso psycopg usa prepared statements
#     que o pooler rejeita com erros de protocolo.
pool = ConnectionPool(
    conninfo=DATABASE_URL,
    min_size=1,
    max_size=5,
    kwargs={"prepare_threshold": None},
)

# ─── Constantes Feistel / Base62 ─────────────────────────────────────

BASE62 = string.digits + string.ascii_lowercase + string.ascii_uppercase
CODE_LEN = 7

# Domínio de 40 bits: 2^40 ≈ 1,1 trilhão de IDs
# Cabe em 7 caracteres base62 (62^7 ≈ 3,5 trilhões).
# Em 10 anos a 100M/dia = 365 bilhões de IDs — dentro do domínio.
HALF_BITS = 20
MASK = (1 << HALF_BITS) - 1
DOMAIN_SIZE = 1 << (2 * HALF_BITS)
ROUNDS = 6

LINK_TTL_DAYS = 30


# ─── Feistel (preservada integralmente) ─────────────────────────────

def _round_fn(half: int, rnd: int) -> int:
    """Função de rodada: HMAC da metade com a chave secreta, truncado a 20 bits."""
    digest = hmac.new(
        SECRET_KEY, f"{rnd}:{half}".encode("utf-8"), hashlib.sha256
    ).digest()
    return int.from_bytes(digest[:8], "big") & MASK


def feistel_encrypt(n: int) -> int:
    left, right = n >> HALF_BITS, n & MASK
    for rnd in range(ROUNDS):
        left, right = right, left ^ _round_fn(right, rnd)
    return (left << HALF_BITS) | right


def feistel_decrypt(n: int) -> int:
    left, right = n >> HALF_BITS, n & MASK
    for rnd in reversed(range(ROUNDS)):
        left, right = right ^ _round_fn(left, rnd), left
    return (left << HALF_BITS) | right


# ─── Base62 com largura fixa (preservada integralmente) ──────────────

def encode(num: int) -> str:
    chars = []
    for _ in range(CODE_LEN):
        num, resto = divmod(num, 62)
        chars.append(BASE62[resto])
    return "".join(reversed(chars))


def decode(code: str) -> int:
    """Lança ValueError se o código tiver tamanho errado ou caracteres inválidos."""
    if len(code) != CODE_LEN:
        raise ValueError("tamanho inválido")
    num = 0
    for c in code:
        num = num * 62 + BASE62.index(c)
    return num


def id_to_code(row_id: int) -> str:
    return encode(feistel_encrypt(row_id))


def code_to_id(code: str) -> int:
    value = decode(code)
    if value >= DOMAIN_SIZE:
        raise ValueError("fora do domínio")
    return feistel_decrypt(value)


# ─── Inicialização do banco (comando CLI, não na importação) ─────────

@app.cli.command("init-db")
def init_db_command():
    """Cria a tabela urls e o índice se não existirem.
    Use: flask init-db
    Só precisa ser executado uma vez (ou o SQL do schema.sql via Supabase).
    """
    with pool.connection() as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS urls (
                id           BIGSERIAL    PRIMARY KEY,
                original_url TEXT         NOT NULL,
                created_at   TIMESTAMPTZ  NOT NULL DEFAULT now(),
                expires_at   TIMESTAMPTZ  NOT NULL
            )
        """)
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_urls_expires_at
            ON urls (expires_at)
        """)
        conn.commit()
    print("Banco inicializado com sucesso.")


# ─── Rotas ───────────────────────────────────────────────────────────

@app.get("/health")
def health():
    """Health check para Railway e outros monitores."""
    return jsonify(status="ok"), 200


@app.post("/shorten")
def shorten():
    data = request.get_json(silent=True) or {}
    url = data.get("url", "").strip()

    if not url.startswith(("http://", "https://")):
        return jsonify(error="URL inválida. Deve começar com http:// ou https://"), 400

    now = datetime.now(timezone.utc)
    expires_at = now + timedelta(days=LINK_TTL_DAYS)

    with pool.connection() as conn:
        # RETURNING id: obtém o BIGSERIAL gerado sem lastrowid (psycopg3 / PostgreSQL)
        row = conn.execute(
            """INSERT INTO urls (original_url, created_at, expires_at)
               VALUES (%s, %s, %s)
               RETURNING id""",
            (url, now, expires_at),
        ).fetchone()
        conn.commit()

    row_id = row[0]

    if row_id >= DOMAIN_SIZE:
        return jsonify(error="Capacidade de códigos esgotada"), 507

    code = id_to_code(row_id)
    return jsonify(
        code=code,
        short_url=f"{request.host_url}{code}",
        expires_at=expires_at.isoformat(),
    ), 201


@app.get("/<code>")
def resolve(code: str):
    try:
        row_id = code_to_id(code)
    except ValueError:
        abort(404)

    with pool.connection() as conn:
        row = conn.execute(
            "SELECT original_url, expires_at FROM urls WHERE id = %s",
            (row_id,),
        ).fetchone()

    if row is None:
        abort(404)

    original_url, expires_at = row

    if expires_at < datetime.now(timezone.utc):
        # 410 Gone: o recurso existiu mas não está mais disponível
        return jsonify(error="Este link expirou e não está mais disponível."), 410

    # 302: o navegador sempre passa pelo servidor (permite contar acessos futuramente)
    return redirect(original_url, code=302)


# ─── Desenvolvimento local ────────────────────────────────────────────

if __name__ == "__main__":
    # Em produção, o Gunicorn ignora este bloco.
    # Debug=True apenas em ambiente de desenvolvimento.
    app.run(debug=True, port=5000)
