# Symbols/labels on 3D terrain — GL JS baseline and Native plan

Date 2026-05-26. Goal: labels/symbols sit on the terrain surface and are
occluded when they're behind a hill (they currently float in screen space at
sea level, never occluded).

## GL JS baseline (how it works)

From `src/render/terrain.ts` and `src/symbol/projection.ts`:

1. **Elevation lookup.** `Terrain.getElevation(tileID, x, y, extent)` =
   `getDEMElevation(...) * exaggeration`, where `getDEMElevation` does **bilinear
   interpolation** over the 4 surrounding DEM samples (`dem.get(cx,cy)…`),
   normalising across tile boundaries.

2. **Elevation applied at projection.** A `getElevation` callback is threaded
   through `SymbolProjectionContext`. When projecting a symbol anchor,
   `projectWithMatrix` builds the homogeneous point `[x, y, getElevation(x,y), 1]`
   — i.e. the terrain height becomes the anchor's **z** before the matrix
   transform. So the label rides on the surface; nothing else in the symbol
   pipeline changes.

3. **Occlusion via a depth texture.** The terrain is rendered to a depth
   framebuffer (`_fboDepthTexture`); `depthAtPoint(screenPixel)` reads the
   terrain depth, and a symbol whose projected depth is *behind* the terrain
   depth at its screen position is hidden (`PointProjection.isOccluded`,
   `pathSlicedToLongestUnoccluded`).

Key ordering consequence: **occlusion depends on elevation.** A symbol left at
z=0 projects to the near plane and always passes the depth test, so it must
first be raised to its ground elevation.

## Native plan (mirror the baseline in the retained pipeline)

**Stage 1 — `RenderTerrain::getElevation` (foundation).** Implement the DEM
lookup (currently a stub returning 0): find the loaded DEM tile covering the
query coord, get its `DEMData`, bilinear-sample `DEMData::get(x,y)`, multiply by
exaggeration. Mirrors GL JS `getDEMElevation`. Self-contained.

**Stage 2 — clamp symbols to the surface.** Raise each symbol anchor by its
terrain elevation so labels sit on the terrain. Two options:
- GPU: bind the DEM elevation to the symbol vertex shader, sample at the
  anchor's tile position, add to the anchor z (matches the terrain mesh, which
  already samples the DEM). Most consistent with our render path.
- CPU: bake elevation into the per-symbol position in the symbol tweaker.
GPU is preferred (per-frame correct as DEM loads), but touches the symbol
shaders (icon/text/SDF variants).

**Stage 3 — occlusion.** Have the terrain mesh write depth in the main pass, and
depth-test the (now elevation-clamped) symbols against it so labels behind hills
are hidden. Mirrors GL JS's depth-texture test, but using the shared depth
buffer in-pass rather than a separate readback.

Risk: the symbol pipeline is the most complex in MapLibre (shaping, SDF,
collision). Changes are staged and verified visually at each step.
