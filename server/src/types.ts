export interface UrlRecord {
  id: number;
  code: string;
  originalUrl: string;
  expiresAt: Date;
}

export interface NewUrl {
  code: string;
  originalUrl: string;
  expiresAt: Date;
}

export interface UrlRepository {
  /** Insere uma URL; deve lançar erro com `code === "23505"` em colisão de código. */
  insert(value: NewUrl): Promise<void>;
  findByCode(code: string): Promise<UrlRecord | null>;
}
