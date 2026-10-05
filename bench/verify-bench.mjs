// Stage-2 benchmark: geometric coverage on (a) every attacked copy vs its original and
// (b) hard negatives = each attacked original's top-5 unrelated shortlist candidates.
//   node bench/verify-bench.mjs <shard> <nshards>   -> results/verify-<shard>.json
import { readFileSync, writeFileSync, readdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parseStl, compare } from "../packages/fp/src/fp.mjs";
import { coverage } from "../packages/fp/src/verify.mjs";
const HERE = dirname(fileURLToPath(import.meta.url)), DATA = join(HERE, "data");
const [shard, nsh] = [Number(process.argv[2] || 0), Number(process.argv[3] || 1)];
const cache = JSON.parse(readFileSync(join(HERE, "results/fps.json")));
const origs = Object.keys(cache).filter((k) => !k.includes("__") && !cache[k].error);
const score = (c) => 0.5 * (1 - c.f1) + 0.5 * c.f3;
const jobs = [];
for (const f of readdirSync(join(DATA, "attacked"))) {
  const [id, a] = f.replace(".stl", "").split("__");
  jobs.push({ kind: "pos", attack: a, a: join(DATA, "raw", id + ".stl"), b: join(DATA, "attacked", f), id });
}
const attacked = [...new Set(jobs.map((j) => j.id))];
for (const id of attacked) {
  const top = origs.filter((o) => o !== id).map((o) => [o, score(compare(cache[id], cache[o]))]).sort((x, y) => y[1] - x[1]).slice(0, 5);
  for (const [o] of top) jobs.push({ kind: "neg", attack: "-", a: join(DATA, "raw", id + ".stl"), b: join(DATA, "raw", o + ".stl"), id: id + "~" + o });
}
const mine = jobs.filter((_, i) => i % nsh === shard);
const load = new Map();
const tris = (p) => { if (!load.has(p)) load.set(p, parseStl(readFileSync(p))); return load.get(p); };
const out = [];
for (const j of mine) {
  const t0 = Date.now();
  // A = the (possible) original, B = the suspect; also the reverse, for cuts
  const ab = coverage(tris(j.a), tris(j.b)), ba = coverage(tris(j.b), tris(j.a));
  out.push({ ...j, ab: ab.coverage, ba: ba.coverage, abt: ab.tight, bat: ba.tight, abd: ab.distinctive, bad: ba.distinctive, detA: ab.detail, detB: ba.detail, scale: ab.scale, mirrored: ab.mirrored, ms: Date.now() - t0 });
  if (load.size > 40) load.clear();
}
writeFileSync(join(HERE, `results/verify-${shard}.json`), JSON.stringify(out));
console.log(`shard ${shard}: ${out.length} pairs`);
