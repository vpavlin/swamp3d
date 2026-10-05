// verify.mjs - geometric verification of a suspected copy (stage 2, run only on shortlisted pairs).
//
// Question: "how much of model A's surface lies on model B, after the best similarity transform
// (rotation, uniform scale, translation, mirror allowed)?" A copy with an added base or text
// covers ~100% of the original; an unrelated model covers little. Works from the files, not from
// the fingerprint, so a client can always re-check it.
//
//  1. sample both surfaces (same canonical sampling as fp.mjs);
//  2. per-point local descriptors (scale-free point-pair features at several neighbour ranks);
//  3. match A points to B points by descriptor;
//  4. RANSAC over triples of matches -> similarity transform (Umeyama, reflection allowed);
//  5. coverage = fraction of A points within eps of B after the transform, refined on inliers.
import { samplePoints, rng } from "./fp.mjs";

const RANKS = [2, 6, 16, 40];

function knn(pts, n, K) {
  const nb = new Int32Array(n * K), nd = new Float64Array(n * K);
  const d = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) {
      const a = pts[j * 3] - pts[i * 3], b = pts[j * 3 + 1] - pts[i * 3 + 1], c = pts[j * 3 + 2] - pts[i * 3 + 2];
      d[j] = a * a + b * b + c * c;
    }
    d[i] = Infinity;
    const best = [];
    for (let j = 0; j < n; j++) {
      if (best.length < K) { best.push(j); if (best.length === K) best.sort((u, v) => d[u] - d[v] || u - v); continue; }
      const last = best[K - 1];
      if (d[j] < d[last]) { let k = K - 1; while (k > 0 && d[best[k - 1]] > d[j]) { best[k] = best[k - 1]; k--; } best[k] = j; }
    }
    if (best.length < K) best.sort((u, v) => d[u] - d[v] || u - v);
    for (let k = 0; k < K; k++) { nb[i * K + k] = best[k] ?? i; nd[i * K + k] = Math.sqrt(d[best[k]] ?? 0); }
  }
  return { nb, nd, K };
}

function descriptors(S) {
  const K = Math.max(...RANKS), n = S.count, p = S.pts, q = S.nrm;
  const { nb, nd } = knn(p, n, K);
  const D = new Float64Array(n * RANKS.length * 4);
  const spacing = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const r = nd[i * K + 3] || 1e-12;
    spacing[i] = r;
    RANKS.forEach((rank, ri) => {
      const j = nb[i * K + rank - 1], dij = nd[i * K + rank - 1] || 1e-12;
      const dx = (p[j * 3] - p[i * 3]) / dij, dy = (p[j * 3 + 1] - p[i * 3 + 1]) / dij, dz = (p[j * 3 + 2] - p[i * 3 + 2]) / dij;
      const o = (i * RANKS.length + ri) * 4;
      D[o] = Math.log2(dij / r) * 0.5;
      D[o + 1] = Math.abs(q[i * 3] * dx + q[i * 3 + 1] * dy + q[i * 3 + 2] * dz);
      D[o + 2] = Math.abs(q[j * 3] * dx + q[j * 3 + 1] * dy + q[j * 3 + 2] * dz);
      D[o + 3] = Math.abs(q[i * 3] * q[j * 3] + q[i * 3 + 1] * q[j * 3 + 1] + q[i * 3 + 2] * q[j * 3 + 2]);
    });
  }
  const sorted = Array.from(spacing).sort((a, b) => a - b);
  return { D, dim: RANKS.length * 4, spacing: sorted[sorted.length >> 1] };
}

// Umeyama similarity transform A -> B from point lists (reflection allowed).
function similarity(a, b) {
  const n = a.length;
  const ca = [0, 0, 0], cb = [0, 0, 0];
  for (let i = 0; i < n; i++) for (let k = 0; k < 3; k++) { ca[k] += a[i][k] / n; cb[k] += b[i][k] / n; }
  const H = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  let va = 0;
  for (let i = 0; i < n; i++) {
    const x = [a[i][0] - ca[0], a[i][1] - ca[1], a[i][2] - ca[2]], y = [b[i][0] - cb[0], b[i][1] - cb[1], b[i][2] - cb[2]];
    va += x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    for (let r = 0; r < 3; r++) for (let c = 0; c < 3; c++) H[r][c] += y[r] * x[c];
  }
  const { U, S, V } = svd3(H);
  const R = mul(U, transpose(V));          // det may be -1: a mirrored copy is still a copy
  const s = (S[0] + S[1] + S[2]) / (va || 1e-12);
  const t = [0, 1, 2].map((r) => cb[r] - s * (R[r][0] * ca[0] + R[r][1] * ca[1] + R[r][2] * ca[2]));
  return { R, s, t };
}

function apply(T, x) {
  return [0, 1, 2].map((r) => T.s * (T.R[r][0] * x[0] + T.R[r][1] * x[1] + T.R[r][2] * x[2]) + T.t[r]);
}
const transpose = (M) => [[M[0][0], M[1][0], M[2][0]], [M[0][1], M[1][1], M[2][1]], [M[0][2], M[1][2], M[2][2]]];
const mul = (A, B) => A.map((row) => [0, 1, 2].map((c) => row[0] * B[0][c] + row[1] * B[1][c] + row[2] * B[2][c]));

// SVD of a 3x3 matrix via Jacobi eigen-decomposition of H^T H.
function svd3(H) {
  const A = mul(transpose(H), H);
  let V = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  const M = A.map((r) => r.slice());
  for (let sweep = 0; sweep < 30; sweep++) {
    for (const [p, q] of [[0, 1], [0, 2], [1, 2]]) {
      if (Math.abs(M[p][q]) < 1e-18) continue;
      const th = (M[q][q] - M[p][p]) / (2 * M[p][q]);
      const t = Math.sign(th || 1) / (Math.abs(th) + Math.sqrt(th * th + 1));
      const c = 1 / Math.sqrt(t * t + 1), s = t * c;
      for (let k = 0; k < 3; k++) { const mkp = M[k][p], mkq = M[k][q]; M[k][p] = c * mkp - s * mkq; M[k][q] = s * mkp + c * mkq; }
      for (let k = 0; k < 3; k++) { const mpk = M[p][k], mqk = M[q][k]; M[p][k] = c * mpk - s * mqk; M[q][k] = s * mpk + c * mqk; }
      for (let k = 0; k < 3; k++) { const vkp = V[k][p], vkq = V[k][q]; V[k][p] = c * vkp - s * vkq; V[k][q] = s * vkp + c * vkq; }
    }
  }
  const ev = [0, 1, 2].map((i) => Math.max(0, M[i][i]));
  const order = [0, 1, 2].sort((a, b) => ev[b] - ev[a]);
  V = [0, 1, 2].map((r) => order.map((c) => V[r][c]));
  const S = order.map((i) => Math.sqrt(ev[i]));
  const HV = mul(H, V);
  const U = [0, 1, 2].map((r) => [0, 1, 2].map((c) => (S[c] > 1e-12 ? HV[r][c] / S[c] : 0)));
  // complete U for rank-deficient cases
  if (S[2] <= 1e-12) {
    const u0 = [U[0][0], U[1][0], U[2][0]], u1 = [U[0][1], U[1][1], U[2][1]];
    const u2 = [u0[1] * u1[2] - u0[2] * u1[1], u0[2] * u1[0] - u0[0] * u1[2], u0[0] * u1[1] - u0[1] * u1[0]];
    for (let r = 0; r < 3; r++) U[r][2] = u2[r];
  }
  return { U, S, V };
}

function grid(pts, n, cell) {
  const g = new Map();
  for (let i = 0; i < n; i++) {
    const k = `${Math.floor(pts[i * 3] / cell)},${Math.floor(pts[i * 3 + 1] / cell)},${Math.floor(pts[i * 3 + 2] / cell)}`;
    let arr = g.get(k); if (!arr) g.set(k, (arr = [])); arr.push(i);
  }
  return {
    near(x, eps) {
      const cx = Math.floor(x[0] / cell), cy = Math.floor(x[1] / cell), cz = Math.floor(x[2] / cell);
      const e2 = eps * eps;
      for (let a = -1; a <= 1; a++) for (let b = -1; b <= 1; b++) for (let c = -1; c <= 1; c++) {
        const arr = g.get(`${cx + a},${cy + b},${cz + c}`);
        if (!arr) continue;
        for (const j of arr) { const dx = pts[j * 3] - x[0], dy = pts[j * 3 + 1] - x[1], dz = pts[j * 3 + 2] - x[2]; if (dx * dx + dy * dy + dz * dz <= e2) return true; }
      }
      return false;
    },
  };
}

/**
 * Coverage of A by B: fraction of A's surface samples that land on B under the best similarity
 * transform. Returns { coverage, scale, mirrored, inliers }.
 */
export function coverage(trisA, trisB, { samples = 1536, iters = 1500, seed = 0x7e57 } = {}) {
  const A = samplePoints(trisA, samples, 0x5a3b), B = samplePoints(trisB, samples * 2, 0x5a3c);
  const da = descriptors(A), db = descriptors(B);
  const dim = da.dim;
  // A -> best B match by descriptor (keep the 40% most confident)
  const matches = [];
  for (let i = 0; i < A.count; i++) {
    let best = -1, bd = Infinity;
    for (let j = 0; j < B.count; j++) {
      let s = 0;
      for (let k = 0; k < dim; k++) { const x = da.D[i * dim + k] - db.D[j * dim + k]; s += x * x; if (s >= bd) break; }
      if (s < bd) { bd = s; best = j; }
    }
    matches.push([i, best, bd]);
  }
  matches.sort((x, y) => x[2] - y[2]);
  const M = matches.slice(0, Math.max(30, Math.floor(matches.length * 0.4)));
  const P = (S, i) => [S.pts[i * 3], S.pts[i * 3 + 1], S.pts[i * 3 + 2]];
  const eps = 2.5 * db.spacing;
  const g = grid(B.pts, B.count, eps);
  const R = rng(seed);
  const countIn = (T, step = 1) => {
    let c = 0, tot = 0;
    for (let i = 0; i < A.count; i += step) { tot++; if (g.near(apply(T, P(A, i)), eps)) c++; }
    return c / tot;
  };
  let bestT = null, bestScore = -1;
  for (let it = 0; it < iters; it++) {
    const m = [M[R.u32() % M.length], M[R.u32() % M.length], M[R.u32() % M.length]];
    if (m[0][0] === m[1][0] || m[1][0] === m[2][0] || m[0][0] === m[2][0]) continue;
    const a = m.map((x) => P(A, x[0])), b = m.map((x) => P(B, x[1]));
    const T = similarity(a, b);
    if (!(T.s > 0) || !isFinite(T.s)) continue;
    // cheap check on correspondences first, then on a subsample
    let ok = 0;
    for (const x of M) { const y = apply(T, P(A, x[0])), z = P(B, x[1]); if ((y[0] - z[0]) ** 2 + (y[1] - z[1]) ** 2 + (y[2] - z[2]) ** 2 < eps * eps * 4) ok++; }
    if (ok < 6) continue;
    const sc = countIn(T, 8);
    if (sc > bestScore) { bestScore = sc; bestT = T; }
  }
  if (!bestT) return { coverage: 0, scale: 0, mirrored: false };
  // refine on inliers (ICP-style, a few rounds of nearest-neighbour correspondences)
  let T = bestT;
  for (let round = 0; round < 4; round++) {
    const a = [], b = [];
    for (let i = 0; i < A.count; i += 2) {
      const y = apply(T, P(A, i));
      let bj = -1, bd = eps * eps * 4;
      for (const j of nearIdx(B, y, eps * 2, g)) { const d = (B.pts[j * 3] - y[0]) ** 2 + (B.pts[j * 3 + 1] - y[1]) ** 2 + (B.pts[j * 3 + 2] - y[2]) ** 2; if (d < bd) { bd = d; bj = j; } }
      if (bj >= 0) { a.push(P(A, i)); b.push(P(B, bj)); }
    }
    if (a.length < 10) break;
    const T2 = similarity(a, b);
    if (T2.s > 0 && countIn(T2, 2) >= countIn(T, 2)) T = T2; else break;
  }
  const det = T.R[0][0] * (T.R[1][1] * T.R[2][2] - T.R[1][2] * T.R[2][1]) - T.R[0][1] * (T.R[1][0] * T.R[2][2] - T.R[1][2] * T.R[2][0]) + T.R[0][2] * (T.R[1][0] * T.R[2][1] - T.R[1][1] * T.R[2][0]);
  return { coverage: countIn(T, 1), scale: T.s, mirrored: det < 0 };
}

function nearIdx(B, y, r, _g) {
  // small helper: brute-force within the grid's neighbourhood is enough at these sizes
  const out = [];
  const r2 = r * r;
  for (let j = 0; j < B.count; j++) { const d = (B.pts[j * 3] - y[0]) ** 2 + (B.pts[j * 3 + 1] - y[1]) ** 2 + (B.pts[j * 3 + 2] - y[2]) ** 2; if (d <= r2) out.push(j); }
  return out;
}
