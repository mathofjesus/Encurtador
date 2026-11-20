import js from "@eslint/js";
import globals from "globals";
import tseslint from "typescript-eslint";

export default tseslint.config(
  {
    ignores: ["**/node_modules/**", "**/dist/**", "**/coverage/**", "**/*.d.ts"],
  },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  {
    files: ["**/*.ts", "**/*.mts"],
    languageOptions: { globals: { ...globals.node } },
    rules: {
      "no-undef": "off",
      "@typescript-eslint/no-unused-vars": ["error", { argsIgnorePattern: "^_" }],
    },
  },
  {
    files: ["web/**/*.ts"],
    languageOptions: { globals: { ...globals.browser } },
  },
  {
    files: ["**/*.mjs", "**/*.config.ts", "**/*.config.mts"],
    languageOptions: { globals: { ...globals.node } },
  },
);
