import "./style.css";
import { ApiError, shortenUrl } from "./api";

function byId<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Elemento #${id} não encontrado`);
  }
  return element as T;
}

const form = byId<HTMLFormElement>("form");
const input = byId<HTMLInputElement>("url");
const botao = byId<HTMLButtonElement>("botao");
const resultado = byId<HTMLDivElement>("resultado");
const erro = byId<HTMLDivElement>("erro");

const API_BASE_URL = (import.meta.env.VITE_API_BASE_URL ?? "").replace(/\/+$/, "");

function formatExpiry(isoDate: string): string {
  const expiresAt = new Date(isoDate);
  const data = expiresAt.toLocaleDateString("pt-BR");
  const hora = expiresAt.toLocaleTimeString("pt-BR", { hour: "2-digit", minute: "2-digit" });
  return `⏳ Expira em: ${data} às ${hora}`;
}

function createCopyButton(shortUrl: string): HTMLButtonElement {
  const button = document.createElement("button");
  button.type = "button";
  button.textContent = "📋 Copiar link";
  button.className = "copiar";

  button.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(shortUrl);
      button.textContent = "✅ Copiado!";
    } catch {
      button.textContent = "❌ Falha ao copiar";
    }
    setTimeout(() => {
      button.textContent = "📋 Copiar link";
    }, 2000);
  });

  return button;
}

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  erro.textContent = "";
  resultado.replaceChildren();
  botao.disabled = true;

  try {
    const dados = await shortenUrl(input.value.trim(), API_BASE_URL);

    const link = document.createElement("a");
    link.href = dados.short_url;
    link.textContent = dados.short_url;
    link.target = "_blank";
    link.rel = "noopener noreferrer";

    const info = document.createElement("p");
    info.className = "info";
    info.textContent = formatExpiry(dados.expires_at);

    resultado.append(link, info, createCopyButton(dados.short_url));
  } catch (error) {
    erro.textContent =
      error instanceof ApiError ? error.message : "Não foi possível conectar ao servidor.";
  } finally {
    botao.disabled = false;
  }
});
