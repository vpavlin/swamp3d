#!/usr/bin/env python3
"""Generate the scripted attack suite for the fingerprint benchmark.

For N originals (from data/selection.json), write one binary STL per attack to
data/attacked/<id>__<attack>.stl. Attacks are what a model thief does in practice
(notes/05a section 1.1). Deterministic (fixed seeds).

    .venv/bin/python bench/attacks.py [N]
"""
import json, sys, os, random
import numpy as np
import trimesh
import manifold3d as m3d
import fast_simplification

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
OUT = os.path.join(DATA, "attacked")
N = int(sys.argv[1]) if len(sys.argv) > 1 else 60


def to_manifold(mesh):
    return m3d.Manifold(m3d.Mesh(vert_properties=np.asarray(mesh.vertices, dtype=np.float32),
                                 tri_verts=np.asarray(mesh.faces, dtype=np.uint32)))


def from_manifold(man):
    mm = man.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mm.vert_properties)[:, :3], faces=np.asarray(mm.tri_verts), process=False)


def union(mesh, other):
    try:
        r = from_manifold(to_manifold(mesh) + to_manifold(other))
        if len(r.faces) > 0:
            return r
    except Exception:
        pass
    return trimesh.util.concatenate([mesh, other])   # fall back: touching shells


def rot(seed):
    r = np.random.default_rng(seed)
    q = r.normal(size=4); q /= np.linalg.norm(q)
    return trimesh.transformations.quaternion_matrix(q)


def attacks(mesh, seed):
    ext = mesh.extents
    lo, hi = mesh.bounds
    size = float(np.max(ext))
    out = {}

    # re-export: shuffled faces + rotated vertex order (same geometry, different file)
    r = np.random.default_rng(seed)
    f = mesh.faces[r.permutation(len(mesh.faces))]
    out["reexport"] = trimesh.Trimesh(mesh.vertices, np.roll(f, 1, axis=1), process=False)

    m = mesh.copy(); m.apply_transform(rot(seed)); m.apply_translation([size * 3, -size, size / 2])
    out["rotate"] = m
    m = mesh.copy(); m.apply_scale(1.37); out["scale"] = m
    m = mesh.copy(); m.apply_transform(np.diag([1.0, 1.0, 1.25, 1.0])); out["stretch"] = m
    m = mesh.copy(); m.apply_transform(np.diag([-1.0, 1.0, 1.0, 1.0])); m.invert(); out["mirror"] = m

    v, fa = fast_simplification.simplify(np.asarray(mesh.vertices, np.float32), np.asarray(mesh.faces, np.int32), target_reduction=0.5)
    out["decimate50"] = trimesh.Trimesh(v, fa, process=False)
    v, fa = trimesh.remesh.subdivide(mesh.vertices, mesh.faces)
    out["subdivide"] = trimesh.Trimesh(v, fa, process=False)
    m = mesh.copy(); m.vertices = m.vertices + r.normal(scale=size * 0.002, size=m.vertices.shape); out["noise"] = m

    # add a base plate under the model (the most common "my own design" edit)
    base = trimesh.creation.box(extents=[ext[0] * 1.3, ext[1] * 1.3, max(ext[2] * 0.08, size * 0.03)])
    base.apply_translation([(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2] - base.extents[2] / 2 + size * 0.005])
    out["add_base"] = union(mesh, base)

    # add a raised "signature" block on one side (stand-in for embossed text)
    tag = trimesh.creation.box(extents=[ext[0] * 0.3, size * 0.05, ext[2] * 0.15])
    tag.apply_translation([(lo[0] + hi[0]) / 2, hi[1] + size * 0.02, (lo[2] + hi[2]) / 2])
    out["add_text"] = union(mesh, tag)

    # cut: keep the part below 60% of the height, capped
    try:
        z = lo[2] + ext[2] * 0.6
        c = from_manifold(to_manifold(mesh).trim_by_plane((0.0, 0.0, -1.0), -z))   # keep z <= cut
        if len(c.faces) > 0:
            out["cut60"] = c
    except Exception as e:
        print("cut failed", e)

    # combined: decimate + base + scale + rotate (a lazy but determined thief)
    m = out["decimate50"].copy()
    m = union(m, base)
    m.apply_scale(0.8); m.apply_transform(rot(seed + 1))
    out["combo"] = m
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    sel = json.load(open(os.path.join(DATA, "selection.json")))[:N]
    done = 0
    for item in sel:
        src = os.path.join(DATA, "raw", item["id"] + ".stl")
        try:
            mesh = trimesh.load(src, force="mesh")
        except Exception as e:
            print("skip", item["id"], e); continue
        only = os.environ.get("ATTACKS")
        for name, m in attacks(mesh, int(item["id"])).items():
            if only and name not in only.split(","):
                continue
            m.export(os.path.join(OUT, f"{item['id']}__{name}.stl"), file_type="stl")
        done += 1
        print(f"{done}/{len(sel)} {item['id']}", flush=True)


if __name__ == "__main__":
    main()
