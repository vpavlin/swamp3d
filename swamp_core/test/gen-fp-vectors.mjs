// Golden fingerprints from the JS reference for the C++ parity test.
//   node swamp_core/test/gen-fp-vectors.mjs > swamp_core/test/fp-vectors.json
import { readFileSync } from "node:fs";
import { parseStl, fingerprint } from "../../packages/fp/src/fp.mjs";
// procedural meshes committed in test/meshes (gen-test-meshes.py), so this runs on a fresh clone
const ids = ["torus", "bracket", "twist", "blob", "torus-ascii"];
const out = ids.map((id) => {
  const f = fingerprint(parseStl(readFileSync(new URL(`./meshes/${id}.stl`, import.meta.url))));
  return { id, d2: f.d2, a3: f.a3, f3: f.f3 };
});
console.log(JSON.stringify(out));
