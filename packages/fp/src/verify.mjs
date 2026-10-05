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

// FPFH-style local descriptor (Rusu 2009), with unsigned normals so mirrored copies and
// inconsistent winding don't matter. 3 features x 11 bins = 33 dims per point.
const NB = 16, BINS = 11;
function descriptors(S) {
  const n = S.count, p = S.pts, q = S.nrm;
  const { nb, nd } = knn(p, n, NB);
  const spfh = new Float64Array(n * 3 * BINS);
  const bin = (x) => Math.min(BINS - 1, Math.max(0, Math.floor(x * BINS)));
  for (let i = 0; i < n; i++) {
    const ux = q[i * 3], uy = q[i * 3 + 1], uz = q[i * 3 + 2];
    for (let k = 0; k < NB; k++) {
      const j = nb[i * NB + k], d = nd[i * NB + k];
      if (j === i || d === 0) continue;
      const dx = (p[j * 3] - p[i * 3]) / d, dy = (p[j * 3 + 1] - p[i * 3 + 1]) / d, dz = (p[j * 3 + 2] - p[i * 3 + 2]) / d;
      // Darboux frame: u = n_i, v = u x d, w = u x v
      let vx = uy * dz - uz * dy, vy = uz * dx - ux * dz, vz = ux * dy - uy * dx;
      const vl = Math.hypot(vx, vy, vz) || 1; vx /= vl; vy /= vl; vz /= vl;
      const wx = uy * vz - uz * vy, wy = uz * vx - ux * vz, wz = ux * vy - uy * vx;
      const nx = q[j * 3], ny = q[j * 3 + 1], nz = q[j * 3 + 2];
      const alpha = Math.abs(vx * nx + vy * ny + vz * nz);
      const phi = Math.abs(ux * dx + uy * dy + uz * dz);
      const theta = Math.atan2(Math.abs(wx * nx + wy * ny + wz * nz), Math.abs(ux * nx + uy * ny + uz * nz)) / (Math.PI / 2);
      const o = i * 3 * BINS;
      spfh[o + bin(alpha)] += 1; spfh[o + BINS + bin(phi)] += 1; spfh[o + 2 * BINS + bin(theta)] += 1;
    }
  }
  // distinctiveness: 0 on flat surface, up to 1 at edges / curvature / detail
  const weight = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    let agree = 0;
    for (let k = 0; k < NB; k++) { const j = nb[i * NB + k]; agree += Math.abs(q[i * 3] * q[j * 3] + q[i * 3 + 1] * q[j * 3 + 1] + q[i * 3 + 2] * q[j * 3 + 2]); }
    weight[i] = Math.min(1, Math.max(0, (1 - agree / NB) * 4));
  }
  const D = new Float64Array(n * 3 * BINS);
  const spacing = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    spacing[i] = nd[i * NB];
    const o = i * 3 * BINS;
    for (let t = 0; t < 3 * BINS; t++) D[o + t] = spfh[o + t];
    for (let k = 0; k < NB; k++) {
      const j = nb[i * NB + k], w = 1 / NB;
      for (let t = 0; t < 3 * BINS; t++) D[o + t] += w * spfh[j * 3 * BINS + t];
    }
    let s2 = 0; for (let t = 0; t < 3 * BINS; t++) s2 += D[o + t] * D[o + t];
    s2 = Math.sqrt(s2) || 1; for (let t = 0; t < 3 * BINS; t++) D[o + t] /= s2;
  }
  const sorted = Array.from(spacing).sort((a, b) => a - b);
  return { D, dim: 3 * BINS, spacing: sorted[sorted.length >> 1], weight };
}

function nearestDesc(Dx, i, Dy, ny, dim) {
  let best = -1, bd = Infinity;
  for (let j = 0; j < ny; j++) {
    let s = 0;
    for (let k = 0; k < dim; k++) { const x = Dx[i * dim + k] - Dy[j * dim + k]; s += x * x; if (s >= bd) break; }
    if (s < bd) { bd = s; best = j; }
  }
  return [best, bd];
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
export function coverage(trisA, trisB, { samples = 1024, iters = 4000, seed = 0x7e57 } = {}) {
  const A = samplePoints(trisA, samples, 0x5a3b), B = samplePoints(trisB, samples * 2, 0x5a3c);
  const da = descriptors(A), db = descriptors(B);
  const dim = da.dim;
  // mutual nearest neighbours in descriptor space
  const matches = [];
  const bBest = new Int32Array(B.count).fill(-2);
  for (let i = 0; i < A.count; i++) {
    const [j, d] = nearestDesc(da.D, i, db.D, B.count, dim);
    if (j < 0) continue;
    if (bBest[j] === -2) bBest[j] = nearestDesc(db.D, j, da.D, A.count, dim)[0];
    if (bBest[j] === i) matches.push([i, j, d]);
  }
  matches.sort((x, y) => x[2] - y[2]);
  const M = matches.length >= 3 ? matches : [];
  if (M.length < 3) return { coverage: 0, tight: 0, distinctive: 0, detail: 0, scale: 0, mirrored: false };
  const P = (S, i) => [S.pts[i * 3], S.pts[i * 3 + 1], S.pts[i * 3 + 2]];
  // A point on B's surface lies within eps of one of B's N random samples with probability
  // 1 - exp(-pi * N * eps^2 / area); eps = 1.2 * sqrt(area / N) gives ~99%.
  const eps = 1.2 * Math.sqrt(B.area / B.count);
  const g = grid(B.pts, B.count, eps);
  const R = rng(seed);
  // If A is contained in B (rotated, mirrored, uniformly scaled by s), then s^2 * area(A) <= area(B).
  // Reject transforms outside [0.3, 1.15] x that bound: tiny scales collapse A onto one spot of B
  // and would "cover" anything.
  const s0 = Math.sqrt(B.area / A.area);
  // the copied part must be at least ~25% of the suspect's surface: s >= 0.5 * s0
  const plausible = (T) => T.s > 0 && isFinite(T.s) && T.s <= 1.15 * s0 && T.s >= 0.5 * s0;
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
    // the three edge lengths must agree up to one common scale (cheap rejection)
    const ea = [dist3(a[0], a[1]), dist3(a[1], a[2]), dist3(a[0], a[2])], eb = [dist3(b[0], b[1]), dist3(b[1], b[2]), dist3(b[0], b[2])];
    if (Math.min(...ea) < 3 * Math.sqrt(A.area / A.count)) continue;
    const r = [eb[0] / ea[0], eb[1] / ea[1], eb[2] / ea[2]];
    if (Math.max(...r) > 1.1 * Math.min(...r)) continue;
    const T = similarity(a, b);
    if (!plausible(T)) continue;
    // cheap check on correspondences first, then on a subsample
    let ok = 0;
    for (const x of M) { const y = apply(T, P(A, x[0])), z = P(B, x[1]); if ((y[0] - z[0]) ** 2 + (y[1] - z[1]) ** 2 + (y[2] - z[2]) ** 2 < eps * eps * 4) ok++; }
    if (ok < 4) continue;
    const sc = countIn(T, 8);
    if (sc > bestScore) { bestScore = sc; bestT = T; }
  }
  if (!bestT) return { coverage: 0, tight: 0, distinctive: 0, detail: 0, scale: 0, mirrored: false };
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
    if (plausible(T2) && countIn(T2, 2) >= countIn(T, 2)) T = T2; else break;
  }
  const det = T.R[0][0] * (T.R[1][1] * T.R[2][2] - T.R[1][2] * T.R[2][1]) - T.R[0][1] * (T.R[1][0] * T.R[2][2] - T.R[1][2] * T.R[2][0]) + T.R[0][2] * (T.R[1][0] * T.R[2][1] - T.R[1][1] * T.R[2][0]);
  // Stage 2b: exact-surface ICP + tight coverage.
  const mi = meshIndex(trisB);
  const sizeA = T.s * (() => { let mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity]; for (let i = 0; i < A.count; i++) for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], A.pts[i * 3 + k]); mx[k] = Math.max(mx[k], A.pts[i * 3 + k]); } return Math.hypot(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]); })();
  const tol = 0.006 * sizeA;
  const Tt = icpToMesh(A, T, mi, eps * 1.5, tol);
  const tight = plausible(Tt) ? tightCoverage(A, Tt, mi, tol) : tightCoverage(A, T, mi, tol);
  let wIn = 0, wTot = 0;
  for (let i = 0; i < A.count; i++) { const w = da.weight[i]; wTot += w; if (g.near(apply(T, P(A, i)), eps)) wIn += w; }
  return { coverage: countIn(T, 1), tight, distinctive: wTot ? wIn / wTot : 0, detail: wTot / A.count, scale: T.s, mirrored: det < 0 };
}

const dist3 = (u, v) => Math.hypot(u[0] - v[0], u[1] - v[1], u[2] - v[2]);

// ── exact point-to-mesh distance (closest point on triangle, Ericson 5.1.5) ─────────
function closestOnTri(p, a, b, c) {
  const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]], ac = [c[0] - a[0], c[1] - a[1], c[2] - a[2]], ap = [p[0] - a[0], p[1] - a[1], p[2] - a[2]];
  const dot = (u, v) => u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
  const d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return a;
  const bp = [p[0] - b[0], p[1] - b[1], p[2] - b[2]], d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return b;
  const vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) { const v = d1 / (d1 - d3); return [a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]]; }
  const cp = [p[0] - c[0], p[1] - c[1], p[2] - c[2]], d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return c;
  const vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) { const w = d2 / (d2 - d6); return [a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]]; }
  const va = d3 * d6 - d5 * d4;
  if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) { const w = (d4 - d3) / (d4 - d3 + (d5 - d6)); return [b[0] + w * (c[0] - b[0]), b[1] + w * (c[1] - b[1]), b[2] + w * (c[2] - b[2])]; }
  const den = 1 / (va + vb + vc), v = vb * den, w = vc * den;
  return [a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w, a[2] + ab[2] * v + ac[2] * w];
}

function meshIndex(tris) {
  const n = tris.length / 9;
  let mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < tris.length; i += 3) for (let k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], tris[i + k]); mx[k] = Math.max(mx[k], tris[i + k]); }
  const diag = Math.hypot(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
  const cell = diag / 48;
  const g = new Map();
  const nrm = new Float64Array(n * 3);
  for (let t = 0; t < n; t++) {
    const o = t * 9;
    const ux = tris[o + 3] - tris[o], uy = tris[o + 4] - tris[o + 1], uz = tris[o + 5] - tris[o + 2];
    const vx = tris[o + 6] - tris[o], vy = tris[o + 7] - tris[o + 1], vz = tris[o + 8] - tris[o + 2];
    let nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx; const l = Math.hypot(nx, ny, nz) || 1;
    nrm[t * 3] = nx / l; nrm[t * 3 + 1] = ny / l; nrm[t * 3 + 2] = nz / l;
    const lo = [0, 1, 2].map((k) => Math.floor(Math.min(tris[o + k], tris[o + 3 + k], tris[o + 6 + k]) / cell));
    const hi = [0, 1, 2].map((k) => Math.floor(Math.max(tris[o + k], tris[o + 3 + k], tris[o + 6 + k]) / cell));
    for (let x = lo[0]; x <= hi[0]; x++) for (let y = lo[1]; y <= hi[1]; y++) for (let z = lo[2]; z <= hi[2]; z++) {
      const key = x + "," + y + "," + z; let arr = g.get(key); if (!arr) g.set(key, (arr = [])); arr.push(t);
    }
  }
  return {
    diag,
    // closest surface point within radius r: [dist, normal] or null
    closest(p, r) {
      const reach = Math.ceil(r / cell);
      const c = [0, 1, 2].map((k) => Math.floor(p[k] / cell));
      let best = Infinity, bt = -1, seen = new Set();
      for (let x = -reach; x <= reach; x++) for (let y = -reach; y <= reach; y++) for (let z = -reach; z <= reach; z++) {
        const arr = g.get(c[0] + x + "," + (c[1] + y) + "," + (c[2] + z)); if (!arr) continue;
        for (const t of arr) {
          if (seen.has(t)) continue; seen.add(t);
          const o = t * 9;
          const q = closestOnTri(p, [tris[o], tris[o + 1], tris[o + 2]], [tris[o + 3], tris[o + 4], tris[o + 5]], [tris[o + 6], tris[o + 7], tris[o + 8]]);
          const d = Math.hypot(q[0] - p[0], q[1] - p[1], q[2] - p[2]);
          if (d < best) { best = d; bt = t; this._q = q; }
        }
      }
      return best <= r ? { d: best, q: this._q, n: [nrm[bt * 3], nrm[bt * 3 + 1], nrm[bt * 3 + 2]] } : null;
    },
  };
}

/**
 * Tight check after alignment: fraction of A's samples lying within tol (default 0.6% of the
 * aligned model's size) of B's actual surface, with agreeing surface normals. Real copies
 * coincide almost exactly; unrelated shapes don't.
 */
function tightCoverage(A, T, mi, tol) {
  let hit = 0;
  for (let i = 0; i < A.count; i++) {
    const p = apply(T, [A.pts[i * 3], A.pts[i * 3 + 1], A.pts[i * 3 + 2]]);
    const c = mi.closest(p, tol);
    if (!c) continue;
    const n = [0, 1, 2].map((r) => T.R[r][0] * A.nrm[i * 3] + T.R[r][1] * A.nrm[i * 3 + 1] + T.R[r][2] * A.nrm[i * 3 + 2]);
    if (Math.abs(n[0] * c.n[0] + n[1] * c.n[1] + n[2] * c.n[2]) > 0.8) hit++;
  }
  return hit / A.count;
}

function icpToMesh(A, T, mi, startR, endR, rounds = 8) {
  let r = startR;
  for (let k = 0; k < rounds; k++, r = Math.max(endR, r * 0.6)) {
    const a = [], b = [];
    for (let i = 0; i < A.count; i += 2) {
      const x = [A.pts[i * 3], A.pts[i * 3 + 1], A.pts[i * 3 + 2]];
      const c = mi.closest(apply(T, x), r);
      if (c) { a.push(x); b.push(c.q); }
    }
    if (a.length < 12) break;
    const T2 = similarity(a, b);
    if (!(T2.s > 0) || !isFinite(T2.s)) break;
    T = T2;
  }
  return T;
}

function nearIdx(B, y, r, _g) {
  // small helper: brute-force within the grid's neighbourhood is enough at these sizes
  const out = [];
  const r2 = r * r;
  for (let j = 0; j < B.count; j++) { const d = (B.pts[j * 3] - y[0]) ** 2 + (B.pts[j * 3 + 1] - y[1]) ** 2 + (B.pts[j * 3 + 2] - y[2]) ** 2; if (d <= r2) out.push(j); }
  return out;
}
