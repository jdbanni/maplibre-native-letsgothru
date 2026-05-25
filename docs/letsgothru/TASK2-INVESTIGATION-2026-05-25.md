# Task 2 Investigation — visible terrain — 2026-05-25

Phase 2 success criteria for Task 2: "render Snowdonia at pitch=60° and
visually see the terrain elevated".

Outcome this run: **no crash, but no visible elevation either**. The
fixes I made set up the math correctly; the remaining issue is that the
basemap fills don't visibly populate the RTT FBOs that the terrain mesh
samples.

Saved at `docs/letsgothru/RENDERED-task2-2026-05-25.png` — Snowdonia at
z=12 pitch=60°. Symbol/label layers visible; basemap fills missing;
terrain mesh present but draped with a near-empty texture.

## What I looked at

### 1. The PR's terrain rendering pipeline (in this fork)

- `src/mbgl/renderer/render_terrain.cpp` builds a 64×64 grid mesh per
  DEM tile in tile-space coordinates (0..EXTENT=8192). For each
  DEM tile loaded by the `raster-dem` source, it creates a `Drawable`
  bound to (a) the DEM texture for that tile and (b) the RTT FBO
  texture that the texture pool allocated for that same tile id
  (`texturePool.getRenderTarget(renderTile.id)->getTexture()`).
  Layer index `TERRAIN_LAYER_INDEX = -1000` — drawn first in
  `visitLayerGroups` iteration.
- `src/mbgl/renderer/renderer_impl.cpp:282-319` is the secret-sauce
  bit. *Before* the upload pass, for every layer group flagged
  `renderToTerrain==true` (fill, line, fill-extrusion all flag true
  in their `createTileLayerGroup` calls), it walks the group's
  drawables and *moves* each into a *new* per-terrain-tile sub-group
  attached to the matching `RenderTarget`. The parent layer group
  ends up empty. The sub-groups end up holding the original
  drawables, organised by which terrain-tile they map to.
- `RenderTarget::render` (`src/mbgl/renderer/render_target.cpp`)
  creates an offscreen render pass into its `offscreenTexture`,
  runs its tweakers, then renders all the sub-layer-groups
  (opaque then translucent). This is `drawableTargetsPass()` in the
  outer renderer.
- Order in `renderer_impl.cpp::Renderer::render`:
  `common3DPass(); drawable3DPass();` → `drawableTargetsPass();` →
  `commonClearPass(); drawableOpaquePass(); drawableTranslucentPass();`
- The terrain layer group has `renderToTerrain=false` (it's a
  *consumer* of RTT FBOs, not a contributor), is in the regular
  layer group set, and its drawables use
  `RenderPass::Translucent`. So the terrain mesh draws in
  `drawableTranslucentPass()`, after RTT FBOs are populated.

### 2. The math (elevation scale + DEM decode)

Both wrong in the PR for our use case; both fixed in this run.

**Exaggeration in tile-units (not metres).** The vertex shader does
`drawable.matrix * float4(pos.x, pos.y, elevation, 1.0)` where
`drawable.matrix == matrixForTile(tileID)` — i.e. the standard
projection matrix that expects tile-space (0..8192) coordinates with
Z=0. Pumping raw metres into Z produces ~0.1 px of displacement
at z=12 even for 1000 m peaks. To use the same matrix correctly we
multiply elevation_metres by
`metersToTileUnits = 2^zoom * EXTENT / (cos(lat) * earthCircumference)`,
which at z=12, lat=53°, gives ~1.4 — i.e. 1000 m → ~1400 tile units of
displacement on an 8192-unit mesh. Done in
`src/mbgl/renderer/layers/terrain_layer_tweaker.cpp`.

**Terrarium decode.** Our R2 DEM uses Mapzen Terrarium encoding
(`height = (R*256 + G + B/256) - 32768`). The PR's shader hard-codes
Mapbox terrain-rgb (`-10000 + ((R*256² + G*256 + B) * 0.1)`). Updated
the MSL source in `include/mbgl/shaders/mtl/terrain.hpp`. The CPU
side (`DEMData::getUnpackVector`) already handles both encodings via
the source spec's `encoding` field; only the GPU shader was wrong.
Cleaner long-term: pass the unpack vector as a UBO. Today's fix
hard-codes terrarium because every DEM in our pipeline is terrarium.

### 3. What's still broken

After both fixes, the terrain still renders flat. The basemap fills
are also not visible on the final image, only symbol layers are.

The likely cause is the **RTT FBO is being drawn into with
near-zero-alpha output**, so the terrain fragment shader's
`if (mapColor.a > 0.01) return mapColor;` short-circuit returns
either the FBO's clear colour (background `#f4efe5`) or transparent
black. Either way no fill geometry shows up.

The terrain logs confirm:
- The terrain layer group's drawables are uploaded (`Drawable::upload
  for terrain-tile: texturesNeedUpload=1, textureCount=3`).
- Per terrain tile, both the DEM (texture 0) and the RTT (texture 1)
  textures are bound and `Metal texture is VALID`.
- `LayerGroup::render for terrain drew 9 drawables` — the mesh is
  drawn 9 times (covering Snowdonia at z=12).

But: I never observed any "rendered N fill drawables" or
"RenderTarget rendered N sub-groups" log because there is no such
log today. Need to add one.

### 4. Suspected sub-group issues

Top suspects, ordered by what I'd try first:

**(a) Sub-group has no LayerTweaker for the right layer.** In
`renderer_impl.cpp:312`,
```
if (!layerGroupPrexists) {
    singleTileLayerGroup->addLayerTweaker(drawables[0]->getLayerTweaker());
}
```
This grabs the tweaker off the *first drawable* added to the new
sub-group. If a layer's drawables are processed in any non-trivial
order (e.g. sorted by tile id, not by layer), the *first* drawable
might be from a different layer than the rest. Tweakers run during
`runTweakers`; if a fill drawable in the sub-group is associated
with a tweaker that's actually a line-layer tweaker, the wrong UBOs
get written.

Also: the same tweaker is referenced by both the parent layer group
(now empty) and every sub-group it spawned. When `runTweakers` runs
on the parent (in the main `visitLayerGroups` loop), the tweaker's
`visitLayerGroupDrawables` finds no drawables and does nothing
useful. Then when `runTweakers` runs on the sub-group (inside
`RenderTarget::render`), the *same* tweaker visits the *sub-group's*
drawables and writes the RTT-matrix UBO to each. That part should
be correct. But the parent's runTweakers is wasted work.

**(b) RenderTarget has no depth/stencil attachment.**
`RenderTarget::render` creates its render pass with `clearDepth={}` and
`clearStencil={}` — no depth or stencil buffer at all. Fill drawables
set `setEnableDepth(true)` in their factory (the depth test is required
to keep filled polygons from over-drawing each other badly). When
depth is enabled but the render pass has no depth buffer, Metal
behaviour depends on the pipeline state's depth attachment format.
If it's set to a depth format but the FBO has none, validation can
silently disable that drawable.

We could (i) attach a depth buffer to the RenderTarget's
`offscreenTexture`, or (ii) force `setEnableDepth(false)` on drawables
moved into RTT sub-groups, parallel to the stencil-disable we already
do.

**(c) The drawable's pipeline state may have been compiled for the
*main backbuffer*'s pixel format and now binds against an RTT
`MTLPixelFormatBGRA8Unorm` / different format.** The PR's
`Drawable::draw` recompiles pipeline state lazily; this should
self-heal. But if it doesn't, the fix is to invalidate
`impl->pipelineState` on `setRenderTarget` (no such API exists today).

**(d) The terrain mesh's drawable.matrix shares the same name as the
fill drawables' matrix UBO field, but they're separate per-drawable
UBOs. So matrix collisions aren't possible. Ruled out.**

### 5. The fragment-shader short-circuit

```msl
float4 mapColor = mapTexture.sample(mapSampler, float2(in.uv.x, 1.0 - in.uv.y));
if (mapColor.a > 0.01) {
    return half4(mapColor);
}
// Fallback: elevation-based color gradient
```

If we want a quick "is the elevation displacement *actually* working at
all?" test, comment out the `if` branch and force the fallback. Should
make the mesh show the blue→green→brown gradient regardless of what the
FBO contains, with whatever shape the elevation displacement produced.
Did not do this in this run (time budget). Worth doing first thing in
Phase 3.

### 6. What I would do next in Phase 3

1. **Attach a depth buffer to `RenderTarget`** (suspect (b) above).
   This is the single highest-yield fix.
2. **Force the elevation-fallback path** in the fragment shader to
   visualise terrain mesh geometry independent of RTT compositing.
3. **Add a log** at `RenderTarget::render` that prints the FBO id +
   number of sub-groups + total drawable count rendered into it. This
   gives concrete evidence the basemap fills made it into the FBO.
4. **Dump intermediate framebuffers** via Xcode Metal capture
   (cmake preset `macos-metal-xcode`, run from Xcode with frame
   capture on). The RTT pass and the final composite would show
   visually whether the FBO has content.
5. **Per-source-tile clip mask** for the RTT sub-group is the proper
   long-term fix to step away from `setEnableStencil(false)` —
   useful when zooming in where one terrain tile contains multiple
   source-data tiles.
6. **Encoding UBO** so the terrarium-vs-mapbox decode is data-driven,
   not hard-coded — once we want to support both pipelines.

Each is independent; (1)–(2) together should produce visible terrain.
Estimated 1–2 focused engineer-days.
