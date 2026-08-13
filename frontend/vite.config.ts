import { defineConfig } from 'vite';

export default defineConfig({
  build: {
    target: 'es2015',
    minify: 'terser',
    cssMinify: true,
    outDir: 'dist',
    assetsDir: 'assets',
    rollupOptions: {
      output: {
        manualChunks: undefined,
      },
    },
  },
});
