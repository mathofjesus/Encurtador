import hashlib
import hmac
import os
import sqlite3
import string
from flask import Flask, request, redirect, abort, jsonify

app = Flask(__name__)
DB_PATH = "encurtador.db"

# Alfabeto base62: 0-9, a-z, A-Z
BASE62 = string.digits + string.ascii_lowercase + string.ascii_uppercase
CODE_LEN =  7

# Domínio de  40 bits: 2⁴0 = 1,1 trilhão de IDs, mais que os 3,65 x 10^11
# previstos em 10 anos. Cabe em 7 caracteres base 62 (62^7 = 3,5 x 10^12).
HALF_BITS = 20
MASK = (1 << HALF_BITS) - 1
DOMAIN_SIZE = 1 << (2 * HALF_BITS)
ROUNDS = 6

SECRET_KEY = os.environ.get("ENCURTADOR_KEY", "").encode("utf-8")
if not SECRET_KEY:
    raise RuntimeError("Defina a variavel de ambiente ENCURTADOR_KEY antes de iniciar.")

def _round_fn(half: int, rnd: int) -> int:
    """Função de rodada: HMAC da metade com a a chave secreta, truncado a 20 bits."""
    digest = hmac.new(SECRET_KEY, f"{rnd}:{half}".encode("utf-8"), hashlib.sha256).digest()
    return int.from_bytes(digest[:8], "big") & MASK

def feistel_encrypt(n: int) -> int:
    left, right = n >> HALF_BITS, n & MASK
    for rnd in range(ROUNDS):
        left, right = right, left ^_round_fn(right, rnd)
    return (left << HALF_BITS) | right

def feistel_decrypt(n: int) -> int:
    left, right = n >> HALF_BITS, n & MASK
    for rnd in reversed(range(ROUNDS)):
        left, right = right ^ _round_fn(left, rnd), left
    return (left << HALF_BITS) | right

# --- Base62 com largura fixa ---
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


# --- Banco ---

def get_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db() -> None:
    with get_db() as db:
        db.execute("""
            CREATE TABLE IF NOT EXISTS urls (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                original_url TEXT    NOT NULL,
                created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """)


# --- Rotas ---

@app.post("/shorten")
def shorten():
    data = request.get_json(silent=True) or {}
    url = data.get("url", "").strip()

    if not url.startswith(("http://", "https://")):
        return jsonify(error="URL inválida. Deve começar com http:// ou https://"), 400

    with get_db() as db:
        cur = db.execute("INSERT INTO urls (original_url) VALUES (?)", (url,))
        row_id = cur.lastrowid

    if row_id >= DOMAIN_SIZE:
        return jsonify(error="Capacidade de códigos esgotada"), 507

    code = id_to_code(row_id)
    return jsonify(short_url=f"{request.host_url}{code}", code=code), 201

@app.get("/<code>")
def resolve(code: str):
    try:
        row_id = code_to_id(code)
    except ValueError:
        abort(404)

    with get_db() as db:
        row = db.execute(
            "SELECT original_url FROM urls WHERE id = ?", (row_id,)
        ).fetchone()

    if row is None:
        abort(404)

    # 302: o navegador sempre passa pelo servidor, o que permite contar acessos.
    return redirect(row["original_url"], code=302)


if __name__ == "__main__":
    init_db()
    app.run(debug=True, port=5000)
