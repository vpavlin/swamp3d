// fp.mjs - Swamp shape fingerprint, version 1 (research PoC).
//
// Two descriptors (docs/adr/0003):
//   F1  global shape histograms: D2 (distances between random surface points, normalised by
//       their mean) and A3 (angle at the middle of random point triples). Rotation-, translation-,
//       mirror- and scale-invariant. Answers "do these look alike overall?".
//   F2  a set of scale-free local tokens from random point triples (the triangle's angles plus
//       how each point's surface normal sits against the triangle's plane), summarised as a MinHash
//       signature plus the set size. Adding a base or cutting a part keeps most of the original's
//       tokens, so containment survives the edits thieves make. Answers "is one inside the other?".
//
// Determinism: triangles are put in a canonical, rotation-invariant order (sorted by rounded
// relative edge lengths) before seeded sampling, so a re-exported file (shuffled faces, rotated
// vertex order, ASCII vs binary) gives the SAME fingerprint. All randomness is a 32-bit integer
// PRNG with a fixed seed, portable to C++.

export const FP_VERSION = 1;
export const PARAMS = {
  samples: 4096,     // surface points
  d2Pairs: 20000, d2Bins: 64, d2Max: 3.0,
  a3Triples: 20000, a3Bins: 32,
  tokTriples: 30000, angBins: 12, nrmBins: 4,
  minhashK: 128, bands: 32, rows: 4,
};

// ── tiny deterministic PRNG (mulberry32): uint32 state, uint32 output ──────────
export function rng(seed) {
  let a = seed >>> 0;
  return {
    u32() {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return (t ^ (t >>> 14)) >>> 0;
    },
    f64() { return this.u32() / 4294967296; },
  };
}

// ── STL parsing (binary + ASCII) into a Float64Array of 9 coords per triangle ───────────
export function parseStl(buf) {
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  if (u8.length >= 84) {
    const n = dv.getUint32(80, true);
    if (84 + n * 50 === u8.length) {
      const out = new Float64Array(n * 9);
      for (let i = 0; i < n; i++) {
        const o = 84 + i * 50 + 12;
        for (let k = 0; k < 9; k++) out[i * 9 + k] = dv.getFloat32(o + k * 4, true);
      }
      return out;
    }
  }
  const text = new TextDecoder().decode(u8);
  const nums = [];
  const re = /vertex\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)/g;
  let m;
  while ((m = re.exec(text))) nums.push(+m[1], +m[2], +m[3]);
  return Float64Array.from(nums.slice(0, nums.length - (nums.length % 9)));
}

// ── canonical triangle order ────────────────────────────────────────────────────
function prepare(tris) {
  const n = tris.length / 9;
  const area = new Float64Array(n);
  let total = 0;
  for (let i = 0; i < n; i++) {
    const o = i * 9;
    const ux = tris[o + 3] - tris[o], uy = tris[o + 4] - tris[o + 1], uz = tris[o + 5] - tris[o + 2];
    const vx = tris[o + 6] - tris[o], vy = tris[o + 7] - tris[o + 1], vz = tris[o + 8] - tris[o + 2];
    const cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    area[i] = 0.5 * Math.sqrt(cx * cx + cy * cy + cz * cz);
    total += area[i];
  }
  if (!(total > 0)) throw new Error("mesh has no area");
  const s = Math.sqrt(total);
  // Rotation-invariant per-triangle key: its three edge lengths relative to sqrt(total area),
  // sorted, rounded. Within a triangle, vertices are reordered so the canonical point placement
  // doesn't depend on the file's vertex order.
  const keys = new Array(n);
  const canon = new Float64Array(n * 9);
  for (let i = 0; i < n; i++) {
    const o = i * 9;
    const P = [[tris[o], tris[o + 1], tris[o + 2]], [tris[o + 3], tris[o + 4], tris[o + 5]], [tris[o + 6], tris[o + 7], tris[o + 8]]];
    const L = [dist(P[1], P[2]), dist(P[0], P[2]), dist(P[0], P[1])]; // edge opposite each vertex
    const idx = [0, 1, 2].sort((a, b) => L[a] - L[b] || a - b);
    for (let k = 0; k < 3; k++) for (let c = 0; c < 3; c++) canon[o + k * 3 + c] = P[idx[k]][c];
    const r = idx.map((j) => Math.round((L[j] / s) * 1e6));
    keys[i] = [r[0], r[1], r[2], i];
  }
  keys.sort((a, b) => a[0] - b[0] || a[1] - b[1] || a[2] - b[2] || 0);
  const order = keys.map((k) => k[3]);
  return { canon, area, total, order };
}

function dist(a, b) { const x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return Math.sqrt(x * x + y * y + z * z); }

// ── area-weighted surface sampling ───────────────────────────────────────────────
export function samplePoints(tris, count = PARAMS.samples, seed = 0x5a3b) {
  const { canon, area, total, order } = prepare(tris);
  const cum = new Float64Array(order.length);
  let acc = 0;
  for (let j = 0; j < order.length; j++) { acc += area[order[j]]; cum[j] = acc; }
  const R = rng(seed);
  const pts = new Float64Array(count * 3), nrm = new Float64Array(count * 3);
  for (let s = 0; s < count; s++) {
    const t = R.f64() * total;
    let lo = 0, hi = cum.length - 1;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (cum[mid] < t) lo = mid + 1; else hi = mid; }
    const o = order[lo] * 9;
    let r1 = R.f64(), r2 = R.f64();
    const sq = Math.sqrt(r1);
    const a = 1 - sq, b = sq * (1 - r2), c = sq * r2;
    for (let k = 0; k < 3; k++) pts[s * 3 + k] = a * canon[o + k] + b * canon[o + 3 + k] + c * canon[o + 6 + k];
    const ux = canon[o + 3] - canon[o], uy = canon[o + 4] - canon[o + 1], uz = canon[o + 5] - canon[o + 2];
    const vx = canon[o + 6] - canon[o], vy = canon[o + 7] - canon[o + 1], vz = canon[o + 8] - canon[o + 2];
    let nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const nl = Math.sqrt(nx * nx + ny * ny + nz * nz) || 1;
    nrm[s * 3] = nx / nl; nrm[s * 3 + 1] = ny / nl; nrm[s * 3 + 2] = nz / nl;
  }
  return { pts, nrm, count, area: total };
}

// ── F1: D2 + A3 histograms ───────────────────────────────────────────────────────
function f1(S) {
  const R = rng(0xd2d2);
  const P = PARAMS, n = S.count, p = S.pts;
  const d = new Float64Array(P.d2Pairs);
  let mean = 0;
  for (let i = 0; i < P.d2Pairs; i++) {
    const a = R.u32() % n, b = R.u32() % n;
    d[i] = Math.hypot(p[a * 3] - p[b * 3], p[a * 3 + 1] - p[b * 3 + 1], p[a * 3 + 2] - p[b * 3 + 2]);
    mean += d[i];
  }
  mean /= P.d2Pairs || 1;
  const d2 = new Float64Array(P.d2Bins);
  for (let i = 0; i < P.d2Pairs; i++) {
    const bin = clampBin(Math.floor((d[i] / (mean || 1) / P.d2Max) * P.d2Bins), P.d2Bins);
    if (bin >= 0) d2[bin] += 1 / P.d2Pairs;
  }
  const a3 = new Float64Array(P.a3Bins);
  for (let i = 0; i < P.a3Triples; i++) {
    const a = R.u32() % n, b = R.u32() % n, c = R.u32() % n;
    const ang = angleAt(p, b, a, c);
    const bin = clampBin(Math.floor((ang / Math.PI) * P.a3Bins), P.a3Bins);
    if (bin >= 0) a3[bin] += 1 / P.a3Triples;
  }
  return { d2: Array.from(d2), a3: Array.from(a3) };
}

function angleAt(p, m, a, c) {
  const ux = p[a * 3] - p[m * 3], uy = p[a * 3 + 1] - p[m * 3 + 1], uz = p[a * 3 + 2] - p[m * 3 + 2];
  const vx = p[c * 3] - p[m * 3], vy = p[c * 3 + 1] - p[m * 3 + 1], vz = p[c * 3 + 2] - p[m * 3 + 2];
  const lu = Math.hypot(ux, uy, uz), lv = Math.hypot(vx, vy, vz);
  if (lu === 0 || lv === 0) return 0;
  return Math.acos(Math.max(-1, Math.min(1, (ux * vx + uy * vy + uz * vz) / (lu * lv))));
}

// ── F2: scale-free triple tokens → MinHash ───────────────────────────────────────
function tokens(S) {
  const R = rng(0x70c5);
  const P = PARAMS, n = S.count, p = S.pts, q = S.nrm;
  const set = new Set();
  for (let i = 0; i < P.tokTriples; i++) {
    const ids = [R.u32() % n, R.u32() % n, R.u32() % n];
    if (ids[0] === ids[1] || ids[1] === ids[2] || ids[0] === ids[2]) continue;
    // angles at each vertex; order vertices by angle so the token doesn't depend on draw order
    const ang = [angleAt(p, ids[0], ids[1], ids[2]), angleAt(p, ids[1], ids[0], ids[2]), angleAt(p, ids[2], ids[0], ids[1])];
    const ord = [0, 1, 2].sort((x, y) => ang[x] - ang[y]);
    // plane normal of the triple
    const A = ids[0] * 3, B = ids[1] * 3, C = ids[2] * 3;
    const ux = p[B] - p[A], uy = p[B + 1] - p[A + 1], uz = p[B + 2] - p[A + 2];
    const vx = p[C] - p[A], vy = p[C + 1] - p[A + 1], vz = p[C + 2] - p[A + 2];
    let nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const nl = Math.hypot(nx, ny, nz);
    if (nl === 0) continue;
    nx /= nl; ny /= nl; nz /= nl;
    let tok = 0;
    tok = tok * P.angBins + Math.min(P.angBins - 1, Math.floor((ang[ord[0]] / (Math.PI / 3)) * P.angBins)); // smallest angle <= 60 deg
    tok = tok * P.angBins + Math.min(P.angBins - 1, Math.floor((ang[ord[1]] / (Math.PI / 2)) * P.angBins)); // middle angle <= 90 deg
    for (const k of ord) {
      const j = ids[k] * 3;
      const c = Math.abs(q[j] * nx + q[j + 1] * ny + q[j + 2] * nz); // unsigned: mirror- and winding-proof
      tok = tok * P.nrmBins + Math.min(P.nrmBins - 1, Math.floor(c * P.nrmBins));
    }
    set.add(tok);
  }
  return set;
}

function mix32(x) {
  x = Math.imul(x ^ (x >>> 16), 0x85ebca6b);
  x = Math.imul(x ^ (x >>> 13), 0xc2b2ae35);
  return (x ^ (x >>> 16)) >>> 0;
}

function minhash(set) {
  const K = PARAMS.minhashK;
  const R = rng(0x3141);
  const salts = Array.from({ length: K }, () => R.u32());
  const sig = new Array(K).fill(0xffffffff);
  for (const t of set) {
    for (let k = 0; k < K; k++) {
      const h = mix32((t ^ salts[k]) >>> 0);
      if (h < sig[k]) sig[k] = h;
    }
  }
  return sig;
}

function lshKeys(sig) {
  const { bands, rows } = PARAMS;
  const keys = [];
  for (let b = 0; b < bands; b++) {
    let h = 0x811c9dc5 ^ b;
    for (let r = 0; r < rows; r++) h = mix32((h ^ sig[b * rows + r]) >>> 0);
    keys.push(h.toString(16).padStart(8, "0"));
  }
  return keys;
}

// ── F3: local, density-normalised point-pair features at several neighbourhood ranks ──
// For each sample point i and its k-th nearest sample neighbour j (k in RANKS), a token from:
//   log2(d_ij / r_i)   r_i = distance to i's 4th neighbour (cancels sampling density and scale)
//   |n_i . d^|, |n_j . d^|, |n_i . n_j|   (unsigned: mirror- and winding-proof)
// Only the surface near i matters, so geometry added elsewhere (a base) leaves these tokens
// unchanged; the histogram of tokens is compared by intersection and by containment.
export const RANKS = [2, 6, 16, 40];
export const F3 = { ratioBins: 4, angBins: 6 };
export const F3_SIZE = RANKS.length * F3.ratioBins * F3.angBins ** 3;

function knnRanks(S, maxRank) {
  const n = S.count, p = S.pts;
  const out = new Int32Array(n * maxRank), dout = new Float64Array(n * maxRank);
  const d = new Float64Array(n), idx = new Int32Array(n);
  for (let i = 0; i < n; i++) {
    const x = p[i * 3], y = p[i * 3 + 1], z = p[i * 3 + 2];
    for (let j = 0; j < n; j++) { const a = p[j * 3] - x, b = p[j * 3 + 1] - y, c = p[j * 3 + 2] - z; d[j] = a * a + b * b + c * c; idx[j] = j; }
    d[i] = Infinity;
    // partial selection of the maxRank smallest (stable: ties by index)
    const best = [];
    for (let j = 0; j < n; j++) {
      if (best.length < maxRank) { best.push(j); if (best.length === maxRank) best.sort((u, v) => d[u] - d[v] || u - v); continue; }
      const last = best[maxRank - 1];
      if (d[j] < d[last] || (d[j] === d[last] && j < last)) {
        let k = maxRank - 1;
        while (k > 0 && (d[best[k - 1]] > d[j] || (d[best[k - 1]] === d[j] && best[k - 1] > j))) { best[k] = best[k - 1]; k--; }
        best[k] = j;
      }
    }
    if (best.length < maxRank) best.sort((u, v) => d[u] - d[v] || u - v);
    for (let k = 0; k < maxRank; k++) { out[i * maxRank + k] = best[k] ?? i; dout[i * maxRank + k] = Math.sqrt(d[best[k]] ?? 0); }
  }
  return { nb: out, nd: dout, maxRank };
}

function f3(S) {
  const maxRank = Math.max(...RANKS);
  const { nb, nd } = knnRanks(S, maxRank);
  const p = S.pts, q = S.nrm, n = S.count;
  const hist = new Float64Array(F3_SIZE);
  const A = F3.angBins, Rb = F3.ratioBins;
  for (let i = 0; i < n; i++) {
    const r = nd[i * maxRank + 3] || 1e-12;
    for (let ri = 0; ri < RANKS.length; ri++) {
      const k = RANKS[ri] - 1;
      const j = nb[i * maxRank + k], dij = nd[i * maxRank + k];
      if (j === i || dij === 0) continue;
      const dx = (p[j * 3] - p[i * 3]) / dij, dy = (p[j * 3 + 1] - p[i * 3 + 1]) / dij, dz = (p[j * 3 + 2] - p[i * 3 + 2]) / dij;
      const a1 = Math.abs(q[i * 3] * dx + q[i * 3 + 1] * dy + q[i * 3 + 2] * dz);
      const a2 = Math.abs(q[j * 3] * dx + q[j * 3 + 1] * dy + q[j * 3 + 2] * dz);
      const a3 = Math.abs(q[i * 3] * q[j * 3] + q[i * 3 + 1] * q[j * 3 + 1] + q[i * 3 + 2] * q[j * 3 + 2]);
      // ratio relative to what that rank "usually" is on a flat uniform surface (~sqrt(k/4))
      const lr = Math.log2(dij / r / Math.sqrt(RANKS[ri] / 4));
      const rb = clampBin(Math.floor((lr + 1) * Rb / 2), Rb);
      const qa = (x) => clampBin(Math.floor(x * A), A);
      if (rb < 0 || qa(a1) < 0 || qa(a2) < 0 || qa(a3) < 0) continue;
      const tok = (((ri * Rb + rb) * A + qa(a1)) * A + qa(a2)) * A + qa(a3);
      hist[tok] += 1;
    }
  }
  let tot = 0;
  for (let t = 0; t < hist.length; t++) tot += hist[t];
  for (let t = 0; t < hist.length; t++) hist[t] /= tot || 1;
  return Array.from(hist, (v) => Math.round(v * 65535));   // uint16 per bin
}

/** Fingerprint a mesh given as triangle soup (Float64Array, 9 per triangle). */
// Drop triangles with a non-finite or absurd (|x| > 1e7) coordinate, so a hostile STL can't
// poison the sampling or the binning. Same rule as swamp_core/src/swamp_fp.hpp sanitize().
export function sanitize(tris) {
  const out = [];
  for (let i = 0; i + 9 <= tris.length; i += 9) {
    let ok = true;
    for (let k = 0; k < 9; k++) { const x = tris[i + k]; if (!Number.isFinite(x) || Math.abs(x) > 1e7) { ok = false; break; } }
    if (ok) for (let k = 0; k < 9; k++) out.push(tris[i + k]);
  }
  return Float64Array.from(out);
}
// Histogram bin, clamped; -1 (skip) for NaN/Infinity. Same as clampBin() in the C++ port.
function clampBin(x, bins) {
  if (!Number.isFinite(x)) return -1;
  if (x < 0) return 0;
  return x >= bins ? bins - 1 : Math.floor(x);
}

export function fingerprint(tris) {
  const S = samplePoints(sanitize(tris));
  const { d2, a3 } = f1(S);
  const set = tokens(S);
  const sig = minhash(set);
  return { v: FP_VERSION, d2, a3, tokens: set.size, minhash: sig, lsh: lshKeys(sig), f3: f3(S) };
}

/** Histogram intersection of the local-feature histograms (1 = identical distributions). */
export function f3Intersection(a, b) {
  let s = 0;
  for (let t = 0; t < a.f3.length; t++) s += Math.min(a.f3[t], b.f3[t]);
  return s / 65535;
}

/** How much of A's local-feature distribution appears in B, if A makes up a fraction p of B. */
export function f3Coverage(a, b, p) {
  let s = 0;
  for (let t = 0; t < a.f3.length; t++) s += Math.min(a.f3[t], b.f3[t] / p);
  return s / 65535;
}

// ── comparison ───────────────────────────────────────────────────────────────────
export function f1Distance(a, b) {
  let s = 0;
  for (let i = 0; i < a.d2.length; i++) s += Math.abs(a.d2[i] - b.d2[i]);
  for (let i = 0; i < a.a3.length; i++) s += Math.abs(a.a3[i] - b.a3[i]);
  return s / 4; // both histograms sum to 1 → max L1 is 2 each → scaled to [0,1]
}

export function jaccard(a, b) {
  let eq = 0;
  for (let k = 0; k < a.minhash.length; k++) if (a.minhash[k] === b.minhash[k]) eq++;
  return eq / a.minhash.length;
}

/** Estimated fraction of A's token set found in B, from Jaccard and the two set sizes. */
export function containment(a, b) {
  const J = jaccard(a, b);
  const inter = (J * (a.tokens + b.tokens)) / (1 + J);
  return Math.min(1, inter / a.tokens);
}

export function compare(a, b) {
  const out = { f1: f1Distance(a, b), jaccard: jaccard(a, b), containAinB: containment(a, b), containBinA: containment(b, a), lshHits: a.lsh.filter((k, i) => k === b.lsh[i]).length };
  if (a.f3 && b.f3) {
    out.f3 = f3Intersection(a, b);
    // containment either way, assuming the shared part is at least 70% of the larger model
    out.f3cov = Math.max(f3Coverage(a, b, 0.7), f3Coverage(b, a, 0.7));
  }
  return out;
}
