// Summarise stage-2 results: coverage distributions, and detection at thresholds set on the
// hard negatives (each original's top-5 most similar unrelated models from the shortlist).
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
const dir = new URL("./results/", import.meta.url);
const rows = readdirSync(dir).filter((f) => /^verify-\d+\.json$/.test(f)).flatMap((f) => JSON.parse(readFileSync(new URL(f, dir))));
const neg = rows.filter((r) => r.kind === "neg"), pos = rows.filter((r) => r.kind === "pos");
const pct = (xs, p) => xs.slice().sort((a, b) => a - b)[Math.floor((xs.length - 1) * p)];
const SC = {
  "coverage (max of both directions)": (r) => Math.max(r.ab, r.ba),
  "TIGHT coverage, exact surface + normals (max)": (r) => Math.max(r.abt ?? 0, r.bat ?? 0),
};
let md = `# Stage 2: geometric coverage\n\n- positives ${pos.length}, hard negatives ${neg.length} (top-5 shortlist look-alikes per original)\n`;
md += `- time per pair (both directions): median ${pct(rows.map((r) => r.ms), 0.5)} ms\n\n`;
for (const [name, sc] of Object.entries(SC)) {
  const nv = neg.map(sc).sort((a, b) => b - a);
  const t1 = nv[Math.max(0, Math.ceil(nv.length * 0.01) - 1)], t5 = nv[Math.max(0, Math.ceil(nv.length * 0.05) - 1)];
  md += `## ${name}\n\nhard negatives: median ${pct(nv, 0.5).toFixed(3)}, p90 ${pct(nv, 0.9).toFixed(3)}, max ${nv[0].toFixed(3)}; FPR 1% above ${t1.toFixed(3)}, FPR 5% above ${t5.toFixed(3)}\n\n`;
  md += `| attack | median | p10 | detected @ FPR 1% | @ FPR 5% |\n|---|---|---|---|---|\n`;
  for (const a of [...new Set(pos.map((r) => r.attack))].sort()) {
    const v = pos.filter((r) => r.attack === a).map(sc);
    md += `| ${a} | ${pct(v, 0.5).toFixed(3)} | ${pct(v, 0.1).toFixed(3)} | ${((v.filter((x) => x > t1).length / v.length) * 100).toFixed(0)}% | ${((v.filter((x) => x > t5).length / v.length) * 100).toFixed(0)}% |\n`;
  }
  md += `\nworst hard negatives: ` + neg.slice().sort((x, y) => sc(y) - sc(x)).slice(0, 6).map((r) => `${r.id} ${sc(r).toFixed(2)}`).join(", ") + "\n\n";
}
writeFileSync(new URL("verify.md", dir), md);
console.log(md);
