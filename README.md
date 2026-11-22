# 🔗 Encurtador de Links

Um serviço moderno e performático de encurtamento de URLs construído em **Python (Flask)**, **PostgreSQL (Supabase)**, **Docker Compose** e **Nginx**.

Utiliza cifra de permutação de **Feistel (HMAC-SHA256)** associada à codificação **Base62 de tamanho fixo (7 caracteres)** para gerar códigos não sequenciais, imprevisíveis e sem colisões, com suporte a mais de 1 trilhão de combinações e expiração automática de links.

---

## 🚀 Tecnologias

- **Backend:** Python 3.12, Flask, Gunicorn
- **Banco de Dados:** PostgreSQL (Supabase) via `psycopg3` com `ConnectionPool` otimizado para Transaction Pooler (Supavisor)
- **Frontend:** HTML5, CSS3 e JavaScript Vanilla (`frontend/index.html`)
- **Infraestrutura & Containers:** Docker, Docker Compose, Nginx (Alpine Linux)
- **CI/CD & Automação:** GitHub Actions (Lint, Testes e Limpeza agendada)

---

## 📐 Arquitetura do Encurtador

```mermaid
flowchart LR
    User[Cliente / Navegador] -->|HTTP / HTTPS| Nginx[Nginx :80/:443]
    Nginx -->|Arquivos estáticos| Static[frontend/index.html]
    Nginx -->|API & Redirecionamento| App[Flask App :5000]
    App -->|psycopg pool| DB[(Supabase Postgres)]
```

### Algoritmo de Geração de Códigos
1. Ao salvar a URL original na tabela `urls`, o PostgreSQL atribui um `BIGSERIAL` auto-incrementado.
2. O ID inteiro passa por **6 rodadas de Permutação de Feistel** com chave simétrica (`ENCURTADOR_KEY`), embaralhando o valor de forma reversível.
3. O número resultante é codificado em **Base62** preenchendo exatamente 7 caracteres (`[0-9a-zA-Z]`).
4. Na resolução (`GET /<code>`), o processo inverso ocorre: o código é decodificado de Base62 para inteiro e a cifra de Feistel é revertida para obter diretamente o `id` da linha no banco, mantendo as consultas rápidas em $O(1)$ por chave primária indexada.

---

## ⚙️ Variáveis de Ambiente

Copie o arquivo `.env.example` para `.env` e configure as credenciais:

```bash
cp .env.example .env
```

| Variável | Descrição | Exemplo |
|---|---|---|
| `DATABASE_URL` | URL de conexão PostgreSQL (porta 6543 no Supabase Pooler) | `postgresql://postgres.[REF]:[SENHA]@aws-0-sa-east-1.pooler.supabase.co:6543/postgres` |
| `ENCURTADOR_KEY` | Chave secreta de alta entropia para a permutação de Feistel | `python3 -c "import secrets; print(secrets.token_hex(32))"` |
| `FRONTEND_ORIGIN` | Origens aceitas pelo CORS (`*` ou URLs separadas por vírgula) | `*` ou `https://meudominio.com.br` |

---

## 🛠️ Instalação e Execução Local

### Pré-requisitos
- Python 3.12+
- Docker & Docker Compose (opcional para rodar com containers)

### 1. Rodando localmente com Python

```bash
# Criar e ativar ambiente virtual
python3 -m venv .venv
source .venv/bin/activate  # No Windows: .venv\Scripts\activate

# Instalar dependências
pip install -r requirements.txt

# Configurar variáveis de ambiente
export DATABASE_URL="sua_string_de_conexao"
export ENCURTADOR_KEY="sua_chave_secreta"
export FRONTEND_ORIGIN="*"

# Inicializar o banco de dados (cria tabela e índices)
flask init-db

# Iniciar o servidor de desenvolvimento
python app.py
```

Acesse em: `http://localhost:5000`

---

## 🐳 Executando com Docker Compose

O projeto inclui configuração pronta para produção com Nginx como proxy reverso e Flask em container Alpine Linux.

```bash
# Subir os containers em segundo plano
docker compose up -d --build

# Verificar logs
docker compose logs -f

# Parar serviços
docker compose down
```

---

## 🚢 Deploy em Produção

### Opção 1: VPS Hostinger / Debian (Script Automatizado)

O projeto possui o script `deploy.sh` que atualiza o código, instala Docker se necessário e recria os containers:

```bash
chmod +x deploy.sh
./deploy.sh
```

### Opção 2: Railway

O repositório já conta com `Procfile` e `runtime.txt`. Basta:
1. Conectar o repositório na Railway.
2. Definir as variáveis `DATABASE_URL` e `ENCURTADOR_KEY` nas configurações de ambiente.
3. O Railway iniciará automaticamente via Gunicorn na porta dinâmica `$PORT`.

---

## 📡 Endpoints da API

| Método | Rota | Descrição | Status de Retorno |
|---|---|---|---|
| `GET` | `/` | Interface web do encurtador | `200 OK` |
| `GET` | `/health` | Verificação de integridade (*health check*) | `200 OK` |
| `POST` | `/shorten` | Cria uma URL encurtada com TTL de 30 dias | `201 Created` / `400 Bad Request` |
| `GET` | `/{code}` | Redireciona para o link original | `302 Found` / `404 Not Found` / `410 Gone` |

### Exemplo de criação de link:

```bash
curl -X POST http://localhost:5000/shorten \
  -H "Content-Type: application/json" \
  -d '{"url": "https://github.com/example/repo"}'
```

Resposta:
```json
{
  "code": "aB3x9K1",
  "short_url": "http://localhost:5000/aB3x9K1",
  "expires_at": "2026-12-22T18:00:00+00:00"
}
```

---

## 🧹 Limpeza de Links Expirados

Links têm validade de **30 dias**. Existem três formas de expurgo:

1. **pg_cron no Supabase:** Execute `sql/schema.sql` ou `sql/cron.sql` no SQL Editor do Supabase para agendamento nativo às 03:00 UTC.
2. **GitHub Actions (`cleanup.yml`):** Executa `cleanup.py` diariamente às 03:00 UTC via Actions. Requer o secret `DATABASE_URL` configurado no repositório.
3. **Execução Manual:**
   ```bash
   DATABASE_URL="sua_conexao" python cleanup.py
   ```

---

## 🤖 GitHub Actions (CI & Workflows)

- **`CI` (`.github/workflows/ci.yml`):** Executado em todo `push` ou `pull_request` na branch `main`. Realiza validação de linting com `ruff` e executa testes automatizados com `pytest`.
- **`Cleanup` (`.github/workflows/cleanup.yml`):** Job agendado diariamente para remover links expirados e acionável manualmente via `workflow_dispatch`.

---

## 📁 Estrutura de Diretórios

```text
├── .github/
│   └── workflows/
│       ├── ci.yml               # Workflow de validação contínua (Lint & Testes)
│       └── cleanup.yml          # Workflow de limpeza agendada de links expirados
├── frontend/
│   ├── index.html               # Interface web responsiva
│   └── vercel.json              # Configuração opcional para Vercel
├── nginx/
│   └── default.conf             # Configuração de proxy reverso e gzip
├── sql/
│   ├── schema.sql               # Schema DDL e agendamento pg_cron
│   ├── cron.sql                 # Script isolado de cron
│   └── migrations/              # Histórico de migrações
├── app.py                       # Aplicação principal Flask e rotas
├── cleanup.py                   # Script avulso de expurgo de links expirados
├── deploy.sh                    # Script de deploy para VPS
├── docker-compose.yml           # Orquestração Nginx + App Flask
├── Dockerfile                   # Imagem Alpine otimizada com usuário não-root
├── requirements.txt             # Dependências Python fixadas
├── runtime.txt                  # Versão Python para plataformas PaaS
├── Procfile                     # Comando de inicialização Gunicorn para PaaS
├── .gitignore                   # Regras de exclusão do controle de versão
├── .env.example                 # Modelo de variáveis de ambiente
└── README.md                    # Documentação do projeto
```
