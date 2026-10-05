// Retrieval recall: for each attacked copy, rank all originals by the F1+F3 score.
import { readFileSync } from "node:fs";
import { compare } from "../packages/fp/src/fp.mjs";
const cache = JSON.parse(readFileSync(new URL("./results/fps.json", import.meta.url)));
const origs = Object.keys(cache).filter((k) => !k.includes("__") && !cache[k].error);
const score = (c) => 0.5 * (1 - c.f1) + 0.5 * c.f3;
const by = {};
for (const k of Object.keys(cache).filter((k) => k.includes("__") && !cache[k].error)) {
  const [orig, att] = k.split("__");
  const ranked = origs.map((o) => [o, score(compare(cache[o], cache[k]))]).sort((a, b) => b[1] - a[1]);
  const rank = ranked.findIndex((r) => r[0] === orig) + 1;
  (by[att] ||= []).push(rank);
}
console.log("| attack | recall@1 | @5 | @20 | median rank |\n|---|---|---|---|---|");
for (const [a, rs] of Object.entries(by).sort()) {
  const r = (k) => ((rs.filter((x) => x <= k).length / rs.length) * 100).toFixed(0) + "%";
  rs.sort((x, y) => x - y);
  console.log(`| ${a} | ${r(1)} | ${r(5)} | ${r(20)} | ${rs[rs.length >> 1]} |`);
}
