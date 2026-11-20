import { defineConfig } from "vitest/config";

export default defineConfig({
  server: {
    port: 5173,
    proxy: {
      "/shorten": "http://localhost:5000",
      "/health": "http://localhost:5000",
    },
  },
  test: {
    environment: "node",
    include: ["test/**/*.test.ts"],
    coverage: {
      provider: "v8",
      reporter: ["text", "lcov"],
      include: ["src/**/*.ts"],
      thresholds: {
        lines: 85,
        functions: 85,
        statements: 85,
        branches: 75,
      },
    },
  },
});
