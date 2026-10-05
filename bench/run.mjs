// Fingerprint benchmark: detection of attacked copies vs false positives on unrelated models.
//   node bench/run.mjs [--fresh]   -> bench/results/report.md (+ fps cache)
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parseStl, fingerprint, compare, PARAMS, FP_VERSION } from "../packages/fp/src/fp.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const DATA = join(HERE, "data"), RES = join(HERE, "results");
mkdirSync(RES, { recursive: true });
const cachePath = join(RES, "fps.json");
const fresh = process.argv.includes("--fresh");
const cache = !fresh && existsSync(cachePath) ? JSON.parse(readFileSync(cachePath)) : {};

const sel = JSON.parse(readFileSync(join(DATA, "selection.json")));
const files = [];
for (const s of sel) files.push({ key: s.id, path: join(DATA, "raw", s.id + ".stl"), orig: s.id, attack: null });
for (const f of readdirSync(join(DATA, "attacked"))) {
  const [id, a] = f.replace(".stl", "").split("__");
  files.push({ key: `${id}__${a}`, path: join(DATA, "attacked", f), orig: id, attack: a });
}

let t = 0, n = 0;
for (const f of files) {
  if (cache[f.key]) continue;
  const t0 = Date.now();
  try { cache[f.key] = fingerprint(parseStl(readFileSync(f.path))); } catch (e) { cache[f.key] = { error: String(e) }; }
  t += Date.now() - t0; n++;
  if (n % 50 === 0) console.log(`fingerprinted ${n}`);
}
writeFileSync(cachePath, JSON.stringify(cache));
console.log(`fingerprinted ${n} new files, ${n ? (t / n).toFixed(0) : "-"} ms avg`);

const fp = (k) => (cache[k] && !cache[k].error ? cache[k] : null);
// Scores: higher = more similar.
const SCORES = {
  "F1 (1 - global distance)": (c) => 1 - c.f1,
  "F2 Jaccard": (c) => c.jaccard,
  "F2 containment (max)": (c) => Math.max(c.containAinB, c.containBinA),
  "F3 local histogram intersection": (c) => c.f3,
  "F3 coverage @0.7": (c) => c.f3cov,
  "F1 + F3": (c) => 0.5 * (1 - c.f1) + 0.5 * c.f3,
  "F3 IDF presence containment": (c) => c.pc,
  "F1 + F3 + containment": (c) => Math.max(0.5 * (1 - c.f1) + 0.5 * c.f3, c.pc - 0.08),
};

// IDF over local features, from the originals NOT used for attacks (a reference corpus; in the
// product this table ships with the fingerprint version so every client scores identically).
const ATTACKED = new Set(files.filter((f) => f.attack).map((f) => f.orig));
const ref = sel.map((s) => s.id).filter((id) => fp(id) && !ATTACKED.has(id));
const PRES_A = 3 * 4, PRES_B = 1 * 4;          // ~3 and ~1 occurrences out of 16384 tokens (uint16-scaled)
const df = new Float64Array(fp(ref[0]).f3.length);
for (const id of ref) fp(id).f3.forEach((v, t) => { if (v >= PRES_A) df[t]++; });
const idf = Array.from(df, (d) => Math.log((ref.length + 1) / (d + 1)));
function presenceContain(a, b) {
  let num = 0, den = 0;
  for (let t = 0; t < a.f3.length; t++) if (a.f3[t] >= PRES_A) { den += idf[t]; if (b.f3[t] >= PRES_B) num += idf[t]; }
  return den ? num / den : 0;
}
const _compare = compare;
const compareX = (a, b) => { const c = _compare(a, b); c.pc = Math.max(presenceContain(a, b), presenceContain(b, a)); c.pcMin = Math.min(presenceContain(a, b), presenceContain(b, a)); return c; };

// negatives: every pair of distinct originals
const origs = sel.map((s) => s.id).filter((id) => fp(id));
const neg = [];
for (let i = 0; i < origs.length; i++) for (let j = i + 1; j < origs.length; j++) neg.push([origs[i], origs[j], compareX(fp(origs[i]), fp(origs[j]))]);
// positives: original vs its attacked versions
const pos = files.filter((f) => f.attack && fp(f.key) && fp(f.orig)).map((f) => [f.orig, f.attack, compareX(fp(f.orig), fp(f.key))]);
const attacks = [...new Set(pos.map((p) => p[1]))].sort();

function thresholdAt(vals, fpr) {
  const s = vals.slice().sort((a, b) => b - a);
  return s[Math.max(0, Math.floor(s.length * fpr) - 1)] ?? 1;
}

let md = `# Fingerprint benchmark (fp/v${FP_VERSION})\n\n`;
md += `- originals: ${origs.length}, unrelated pairs (negatives): ${neg.length}, attacked copies: ${pos.length}\n`;
md += `- params: \`${JSON.stringify(PARAMS)}\`\n\n`;
for (const [name, fn] of Object.entries(SCORES)) {
  const nv = neg.map((x) => fn(x[2]));
  md += `## ${name}\n\n| attack | median score | detected @ FPR 1% | detected @ FPR 0.1% |\n|---|---|---|---|\n`;
  const t1 = thresholdAt(nv, 0.01), t01 = thresholdAt(nv, 0.001);
  for (const a of attacks) {
    const pv = pos.filter((p) => p[1] === a).map((p) => fn(p[2])).sort((x, y) => x - y);
    const med = pv[Math.floor(pv.length / 2)];
    md += `| ${a} | ${med.toFixed(3)} | ${((pv.filter((v) => v > t1).length / pv.length) * 100).toFixed(0)}% | ${((pv.filter((v) => v > t01).length / pv.length) * 100).toFixed(0)}% |\n`;
  }
  const negSorted = nv.slice().sort((a, b) => b - a);
  md += `\nthresholds: FPR 1% → ${t1.toFixed(3)}, FPR 0.1% → ${t01.toFixed(3)}; unrelated median ${negSorted[Math.floor(negSorted.length / 2)].toFixed(3)}, max ${negSorted[0].toFixed(3)}\n\n`;
}
// the most similar unrelated pairs (to eyeball: true look-alikes or descriptor weakness?)
const top = neg.map((x) => [x[0], x[1], SCORES["F1 + F3 + containment"](x[2])]).sort((a, b) => b[2] - a[2]).slice(0, 10);
md += `## Most similar unrelated pairs (combined)\n\n` + top.map((x) => `- ${x[0]} vs ${x[1]}: ${x[2].toFixed(3)}`).join("\n") + "\n";
writeFileSync(join(RES, "report.md"), md);
console.log(md);
