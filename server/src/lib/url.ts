/** Mesma regra do backend antigo: exige explicitamente http:// ou https://. */
export function isValidHttpUrl(value: string): boolean {
  return value.startsWith("http://") || value.startsWith("https://");
}
