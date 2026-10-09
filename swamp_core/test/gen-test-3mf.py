#!/usr/bin/env python3
"""Writes test/meshes/bambu-style.3mf, laid out the way Bambu Studio / OrcaSlicer save projects:
the meshes live in 3D/Objects/*.model, the root model points at them with <component p:path=...>,
and both the components and the build items carry transforms. Entries are deflated. There's also a
non-printable item and some G-code in Metadata/, which the reader must ignore.

Shapes: item 1 is a unit cube scaled x10 at (100,100,0), 12 triangles; item 2 is a tetrahedron made
of two components (offsets 0 and +20 in x), 8 triangles; item 3 is not printable."""
import os, zipfile
here = os.path.dirname(os.path.abspath(__file__))
cube_v = [(x, y, z) for z in (0, 1) for y in (0, 1) for x in (0, 1)]
cube_t = [(0,2,1),(1,2,3),(4,5,6),(5,7,6),(0,1,4),(1,5,4),(2,6,3),(3,6,7),(0,4,2),(2,4,6),(1,3,5),(3,7,5)]
tet_v = [(0,0,0),(10,0,0),(0,10,0),(0,0,10)]
tet_t = [(0,2,1),(0,1,3),(0,3,2),(1,2,3)]
def mesh(oid, v, t):
    vs = "".join('<vertex x="%g" y="%g" z="%g"/>' % p for p in v)
    ts = "".join('<triangle v1="%d" v2="%d" v3="%d"/>' % p for p in t)
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
            '<resources><object id="%d" type="model"><mesh><vertices>%s</vertices><triangles>%s</triangles></mesh></object></resources><build/></model>' % (oid, vs, ts))
root = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" '
        'xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p"><resources>'
        '<object id="10" type="model"><components><component p:path="/3D/Objects/cube.model" objectid="1" transform="10 0 0 0 10 0 0 0 10 0 0 0"/></components></object>'
        '<object id="11" type="model"><components>'
        '<component p:path="/3D/Objects/tet.model" objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0"/>'
        '<component p:path="/3D/Objects/tet.model" objectid="1" transform="1 0 0 0 1 0 0 0 1 20 0 0"/>'
        '</components></object>'
        '<object id="12" type="model"><components><component p:path="/3D/Objects/cube.model" objectid="1"/></components></object>'
        '</resources><build>'
        '<item objectid="10" transform="1 0 0 0 1 0 0 0 1 100 100 0" printable="1"/>'
        '<item objectid="11" transform="1 0 0 0 1 0 0 0 1 50 50 0"/>'
        '<item objectid="12" printable="0"/>'
        '</build></model>')
out = os.path.join(here, "meshes", "bambu-style.3mf")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name, data in [("[Content_Types].xml", '<?xml version="1.0"?><Types/>'),
                       ("3D/3dmodel.model", root),
                       ("3D/Objects/cube.model", mesh(1, cube_v, cube_t)),
                       ("3D/Objects/tet.model", mesh(1, tet_v, tet_t)),
                       ("Metadata/plate_1.gcode", "G28\nM104 S300 ; a stranger's G-code\n"),
                       ("Metadata/project_settings.config", '{"nozzle_temperature": ["300"]}')]:
        info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)); info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, data)
print(out)

# inch.3mf: a one-inch cube in a model whose unit is inches -> 25.4 mm
inch = ('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="inch" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources>'
        '<object id="1" type="model"><mesh><vertices>' + "".join('<vertex x="%g" y="%g" z="%g"/>' % p for p in cube_v) + '</vertices><triangles>'
        + "".join('<triangle v1="%d" v2="%d" v3="%d"/>' % p for p in cube_t) + '</triangles></mesh></object></resources>'
        '<build><item objectid="1" transform="1 0 0 0 1 0 0 0 1 2 0 0"/></build></model>')
out2 = os.path.join(here, "meshes", "inch.3mf")
with zipfile.ZipFile(out2, "w", zipfile.ZIP_DEFLATED) as z:
    info = zipfile.ZipInfo("3D/3dmodel.model", (2026, 1, 1, 0, 0, 0)); info.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(info, inch)
print(out2)

# hostile files (review 2026-10-09), written to a scratch dir, not committed:  gen-test-3mf.py --hostile DIR
import sys, struct
if len(sys.argv) == 3 and sys.argv[1] == "--hostile":
    d = sys.argv[2]; os.makedirs(d, exist_ok=True)
    H = '<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources>'
    def mdl(objs, items): return H + objs + '</resources><build>' + items + '</build></model>'
    def strip(oid, n):
        return ('<object id="%d" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="10" y="0" z="0"/><vertex x="0" y="10" z="0"/></vertices><triangles>' % oid
                + '<triangle v1="0" v2="1" v3="2"/>' * n + '</triangles></mesh></object>')
    def put(name, parts):
        with zipfile.ZipFile(os.path.join(d, name), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for n, x in parts.items(): z.writestr(n, x)
    # component bomb: 16 levels x 8 components each, empty leaf (8^16 visits, no triangles)
    objs = "".join('<object id="%d" type="model"><components>' % i + '<component objectid="%d"/>' % (i + 1) * 8 + '</components></object>' for i in range(1, 17))
    put("component-bomb.3mf", {"3D/3dmodel.model": mdl(objs + '<object id="17" type="model"><mesh><vertices/><triangles/></mesh></object>', '<item objectid="1"/>')})
    # instancing: one 200k-triangle object on 60 build items (12M triangles out)
    put("instancing.3mf", {"3D/3dmodel.model": mdl(strip(1, 200000), '<item objectid="1"/>' * 60)})
    # many big parts: 12 parts of 2M triangles each (~66 MB of XML apiece)
    big = mdl(strip(1, 2000000), "")
    parts = {"3D/3dmodel.model": mdl(strip(1, 12), '<item objectid="1"/>')}
    for k in range(12): parts["3D/Objects/big%d.model" % k] = big
    put("many-big-parts.3mf", parts)
    # NaN / infinite coordinates
    put("nan-inf.3mf", {"3D/3dmodel.model": mdl('<object id="1" type="model"><mesh><vertices><vertex x="nan" y="0" z="0"/><vertex x="inf" y="0" z="0"/><vertex x="0" y="1e300" z="0"/></vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles></mesh></object>', '<item objectid="1"/>')})
    # a zip directory that claims 500 MB for a tiny entry
    put("ok-small.3mf", {"3D/3dmodel.model": mdl(strip(1, 12), '<item objectid="1"/>')})
    b = open(os.path.join(d, "ok-small.3mf"), "rb").read(); i = b.find(b"PK\x01\x02") + 24
    open(os.path.join(d, "lying-size.3mf"), "wb").write(b[:i] + struct.pack("<I", 500 << 20) + b[i + 4:])
    print(d)
