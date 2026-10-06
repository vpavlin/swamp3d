#!/bin/sh
# A stand-in for the OrcaSlicer CLI (tests): records its arguments, writes what a slice writes.
echo "$@" >> "$(dirname "$0")/fake-orca-args.txt"
out=""; name=""; prev=""
for a in "$@"; do
  case "$prev" in --outputdir) out="$a";; --export-3mf) name="$a";; esac
  prev="$a"
done
case " $* " in
  *" --export-stl "*) printf 'solid x\nendsolid x\n' > "$out/geom.stl"; exit 0;;
esac
mkdir -p "$out"
printf 'PK fake sliced project' > "$out/$name"
printf '; HEADER_BLOCK_START\n; model printing time: 1h 2m; total estimated time: 1h 5m 3s\n; total layer number: 42\n; filament used [mm] = 3333.3\n; filament used [cm3] = 8.02\n' > "$out/plate_1.gcode"
