// 暫用風格預覽；正常 dev/build 不讀取此設定。
import { fileURLToPath } from 'node:url';
import { defineConfig, mergeConfig } from 'vite';
import base from './vite.config';

export default defineConfig(({ command, mode }) => {
  if (command !== 'serve' || mode !== 'prototype') {
    throw new Error('風格原型僅支援 npm run prototype；正式建置請使用 npm run build。');
  }
  const mock = fileURLToPath(new URL('./src/prototype/tauri.ts', import.meta.url));
  return mergeConfig(base, {
    cacheDir: 'node_modules/.vite-prototype',
    server: { host: '127.0.0.1', port: 5188, strictPort: true },
    resolve: { alias: [
      ...['api/core', 'api/event', 'api/window', 'plugin-dialog', 'plugin-autostart']
        .map((name) => ({ find: `@tauri-apps/${name}`, replacement: mock })),
    ] },
    plugins: [{
      name: 'throwaway-style-prototype',
      apply: 'serve',
      transformIndexHtml: {
        order: 'pre',
        handler(html: string) {
          return html.replace('src="/src/main.ts"', 'src="/src/prototype/main.ts"');
        },
      },
    }],
  });
});
