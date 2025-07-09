import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  server: {
    headers: {
      "Cross-Origin-Opener-Policy": "same-origin",
      "Cross-Origin-Embedder-Policy": "require-corp",
    },
    watch: {
      ignored: ["**/node_modules/**", "**/dist/**"],
    },
  },
  optimizeDeps: {
    exclude: [
      /* '@sqlite.org/sqlite-wasm',  */
      "@sqliteai/sqlite-sync-wasm",
    ],
  },
});
