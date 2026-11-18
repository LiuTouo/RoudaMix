import { svelte } from '@sveltejs/vite-plugin-svelte';
import { defineConfig } from 'vite';
import { fileURLToPath } from 'node:url';

export default defineConfig(({ command, mode }) => {
  // Throwaway console study: all native APIs are replaced only in this DEV mode.
  const consolePreview = command === 'serve' && mode === 'console-prototype' && !process.env.VITEST;
  const mock = fileURLToPath(new URL('./src/lib/consolePrototypeTauri.ts', import.meta.url));
  return {
    plugins: [svelte()],
    define: { 'import.meta.env.VITE_CONSOLE_PREVIEW': JSON.stringify(consolePreview) },
    cacheDir: consolePreview ? 'node_modules/.vite-console-prototype' : undefined,
    // design/ 內是靜態稿，不是入口；掃進去會把 mock 的缺漏 export 當成錯誤
    optimizeDeps: { entries: ['index.html'] },
    resolve: {
      ...(process.env.VITEST ? { conditions: ['browser'] } : {}),
      alias: consolePreview ? ['api/core', 'api/event', 'api/window', 'api/webview', 'api/app', 'plugin-dialog', 'plugin-autostart', 'plugin-global-shortcut', 'plugin-updater']
        .map(name => ({ find: `@tauri-apps/${name}`, replacement: mock })) : [],
    },
    clearScreen: false,
    server: { port: 5173, strictPort: true },
  };
});
