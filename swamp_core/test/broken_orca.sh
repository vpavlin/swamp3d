#!/bin/sh
# A stand-in for an OrcaSlicer build that can't start here (tests): the Ubuntu 22.04 AppImage on a
# current distribution, which has WebKitGTK 4.1 but not 4.0.
echo "/tmp/.mount_Orca/bin/orca-slicer: error while loading shared libraries: libwebkit2gtk-4.0.so.37: cannot open shared object file: No such file or directory" >&2
exit 127
