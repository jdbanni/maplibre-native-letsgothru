# Terrain basemap draping — GL JS vs Native, and a render-in-place redesign

Date: 2026-05-26. Author: letsgothru (Claude).

## Problem

In our fork (upstream PR #4190 "Terrain 3D", head `56c71df`), 3D terrain relief
renders, but the **vector basemap does not drape** onto it. The terrain shows
only the RTT background colour + screen-space labels; landcover/roads/water are
missing.

### Root cause (measured, not guessed)

The PR drapes by **relocating drawables**: each frame it moves the
`renderToTerrain` layers' translucent drawables out of their `TileLayerGroup`
and into new per-terrain-tile sub-groups attached to each `RenderTarget`
(`renderer_impl.cpp:283-327`), then renders those sub-groups into the offscreen
FBO.

Instrumentation showed:
- Drawables **are** moved into the RTT sub-groups (~470/frame; ~35 per FBO).
- The fill tweaker computes a **correct on-screen RTT matrix** for them
  (logged `m0≈0.5/8192, m12=0.5, m13=-0.5` → a valid FBO quadrant).
- Yet they **rasterise zero fragments**: clearing the FBO to magenta left the
  terrain uniformly magenta, and forcing the fill fragment shader to output
  opaque red produced no red (byte-identical output).

So the relocated drawables issue draw calls but their per-draw uniform binding
(the matrix the vertex stage actually reads) does not survive the move into a
foreign layer group / offscreen encoder. The PR author knew draping was
unfinished — the branch contains commits `"set the background color of RTT
tiles. Ideally we would just apply the [basemap]"` and `"HACK to get line
rendering to work on terrain"`. Upstream Terrain 3D is "partially funded, in
progress" with no completion date.

## How MapLibre GL JS does it (and why it works)

GL JS is **immediate-mode**: each frame it re-walks layers and tiles and issues
draw calls. Terrain draping (`src/webgl/render_to_texture.ts`,
`src/render/terrain.ts`):

- **Redirect, don't relocate.** It keeps the normal `painter.renderLayer()`
  loop in order and simply **binds a per-tile offscreen framebuffer** instead of
  the screen. The layer-draw code is unchanged; only the render target + matrix
  change.
- **Stacks.** Consecutive RTT-eligible layers are batched into `_stacks`
  (`string[][]`). A stack is flushed to each tile's texture when a non-draped
  layer (symbol/circle) or the end is reached.
- **Allowlist** `LAYERS_TO_TEXTURES = {background, fill, line, raster,
  hillshade, color-relief}`. Symbols/circles draw **on top** in screen space.
- **Cached, fingerprinted textures.** Per-tile FBOs are pooled
  (`tile.acquireRTT/getRTT/releaseRTT`); `_rttFingerprints` re-render a tile's
  drape **only when its source content changes** — not every frame.
- **Mesh** 128-grid reused for all tiles, **with skirts** to hide seams;
  `getDEMElevation` bilinear, `getUnpackVector` handles mapbox + terrarium.
- Recently abstracted behind `render_to_texture_interface.ts` for WebGPU.

## Architectural comparison

| Aspect | GL JS (works) | Native PR #4190 (draping broken) |
|---|---|---|
| Mode | Immediate (redraw each frame) | Retained (drawables persist; tweakers update UBOs) |
| Drape mechanism | Redirect output FBO; replay normal layer draw per tile | **Move** drawables into per-terrain-tile sub-groups |
| Per-tile texture | Pooled + fingerprint-cached (render on change) | Recreated per frame (no content cache) |
| Layer selection | `LAYERS_TO_TEXTURES` allowlist | `renderToTerrain` flag; only Translucent moved |
| Per-tile matrix | Recomputed in the draw loop (free) | `getTerrainRttPosMatrix` (correct) but applied via move |
| Failure mode | — | Moved drawables rasterise nothing (uniform binding lost) |
| Mesh skirts | Yes | No |
| Symbols | Screen-space, ground-clamped via `getElevation` | Screen-space, no clamp/occlusion |

### The crux

GL JS's "draw the same geometry into N targets with N matrices" is free because
it's immediate-mode. Native is retained-mode: a drawable holds **one**
matrix/UBO, so rendering it into screen + each terrain tile is awkward. The
PR's move is a workaround for that mismatch, and it's exactly where draping
breaks.

## Recommended redesign for Native: render-in-place

Mirror GL JS's principle within Native's retained model: **do not move
drawables.** Instead, for each terrain-tile FBO, render the draped layer groups
**in place** with a per-tile matrix, re-running their tweakers against that
tile's RTT matrix.

Mechanism:

1. `RenderTarget` knows its terrain `UnwrappedTileID` (set at creation in
   `TexturePool::createRenderTarget`).
2. A `std::optional<UnwrappedTileID> terrainTileID` on `PaintParameters` carries
   the "current RTT target tile" context.
3. `LayerTweaker::getTileMatrix`: when `parameters.terrainTileID` is set, return
   `getTerrainRttPosMatrix(sourceTile, *parameters.terrainTileID)`. Source tiles
   that don't overlap this terrain tile fall through to a **zero matrix**
   (`mat4` default) → degenerate → naturally culled. So every draped drawable
   can be rendered into every FBO; only the overlapping ones survive.
4. `RenderTarget::render`: set `terrainTileID`, then for each
   `shouldRenderToTerrain()` layer group: `runTweakers` (writes per-tile RTT
   matrices), then `render` (opaque, then translucent) into the FBO. Clear
   `terrainTileID` afterward.
5. Main screen passes: **skip** `shouldRenderToTerrain()` layer groups (the
   terrain mesh is what's drawn to screen; draped layers must not also render
   flat at z=0).
6. Delete the move loop (`renderer_impl.cpp:283-327`).

Because drawables are never relocated, they keep their normal uniform-binding
path — the path that already works on screen — which is expected to fix the
"rasterise nothing" failure.

### Follow-ups worth porting from GL JS

- **Fingerprint texture caching**: re-render a tile's drape only when its source
  content changes → fixes the flicker and most of the per-frame cost.
- **Mesh skirts** to remove inter-tile seams.
- **Symbol ground-clamp** via `getElevation()` sampling → labels sit on terrain
  and can be depth-tested/occluded.
- Use the **merged upstream Color-Relief + Hillshade** layers (PRs #3965,
  #4166) as draped layers rather than a custom terrain-shader hillshade.

### Caveat

This is not a line-for-line port (immediate vs retained mode). It is a focused
refactor of how draped layers reach the per-tile target, following a proven
design instead of finishing an abandoned hack.

## Prototype results (2026-05-26)

Implemented the render-in-place design above (uncommitted working tree):

- `PaintParameters::terrainTileID` (current RTT target tile).
- `RenderTarget` stores its terrain tile (`setTerrainTileID`, set in
  `TexturePool::createRenderTarget`).
- `RenderTarget::render`: when it has a terrain tile, binds global uniform
  buffers and renders the orchestrator's `shouldRenderToTerrain()` layer groups
  **in place** (opaque then translucent) with `terrainTileID` set.
- `LayerTweaker::getTileMatrix`: uses `parameters.terrainTileID` when set →
  `getTerrainRttPosMatrix(sourceTile, terrainTileID)`; non-overlapping source
  tiles get a zero matrix and cull.
- Move loop deleted; screen passes skip `shouldRenderToTerrain()` groups.

**What it validated:** the architecture is correctly wired. Diagnostics showed
each terrain FBO renders the right set in place — `drapedGroups=59,
drapedDrawables=114` per tile, with `terrainTileID` set and the per-tile RTT
matrix selected.

**What's still blocked:** fills/lines **still rasterise nothing** into the FBO —
identical to the move approach. A forced opaque-red fill fragment produced no
red in either approach. Since this is common to both move and render-in-place,
the blocker is **not** drawable relocation. It is the **per-draw uniform binding
in the offscreen pass**: the fill vertex shader reads `drawableVector[uboIndex]`
for its matrix, and in the terrain RTT pass it does not receive the matrix the
tweaker wrote (geometry lands off-screen → zero fragments). Depth, stencil,
cull, pipeline build, buffer upload, and the matrix value itself were all ruled
out.

### Next concrete step

Trace, in `src/mbgl/mtl/`, how `idFillDrawableUBO` (the `drawableVector`) and
`idGlobalUBOIndex` (`uboIndex`) are bound when a drawable is rendered inside an
offscreen `RenderTarget` render pass vs the main screen pass. Likely the
per-drawable UBO array isn't populated/bound for drawables drawn through the
offscreen encoder (or `uboIndex` is stale), so the vertex stage reads a wrong
matrix. This is the same wall the upstream PR hit (it punted with a
background-colour clear). Fixing it is the crux for *either* draping approach.

### Caveat on render-in-place specifically

Rendering every draped drawable into every terrain FBO and re-tweaking its
shared per-drawable UBO per target also risks a write-ordering race (retained
single-UBO vs immediate-mode). Once the offscreen-binding blocker is fixed,
either (a) give drawables per-target UBO sets, or (b) filter source tiles to
their overlapping terrain tile before rendering (GL JS's proxy mapping).

## Deep-dive findings (2026-05-26, session 2)

Two distinct bugs, isolated empirically with shader hacks (magenta FBO clear,
forced-red fragment, matrix-bypass vertex):

1. **Stencil culling — FOUND & FIXED.** The draped fill/line drawables were
   being stencil-culled in the offscreen FBO: their source-tile clip masks are
   projected in screen space and are meaningless in the per-tile FBO. The move
   approach disabled stencil per-drawable; render-in-place did not. Proof:
   matrix-bypass + forced-red rendered **nothing** with stencil on, and **solid
   red** once stencil was disabled on the draped drawables. Fix kept in
   `RenderTarget::render` (terrain branch): `setEnableStencil(false)` on the
   draped groups' drawables before rendering them into the FBO.

2. **Offscreen matrix delivery — STILL OPEN.** With stencil off:
   - matrix-bypass (map tile coords straight to NDC) + forced-red → **red**
     fills the terrain. So the offscreen draw pipeline (geometry + fragment)
     works once stencil is off.
   - the **real** `drawable.matrix` + forced-red → **nothing**, at z12 (dz=0,
     full-FBO mapping) *and* z14 (dz=2, cell mapping). So `drawable.matrix` as
     read by the fill vertex maps geometry off-screen in the offscreen pass.
   - The tweaker computes a correct on-screen RTT matrix (logged), buffers are
     reallocated on update (no in-place race, `buffer_resource.cpp:141`), and
     the *same* per-drawable UBO binding renders correctly on screen in 2D mode.
     So the vertex is reading a wrong matrix specifically in the offscreen pass.

   This is the remaining blocker and is common to both the move and
   render-in-place approaches — it is the wall the upstream PR hit.

### Next step (needs a GPU capture)

Static source reading has bottomed out. Capture a Metal frame (Xcode, via the
`macos-metal-xcode` preset) on the terrain RTT pass and inspect, for a fill
draw: the bound buffer at `idFillDrawableUBO`, the `idGlobalUBOIndex` value, and
the actual `FillDrawableUBO.matrix` the vertex receives vs what the tweaker
wrote. That will show whether it's a stale/zero buffer, a wrong `uboIndex`, or a
binding that the offscreen encoder drops.

Current working tree holds the render-in-place prototype + the stencil fix
(uncommitted); terrain relief still renders, basemap drape pending bug #2.

## GPU readback findings (2026-05-26, session 3)

Rather than a GUI capture, encoded the matrix the fill vertex reads (m0, m12,
m13) into the fragment colour, bypassed the position so fragments are on-screen,
and made the terrain shader output the raw RTT — so the saved PNG *is* the
matrix, decodable from pixel colour.

**Result: the matrix IS delivered to the vertex with valid values.** At z13
top-down the readback showed distinct per-terrain-tile colours with m0 ≈ 1/8192
(the dz=1 ortho scale) and varying translation — i.e. valid RTT matrices, not
zero and not screen-space. So bug #2 is **not** a delivery/binding failure and
**not** the UBO race.

That redirects bug #2 to the **RTT-cell ↔ mesh-UV coordinate mapping**: the
fills are placed into the FBO by `getTerrainRttPosMatrix`, but the terrain mesh
samples a different location, so it reads background. Removing the mesh's
`1.0 - uv.y` Y-flip alone did **not** fix it (no visual change), so it's not a
simple vertical mirror — the cell offset/scale vs the mesh sample coordinate is
off in a way that needs pinning down precisely.

### Next step

A surgical readback: in the fill fragment, output the source tile's *intended*
normalised FBO position (derived from its matrix), and separately have the
terrain mesh output its *sample* coordinate — render both and compare where a
known feature lands vs where the mesh looks for it. That pins the exact
transform discrepancy (offset, scale, or axis) between `getTerrainRttPosMatrix`
(layer_tweaker.cpp) and the mesh UV in `render_terrain.cpp`/`terrain.hpp`.
Alternatively, a Metal frame capture shows it directly.

Net: stencil culling fixed; matrix delivery proven correct; remaining work is a
contained coordinate-transform fix between the RTT placement and mesh sampling.
