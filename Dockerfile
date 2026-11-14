FROM python:3.12-slim

# Evita arquivos .pyc e força flush imediato de logs
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

# Instala dependências
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copia todo o código da aplicação
COPY . .

# Executa com usuário não-root por segurança
RUN useradd -m appuser && chown -R appuser:appuser /app
USER appuser

EXPOSE 5000

# Gunicorn com 2 workers, escutando em todas as interfaces na porta 5000
CMD ["gunicorn", "-w", "2", "-b", "0.0.0.0:5000", "--access-logfile", "-", "--error-logfile", "-", "app:app"]
