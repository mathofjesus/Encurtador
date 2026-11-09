import sqlite3
import string
from flask import Flask, request, redirect, abort, jsonify

app = Flask(__name__)
DB_PATH = "encurtador.db"

# Alfabeto base62: 0-9, a-z, A-Z
BASE62 = string.digits + string.ascii_lowercase + string.ascii_uppercase


def encode(num: int) -> str:
    """Converte um inteiro (ID do banco) para string base62."""
    if num == 0:
        return BASE62[0]
    chars = []
    while num > 0:
        num, resto = divmod(num, 62)
        chars.append(BASE62[resto])
    return "".join(reversed(chars))


def decode(code: str) -> int:
    """Converte uma string base62 de volta para inteiro. Lança ValueError se inválida."""
    num = 0
    for c in code:
        num = num * 62 + BASE62.index(c)
    return num


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


@app.post("/shorten")
def shorten():
    data = request.get_json(silent=True) or {}
    url = data.get("url", "").strip()

    if not url.startswith(("http://", "https://")):
        return jsonify(error="URL inválida. Use http:// ou https://"), 400

    with get_db() as db:
        cur = db.execute("INSERT INTO urls (original_url) VALUES (?)", (url,))
        code = encode(cur.lastrowid)

    return jsonify(short_url=f"{request.host_url}{code}", code=code), 201


@app.get("/<code>")
def resolve(code: str):
    try:
        row_id = decode(code)
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
