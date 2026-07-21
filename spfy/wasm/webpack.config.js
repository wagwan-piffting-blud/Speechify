// Webpack bundling for the spfy WASM demo.
//
// The emscripten build (build.sh) emits into dist/:
//   spfy_wasm.js       JS factory (ES module).
//   spfy_wasm.wasm     WebAssembly bytecode (code + small SWIttsFe DLLs).
//   voices/            Lazy voice assets + manifest.json.
//
// Webpack here just bundles the page glue (web/index.js + index.html)
// and copies the emscripten artifacts + the voices/ tree + styles.css
// straight through. We deliberately do NOT let webpack parse
// spfy_wasm.js (it's already self-contained); we just load it via a
// <script> tag and let the browser fetch the wasm + voices at runtime.

import path        from "node:path";
import { fileURLToPath } from "node:url";
import HtmlPlugin from "html-webpack-plugin";
import CopyPlugin from "copy-webpack-plugin";
import compression from "compression";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

export default (env, argv) => {
  const isProd = argv.mode === "production";
  return {
    mode: isProd ? "production" : "development",
    entry: "./web/index.js",
    output: {
      filename:    isProd ? "[name].[contenthash:8].js" : "[name].js",
      path:        path.resolve(__dirname, "build/web"),
      clean:       true,
      publicPath:  "",
    },
    devtool: isProd ? "source-map" : "eval-cheap-module-source-map",
    resolve: { extensions: [".js"] },
    // The emscripten output (dist/spfy_wasm.js) is loaded at runtime via
    // a <script> tag in web/index.js (NOT imported through webpack), so
    // we don't need any rules to suppress webpack from parsing it.
    plugins: [
      new HtmlPlugin({
        template: "./web/index.html",
        inject:   "body",
        minify:   isProd,
      }),
      new CopyPlugin({
        patterns: [
          { from: "web/styles.css", to: "styles.css" },
          // Emscripten outputs. `noErrorOnMissing` lets `npm run dev`
          // succeed before `./build.sh` has produced the artifacts —
          // the dev server starts; the page surfaces a clear error when
          // it can't fetch spfy_wasm.js, prompting the user to build.
          // Production builds will fail loudly here (as they should).
          // NB there is no .data sidecar anymore: voices load lazily.
          { from: "dist/spfy_wasm.wasm", to: "spfy_wasm.wasm",
            noErrorOnMissing: !isProd },
          { from: "dist/spfy_wasm.js",   to: "spfy_wasm.js",
            noErrorOnMissing: !isProd },
          // Lazy voice assets + manifest (dist/voices/, produced by
          // stage_voices.py). Each file is <100 MB so it deploys cleanly
          // to GitHub Pages; the browser fetches only the chosen voice.
          { from: "dist/voices", to: "voices",
            noErrorOnMissing: !isProd },
        ],
      }),
    ],
    devServer: {
      static: [
        { directory: path.resolve(__dirname, "build/web") },
        { directory: path.resolve(__dirname, "dist"), publicPath: "/" },
      ],
      // Force compression for the WASM artifacts. webpack-dev-server's
      // default `compress: true` uses `compression`'s filter which skips
      // application/octet-stream (our .data, .wasm) — explicit
      // setupMiddlewares ensures every byte that crosses the wire gets
      // gzipped before transfer.
      compress: false,
      setupMiddlewares: (middlewares, devServer) => {
        // Force compression on every response — bypasses the default
        // mime-type filter that skips application/octet-stream.
        devServer.app.use(compression({ filter: () => true, threshold: 0 }));
        return middlewares;
      },
      port:     6660,
      hot:      false,         // wasm reload is cheap on full refresh
      headers: {
        // SharedArrayBuffer + COOP/COEP enable threading should we ever
        // emit -pthread builds; harmless for the current single-thread
        // build.
        "Cross-Origin-Opener-Policy":   "same-origin",
        "Cross-Origin-Embedder-Policy": "require-corp",
      },
      client: {
        logging:        "info",
        overlay:        { errors: true, warnings: false },
        progress:       true,
      },
    },
    experiments: {
      asyncWebAssembly:  true,
      topLevelAwait:     true,
    },
    performance: {
      // The .data sidecar is huge by design; suppress the size warning.
      maxAssetSize:      200 * 1024 * 1024,
      maxEntrypointSize: 200 * 1024 * 1024,
    },
  };
};
