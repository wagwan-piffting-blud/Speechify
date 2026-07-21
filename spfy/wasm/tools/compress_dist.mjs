// Pre-compress the build artifacts in dist/ so deploy targets can serve
// them with Content-Encoding: gzip or br. The browser picks the best
// variant via Accept-Encoding; static hosts (Netlify, Vercel, S3 +
// CloudFront, NGINX with gzip_static / brotli_static, etc.) auto-route
// to .gz / .br when present alongside the original.
//
// Writes:
//   dist/spfy_wasm.wasm.gz   dist/spfy_wasm.wasm.br
//   dist/spfy_wasm.js.gz     dist/spfy_wasm.js.br
//
// Voice assets under dist/voices/ are NOT compressed here: the VDBs are
// already-compact μ-law-ish PCM that barely shrinks, and they are fetched
// on demand rather than up front. Original files are kept (some hosts
// don't serve pre-compressed; they gzip on the fly).

import { promises as fs } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
import zlib from "node:zlib";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const distDir   = path.resolve(__dirname, "..", "dist");

const files = ["spfy_wasm.wasm", "spfy_wasm.js"];

// Gzip @ level 9 (max) — slow at build time, smaller payload at runtime.
// Brotli quality is tunable via $BROTLI_QUALITY (default 6). Quality 11
// is the absolute max but single-threaded brotli at q=11 on ~90 MB of
// binary takes 5–15 minutes for ~5% extra savings vs q=6. For audio /
// already-compressed payloads the curve flattens hard past q=5; q=6 is
// the practical sweet spot. Bump to 11 for one-shot production builds
// going to a long-lived CDN.
const GZIP_OPTS   = { level: zlib.constants.Z_BEST_COMPRESSION };
const BROTLI_Q    = Number.parseInt(process.env.BROTLI_QUALITY ?? "6", 10);
const BROTLI_OPTS = {
  params: {
    [zlib.constants.BROTLI_PARAM_QUALITY]: BROTLI_Q,
    [zlib.constants.BROTLI_PARAM_MODE]:    zlib.constants.BROTLI_MODE_GENERIC,
  },
};

function fmtBytes(n) {
  if (n >= 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + " MiB";
  if (n >= 1024)        return (n / 1024).toFixed(1)        + " KiB";
  return n + " B";
}

async function compress(src, ext, encoder, opts) {
  const dst = src + ext;
  const t0  = process.hrtime.bigint();
  const buf = await fs.readFile(src);
  const out = await new Promise((resolve, reject) =>
    encoder(buf, opts, (err, res) => err ? reject(err) : resolve(res))
  );
  await fs.writeFile(dst, out);
  const dt    = Number(process.hrtime.bigint() - t0) / 1e6;
  const ratio = (out.length / buf.length * 100).toFixed(1);
  return { src, dst, srcLen: buf.length, dstLen: out.length, ratio, ms: dt };
}

async function main() {
  console.log(`compressing artifacts in ${distDir}`);
  for (const name of files) {
    const src = path.join(distDir, name);
    try { await fs.access(src); }
    catch { console.warn(`  skip: ${name} not found (run ./build.bat first?)`); continue; }

    const gz = await compress(src, ".gz", zlib.gzip,    GZIP_OPTS);
    const br = await compress(src, ".br", zlib.brotliCompress, BROTLI_OPTS);
    console.log(
      `  ${name.padEnd(18)}` +
      ` raw=${fmtBytes(gz.srcLen).padStart(9)}` +
      ` gz=${fmtBytes(gz.dstLen).padStart(9)} (${gz.ratio}%, ${gz.ms.toFixed(0)}ms)` +
      ` br=${fmtBytes(br.dstLen).padStart(9)} (${br.ratio}%, ${br.ms.toFixed(0)}ms)`
    );
  }
}

main().catch(err => { console.error(err); process.exit(1); });
