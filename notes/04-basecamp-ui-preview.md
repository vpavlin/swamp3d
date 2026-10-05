# 04 - Basecamp UI: can we show a 3D model?

Checked 2026-10-05 against the Basecamp 0.2.0 AppImage (`~/basecamp-test/squashfs-root`).

## What the host bundles

- **No Qt Quick 3D and no Qt3D.** Neither `libQt6Quick3D*` nor a `QtQuick3D` QML import exists.
  A ui_qml view therefore can't use `View3D`/`Model` to render a mesh directly.
- **Qt WebEngine is bundled** (`libQt6WebEngineCore`, `libQt6WebEngineQuick`, `qml/QtWebEngine`), and so are
  QtWebChannel and QtWebView. A view *may* be able to host a `WebEngineView` running a small
  WebGL viewer (three.js-style STL/3MF loader), but this is unverified in a sandboxed ui_qml
  plugin. Likely questions: local file access, the view sandbox, GPU availability.
- **Qt Quick Shapes, Canvas, Labs WavefrontMesh** are available, enough for a 2D projected
  wireframe/flat-shaded preview drawn from triangles the core sends.
- Basecamp 0.3.x: the logosctl AppImage is the only local 0.3 artifact. The GUI bundle needs
  checking for the same libraries before committing to an approach.

## Options for the model preview (cheapest first)

| option | how | pros | cons |
|---|---|---|---|
| A. Thumbnails only | creator's app renders PNG thumbnails at publish time (core-side software rasterizer over the STL), stored as small blobs | works everywhere incl. phone; tiny; also what listings need | not interactive |
| B. Canvas turntable | core decimates the mesh to ~2-5k triangles and returns projected, shaded polygons for N angles; QML `Canvas` draws them | pure QML, no new deps, interactive-ish (drag rotates) | crude, CPU-bound in JS |
| C. WebEngineView + WebGL | view embeds a local HTML viewer; mesh is passed via WebChannel or a file URL | real 3D, smooth, familiar | unverified in the sandbox; heavy; must not fetch remote URLs (privacy) |
| D. Open in slicer | core writes the file to a temp/Downloads path; "Open in PrusaSlicer/OrcaSlicer/Bambu Studio" via the OS handler | what makers actually want | needs the slicer installed |

**Recommendation for the PoC:** A + D, with B as a stretch. C is a spike to run once in real
Basecamp (it decides whether a real 3D viewer is possible at all).

## Other UI notes

- Listings, search and detail pages are ordinary QML lists; images: the view sandbox blocks
  `data:` URIs (memory: basecamp-qr-core-unreachable), so thumbnails must be served as files the
  core writes, or drawn on Canvas. Needs a check: can a ui_qml `Image` load a `file://` path in
  the module's data dir?
- Android (charter: both platforms): RN can use expo-gl/three.js for a real 3D preview; the
  phone is a good *browsing* client (fetch-only Storage), less so for publishing big files.
