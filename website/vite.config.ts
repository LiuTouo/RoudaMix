import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { resolve } from 'node:path';

export default defineConfig({
  base: '/RoudaMix/',
  plugins: [svelte()],
  build: { rollupOptions: { input: { zh: resolve('index.html'), en: resolve('en/index.html') } } },
});
