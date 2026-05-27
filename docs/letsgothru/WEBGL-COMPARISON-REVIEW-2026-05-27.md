# Terrain: our approach vs MapLibre GL JS — review & realignment plan

2026-05-27. Trigger: GL JS renders the same Snowdon data with (a) pin-sharp drape
at every zoom and (b) labels placed perfectly on terrain. Ours is fuzzy and the
labels drift. This documents *why* and the plan to adopt the GL JS approach.

## Issue 1 — fuzzy drape (the big one)

**Ours:** `Renderer::Impl::render` reconciles the RTT pool against
`demSource->getRawRenderTiles()` (renderer_impl.cpp:246) — one 512² render target
**per DEM tile**, and `RenderTerrain` builds one terrain-mesh drawable per DEM tile
that samples that RTT. The DEM source maxzoom is **12**. So:

- At a z14 view the terrain tiles are z12 (capped). Each 512² RTT covers a z12 tile
  = sixteen z14 basemap tiles ⇒ ~128 px of basemap per tile ⇒ heavy blur.
- Raising the RTT size (we tried 1024²) just shifts the wall and cratered fps; it
  can't fix an architecture that ties drape resolution to the DEM tile grid.

**GL JS:** the drape is decoupled from the DEM. It renders to texture per **proxy
tile at the render zoom** (z14), each 512², then draws the terrain as proxy tiles
that sample their own full-res drape; elevation comes from whatever DEM tile covers
them (often a lower-zoom parent), addressed with a tile-to-DEM uv transform. Drape
resolution therefore tracks the *content* zoom, not the DEM ⇒ always sharp.

**Fix:** render terrain per render-zoom (basemap) tile, not per DEM tile:
- Key the RTT pool + terrain-mesh drawables by the basemap/proxy tiles at render zoom.
- Each tile gets its own sharp 512² drape (native basemap resolution).
- Each tile samples elevation from its covering DEM tile via a uv transform — we
  already have exactly this: `RenderTerrain::getDEMTextureFor` returns
  `{texture, dem_tl, dem_scale}` (built for the symbol work). The terrain vertex
  shader would sample the DEM at `dem_tl + (pos/EXTENT)*dem_scale` instead of
  assuming 1:1 `pos/EXTENT`.

This is the core rework. It also naturally fixes the tile-boundary drape seam
(tiles align at the render grid) and makes the per-tile counts sane for the RTT
cache (so the dirty-skip can be revisited).

## Issue 2 — label placement / drift

**Ours:** labels render with the normal (flat) symbol projection, then a vertex
shader adds a screen-space shift = the projected parallax of the *single anchor*
raised to terrain height (symbol.hpp). Two problems: the shift is computed in the
tile-matrix space but applied to label-plane/coord-matrix space, and for line
labels the glyphs sit along the line away from that one anchor ⇒ they slide faster
than the map when panning.

**GL JS:** elevation enters at **placement/projection time** on the CPU:
`getElevation(anchor)` is folded into the anchor's world position before it is
projected, for placement, collision, and line-label reprojection. The glyphs are
then laid out around the already-elevated, correctly-projected anchor ⇒ perfect
placement, correct tracking, and depth occlusion works.

**Fix:** plumb terrain elevation into the symbol projection (`symbol_projection`,
placement, line-label reprojection) so the projected anchor includes elevation —
remove the screen-space shader hack. Shared GL+Metal code; intricate; the larger of
the two changes. (Cheaper interim: per-glyph shader sample instead of per-anchor —
helps line labels, still approximate.)

## Recommended order
1. **Proxy-tile draping** (Issue 1) — biggest visual win, fixes blur + the drape
   seam, and reuses `getDEMTextureFor`. Sizeable but self-contained to the terrain
   render path.
2. **Placement-time label elevation** (Issue 2) — once the drape is right, do
   labels the GL JS way; retire the shader hack.

Both are real reworks, not patches. Item 1 is where the visible quality jump is.
