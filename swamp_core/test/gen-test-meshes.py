#!/usr/bin/env python3
"""Small procedural test meshes, so the tests run on a fresh clone (the Thingi10K benchmark set in
bench/data is git-ignored). Deterministic: same bytes every run.

  python3 gen-test-meshes.py <outdir>   -> torus.stl, bracket.stl, twist.stl, blob.stl (binary)
                                           torus-ascii.stl (ASCII, for the decimal-comma locale test)
"""
import math, struct, sys, os

def tri_normal(a, b, c):
    u = [b[i] - a[i] for i in range(3)]; v = [c[i] - a[i] for i in range(3)]
    n = [u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]]
    l = math.sqrt(sum(x*x for x in n)) or 1.0
    return [x / l for x in n]

def grid(f, nu, nv, wrap_u=True, wrap_v=True):
    tris = []
    for i in range(nu):
        for j in range(nv):
            i1 = (i + 1) % nu if wrap_u else i + 1
            j1 = (j + 1) % nv if wrap_v else j + 1
            a, b, c, d = f(i, j), f(i1, j), f(i1, j1), f(i, j1)
            tris += [(a, b, c), (a, c, d)]
    return tris

def torus(R=20.0, r=6.0, nu=48, nv=24):
    def p(i, j):
        u, v = 2*math.pi*i/nu, 2*math.pi*j/nv
        return ((R + r*math.cos(v))*math.cos(u), (R + r*math.cos(v))*math.sin(u), r*math.sin(v))
    return grid(p, nu, nv)

def box(x0, y0, z0, x1, y1, z1):
    v = [(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),(x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    f = [(0,2,1),(0,3,2),(4,5,6),(4,6,7),(0,1,5),(0,5,4),(1,2,6),(1,6,5),(2,3,7),(2,7,6),(3,0,4),(3,4,7)]
    return [(v[a], v[b], v[c]) for a, b, c in f]

def bracket():   # an L-shaped bracket with a few ribs - lots of flat faces and right angles
    t = box(0, 0, 0, 60, 20, 4) + box(0, 0, 0, 4, 20, 40)
    for k in range(3): t += box(10 + 18*k, 8, 4, 13 + 18*k, 12, 18 - 4*k)
    return t

def twist(n=6, layers=40, h=50.0, r=15.0, turn=math.pi/2):   # a twisted hexagonal prism
    def p(i, j):
        a = 2*math.pi*i/n + turn*j/layers
        return (r*math.cos(a), r*math.sin(a), h*j/layers)
    side = grid(p, n, layers, True, False)
    caps = []
    for j in (0, layers):
        c = (0.0, 0.0, h*j/layers)
        for i in range(n):
            a, b = p(i, j), p((i + 1) % n, j)
            caps.append((c, b, a) if j == 0 else (c, a, b))
    return side + caps

def blob(nu=40, nv=20):   # a lumpy sphere
    def p(i, j):
        u, v = 2*math.pi*i/nu, math.pi*(j/nv)
        rr = 18 + 3*math.sin(3*u)*math.sin(2*v) + 2*math.cos(5*v)
        return (rr*math.sin(v)*math.cos(u), rr*math.sin(v)*math.sin(u), rr*math.cos(v))
    return grid(p, nu, nv, True, False)

def write_binary(path, tris):
    with open(path, "wb") as f:
        f.write(b"swamp test mesh".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for a, b, c in tris:
            f.write(struct.pack("<12fH", *tri_normal(a, b, c), *a, *b, *c, 0))

def write_ascii(path, tris):
    with open(path, "w") as f:
        f.write("solid swamp\n")
        for a, b, c in tris:
            f.write("  facet normal %e %e %e\n    outer loop\n" % tuple(tri_normal(a, b, c)))
            for v in (a, b, c): f.write("      vertex %e %e %e\n" % v)
            f.write("    endloop\n  endfacet\n")
        f.write("endsolid swamp\n")

out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)
write_binary(out + "/torus.stl", torus())
write_binary(out + "/bracket.stl", bracket())
write_binary(out + "/twist.stl", twist())
write_binary(out + "/blob.stl", blob())
write_ascii(out + "/torus-ascii.stl", torus(nu=32, nv=16))
