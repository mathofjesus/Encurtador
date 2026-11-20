import type { ErrorRequestHandler } from "express";

export const errorHandler: ErrorRequestHandler = (error, _req, res, _next) => {
  // Corpo JSON malformado é rejeitado pelo express.json() com SyntaxError.
  if (error instanceof SyntaxError && "body" in error) {
    res.status(400).json({ error: "JSON inválido." });
    return;
  }

  console.error(error);
  res.status(500).json({ error: "Erro interno do servidor." });
};
