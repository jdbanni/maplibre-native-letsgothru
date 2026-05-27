#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/renderer/update_parameters.hpp>
#include <mbgl/renderer/render_source.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/render_pass.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_static_data.hpp>
#include <mbgl/renderer/render_orchestrator.hpp>
#include <mbgl/renderer/render_target.hpp>
#include <mbgl/renderer/change_request.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>
#include <mbgl/renderer/buckets/hillshade_bucket.hpp>
#include <mbgl/geometry/dem_data.hpp>
#include <mbgl/tile/raster_dem_tile.hpp>
#include <mbgl/tile/tile.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/drawable_impl.hpp>
#include <mbgl/gfx/drawable_builder.hpp>
#include <mbgl/gfx/shader_registry.hpp>
#include <mbgl/gfx/color_mode.hpp>
#include <mbgl/gfx/texture2d.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/shaders/segment.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/mat4.hpp>

#include <cmath>
#include <cstring>
#include <unordered_set>

namespace mbgl {

RenderTerrain::RenderTerrain(Immutable<style::Terrain::Impl> impl_)
    : impl(std::move(impl_)) {}

RenderTerrain::~RenderTerrain() = default;

void RenderTerrain::update(const UpdateParameters& /*parameters*/) {
    // Find the DEM source if we haven't already
    if (!demSource && !impl->sourceID.empty()) {
        // In a full implementation, we would look up the source from parameters.sources
        // and cache the RenderSource pointer
        // For now, this is a placeholder
    }
}

void RenderTerrain::update(RenderOrchestrator& orchestrator,
                           gfx::ShaderRegistry& shaders,
                           gfx::Context& context,
                           const TexturePool& texturePool,
                           const TransformState& /*state*/,
                           const std::shared_ptr<UpdateParameters>& /*updateParameters*/,
                           const RenderTree& /*renderTree*/,
                           UniqueChangeRequestVec& changes) {
    // Find the DEM source if we haven't already
    if (!demSource && !impl->sourceID.empty()) {
        demSource = orchestrator.getRenderSource(impl->sourceID);
        if (demSource) {
            Log::Info(Event::Render, "Terrain found DEM source: " + impl->sourceID);
        } else {
            Log::Warning(Event::Render, "Terrain could not find DEM source: " + impl->sourceID);
        }
    }

    // letsgothru/terrain-3d: incremental update for continuous mode.
    // The previous code force-rebuilt the entire terrain layer group every
    // frame (reset layerGroup + clear tilesWithDrawables), which recreated
    // every DEM texture and drawable per frame. In continuous mode that leaked
    // GPU memory and drove frame time up without bound (~656 ms/frame after a
    // few seconds; ~1.7 GB of DEM uploads in 16 frames). We now keep drawables
    // across frames, build them only for newly-visible tiles, evict tiles that
    // scroll off, and re-point the per-frame RTT texture (slot 1) below.

    // Create layer group if we don't have one
    if (!layerGroup) {
        if (auto layerGroup_ = context.createLayerGroup(TERRAIN_LAYER_INDEX, /*initialCapacity=*/1, "terrain", false)) {
            layerGroup = std::move(layerGroup_);
            activateLayerGroup(true, changes);
            Log::Info(Event::Render, "Created terrain layer group");
        } else {
            Log::Error(Event::Render, "Failed to create terrain layer group");
            return;
        }
    }

    // Create tweaker if we don't have one
    if (!tweaker) {
        tweaker = std::make_unique<TerrainLayerTweaker>(this);
        Log::Info(Event::Render, "Created terrain layer tweaker");
    }

    // If we don't have a DEM source, we can't create terrain drawables
    if (!demSource) {
        return;
    }

    // Get tiles from the DEM source. Immutable<> is never null; an empty list
    // is fine — eviction below will then clear any stale drawables.
    auto renderTiles = demSource->getRawRenderTiles();

    // Cast to LayerGroup for addDrawable
    auto* lg = static_cast<LayerGroup*>(layerGroup.get());
    if (!lg) {
        return;
    }

    // Build the set of DEM tiles currently visible this frame.
    std::unordered_set<OverscaledTileID> presentTiles;
    presentTiles.reserve(renderTiles->size());
    for (const auto& renderTile : *renderTiles) {
        presentTiles.insert(renderTile.getOverscaledTileID());
    }

    // Evict drawables whose DEM tile is no longer visible. This bounds GPU
    // memory as the user pans/zooms in continuous mode; destroying the
    // UniqueDrawable releases its DEM texture and geometry buffers.
    const size_t evicted = lg->removeDrawablesIf([&](gfx::Drawable& drawable) {
        const auto& tid = drawable.getTileID();
        if (tid && presentTiles.count(*tid) == 0) {
            tilesWithDrawables.erase(*tid);
            demTextures.erase(*tid); // letsgothru/terrain-3d
            return true;
        }
        return false;
    });
    if (evicted > 0) {
        Log::Info(Event::Render, "Terrain evicted " + std::to_string(evicted) + " drawables for off-screen tiles");
    }

    // For each visible tile: build a drawable on first sight; otherwise just
    // re-point its map texture (slot 1) at this frame's render-to-texture FBO,
    // which the TexturePool recreates every frame.
    size_t newDrawables = 0;
    for (const auto& renderTile : *renderTiles) {
        const auto& tileID = renderTile.getOverscaledTileID();

        // This frame's RTT FBO texture for the tile (recreated per frame).
        std::shared_ptr<gfx::Texture2D> mapTexture;
        if (const auto& renderTarget = texturePool.getRenderTarget(renderTile.id)) {
            mapTexture = renderTarget->getTexture();
        }

        // Existing drawable: rebind the per-frame RTT texture and move on.
        if (auto it = tilesWithDrawables.find(tileID); it != tilesWithDrawables.end()) {
            if (mapTexture && it->second) {
                it->second->setTexture(mapTexture, 1);
            }
            continue;
        }

        // New tile: build a drawable once its DEM data is available.
        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) {
            continue;
        }

        auto* demTile = const_cast<RasterDEMTile*>(static_cast<const RasterDEMTile*>(&tile));
        if (!demTile) {
            continue;
        }

        auto* hillshadeBucket = demTile->getBucket();
        if (!hillshadeBucket) {
            continue; // still loading
        }

        const auto& demData = hillshadeBucket->getDEMData();
        auto imagePtr = demData.getImagePtr();
        if (!imagePtr || imagePtr->size.isEmpty()) {
            continue;
        }

        auto demTexture = createDEMTexture(context, demData);
        if (!demTexture) {
            Log::Warning(Event::Render, "Failed to create DEM texture for tile " + util::toString(tileID));
            continue;
        }

        auto drawable = createDrawableForTile(context, shaders, tileID, demTexture, mapTexture);
        if (drawable) {
            gfx::Drawable* raw = drawable.get();
            lg->addDrawable(std::move(drawable));
            tilesWithDrawables[tileID] = raw;
            demTextures[tileID] = demTexture; // letsgothru/terrain-3d: keep for symbol elevation
            newDrawables++;
        }
    }

    if (newDrawables > 0) {
        Log::Info(Event::Render,
                  "Terrain created " + std::to_string(newDrawables) +
                      " new drawables (total: " + std::to_string(tilesWithDrawables.size()) + ")");
    }
}

float RenderTerrain::getElevation(const UnwrappedTileID& tileID, float x, float y) const {
    // letsgothru/terrain-3d: DEM elevation lookup with bilinear interpolation,
    // mirroring GL JS Terrain.getDEMElevation. Returns raw metres (exaggeration
    // is applied by getElevationWithExaggeration). x,y are tile units [0,EXTENT].
    if (!demSource) {
        return 0.0f;
    }
    auto renderTiles = demSource->getRawRenderTiles();
    const int qz = tileID.canonical.z;
    // Query point in global tile-space at the query zoom.
    const double gx = static_cast<double>(tileID.canonical.x) + x / util::EXTENT;
    const double gy = static_cast<double>(tileID.canonical.y) + y / util::EXTENT;

    for (const auto& renderTile : *renderTiles) {
        const auto& demCanonical = renderTile.getOverscaledTileID().canonical;
        const int dz = demCanonical.z;
        const double scale = std::pow(2.0, dz - qz);
        const double dgx = gx * scale; // query position in DEM-zoom tile units
        const double dgy = gy * scale;
        if (static_cast<int>(std::floor(dgx)) != static_cast<int>(demCanonical.x) ||
            static_cast<int>(std::floor(dgy)) != static_cast<int>(demCanonical.y)) {
            continue; // this DEM tile doesn't cover the point
        }
        const auto& tile = renderTile.getTile();
        if (tile.kind != Tile::Kind::RasterDEM) {
            continue;
        }
        const auto* demTile = static_cast<const RasterDEMTile*>(&tile);
        const auto* bucket = demTile->getBucket();
        if (!bucket) {
            continue;
        }
        const auto& dem = bucket->getDEMData();
        const int dim = dem.dim;
        // Fractional position within the DEM tile -> DEM pixel (sample centres).
        const double fx = dgx - std::floor(dgx);
        const double fy = dgy - std::floor(dgy);
        const double px = fx * dim - 0.5;
        const double py = fy * dim - 0.5;
        const int x0 = static_cast<int>(std::floor(px));
        const int y0 = static_cast<int>(std::floor(py));
        const double tx = px - x0;
        const double ty = py - y0;
        const auto clampIdx = [dim](int v) { return v < -1 ? -1 : (v > dim ? dim : v); };
        const auto s = [&](int sx, int sy) { return static_cast<double>(dem.get(clampIdx(sx), clampIdx(sy))); };
        const double e = s(x0, y0) * (1 - tx) * (1 - ty) + s(x0 + 1, y0) * tx * (1 - ty) +
                         s(x0, y0 + 1) * (1 - tx) * ty + s(x0 + 1, y0 + 1) * tx * ty;
        return static_cast<float>(e);
    }
    return 0.0f;
}

float RenderTerrain::getElevationWithExaggeration(const UnwrappedTileID& tileID, float x, float y) const {
    return getElevation(tileID, x, y) * getExaggeration();
}

std::optional<RenderTerrain::DEMTextureRef> RenderTerrain::getDEMTextureFor(const UnwrappedTileID& tileID) const {
    // letsgothru/terrain-3d: find the cached DEM texture whose tile covers this
    // (symbol) tile at the same or a lower zoom, so one texture covers the whole
    // tile. Prefer the highest-zoom (most detailed) candidate. Returns the uv
    // transform mapping the symbol tile's [0,EXTENT] coords into [0,1] DEM uv.
    const int sz = tileID.canonical.z;
    const double sx = static_cast<double>(tileID.canonical.x);
    const double sy = static_cast<double>(tileID.canonical.y);

    std::optional<DEMTextureRef> best;
    int bestZ = -1;
    for (const auto& [demTid, texture] : demTextures) {
        if (!texture) {
            continue;
        }
        const int dz = demTid.canonical.z;
        if (dz > sz || dz <= bestZ) {
            continue; // higher zoom can't cover with one tile; lower than best is worse
        }
        const double scale = std::exp2(static_cast<double>(dz - sz)); // <= 1
        if (static_cast<int64_t>(std::floor(sx * scale)) != static_cast<int64_t>(demTid.canonical.x) ||
            static_cast<int64_t>(std::floor(sy * scale)) != static_cast<int64_t>(demTid.canonical.y)) {
            continue; // this DEM tile doesn't cover the query tile
        }
        DEMTextureRef ref;
        ref.texture = texture;
        ref.demScale = static_cast<float>(scale);
        ref.demTlX = static_cast<float>(sx * scale - static_cast<double>(demTid.canonical.x));
        ref.demTlY = static_cast<float>(sy * scale - static_cast<double>(demTid.canonical.y));
        best = ref;
        bestZ = dz;
    }
    return best;
}

float RenderTerrain::getExaggeration() const {
    return impl->exaggeration;
}

const std::string& RenderTerrain::getSourceID() const {
    return impl->sourceID;
}

bool RenderTerrain::isEnabled() const {
    return !impl->sourceID.empty();
}

const RenderTerrain::TerrainMesh& RenderTerrain::getMesh(gfx::Context& context) {
    if (!mesh) {
        generateMesh(context);
    }
    return *mesh;
}

void RenderTerrain::generateMesh(gfx::Context& /*context*/) {
    // Generate a regular grid mesh for terrain
    // This mesh will be reused for all tiles and displaced by DEM data in shaders

    const size_t gridSize = MESH_SIZE;
    const size_t verticesPerSide = gridSize + 1;
    const size_t totalVertices = verticesPerSide * verticesPerSide;

    // Vertex data: Each vertex has pos (x,y) and texture_pos (u,v)
    // Store as int16_t (short) for Metal short2 attribute format
    // Format: [pos.x, pos.y, tex.u, tex.v, pos.x, pos.y, tex.u, tex.v, ...]
    std::vector<int16_t> vertices;
    vertices.reserve(totalVertices * 4); // 4 shorts per vertex (x, y, u, v)

    const float posStep = static_cast<float>(util::EXTENT) / static_cast<float>(gridSize);

    // Main grid. texture_pos is repurposed as a skirt flag (x: 0 = surface vertex).
    for (size_t y = 0; y < verticesPerSide; ++y) {
        for (size_t x = 0; x < verticesPerSide; ++x) {
            vertices.push_back(static_cast<int16_t>(x * posStep)); // pos.x (tile space 0-8192)
            vertices.push_back(static_cast<int16_t>(y * posStep)); // pos.y
            vertices.push_back(0);                                 // skirt flag (0 = surface)
            vertices.push_back(0);
        }
    }

    // Index data: generate triangles for the grid
    std::vector<uint16_t> indices;
    indices.reserve(gridSize * gridSize * 6 + gridSize * 4 * 6);

    for (size_t y = 0; y < gridSize; ++y) {
        for (size_t x = 0; x < gridSize; ++x) {
            uint16_t topLeft = static_cast<uint16_t>(y * verticesPerSide + x);
            uint16_t topRight = topLeft + 1;
            uint16_t bottomLeft = static_cast<uint16_t>((y + 1) * verticesPerSide + x);
            uint16_t bottomRight = bottomLeft + 1;

            indices.push_back(topLeft);
            indices.push_back(bottomLeft);
            indices.push_back(topRight);

            indices.push_back(topRight);
            indices.push_back(bottomLeft);
            indices.push_back(bottomRight);
        }
    }

    // letsgothru/terrain-3d: skirts. Adjacent tiles' edges sample separate DEM
    // textures whose shared-edge heights differ by ~a DEM pixel, leaving hairline
    // cracks (you can see through to the background). Hang a downward apron from
    // each tile's perimeter: duplicate the border vertices, flag them as skirt
    // (texture_pos.x = 1) so the shader drops them below the surface, and stitch
    // a vertical strip between the edge and the apron to fill the gaps.
    const auto gridIdx = [&](size_t x, size_t y) {
        return static_cast<uint16_t>(y * verticesPerSide + x);
    };
    const auto addSkirtStrip = [&](const std::vector<std::pair<size_t, size_t>>& border) {
        const auto firstSkirt = static_cast<uint16_t>(vertices.size() / 4);
        for (const auto& [gx, gy] : border) {
            vertices.push_back(static_cast<int16_t>(gx * posStep));
            vertices.push_back(static_cast<int16_t>(gy * posStep));
            vertices.push_back(1); // skirt flag
            vertices.push_back(0);
        }
        for (size_t i = 0; i + 1 < border.size(); ++i) {
            const uint16_t e0 = gridIdx(border[i].first, border[i].second);
            const uint16_t e1 = gridIdx(border[i + 1].first, border[i + 1].second);
            const auto s0 = static_cast<uint16_t>(firstSkirt + i);
            const auto s1 = static_cast<uint16_t>(firstSkirt + i + 1);
            // Two triangles per segment (both windings emitted so the apron shows
            // regardless of cull state).
            indices.push_back(e0);
            indices.push_back(e1);
            indices.push_back(s0);
            indices.push_back(s1);
            indices.push_back(s0);
            indices.push_back(e1);
        }
    };
    std::vector<std::pair<size_t, size_t>> top, bottom, left, right;
    for (size_t x = 0; x < verticesPerSide; ++x) {
        top.emplace_back(x, 0);
        bottom.emplace_back(x, gridSize);
    }
    for (size_t y = 0; y < verticesPerSide; ++y) {
        left.emplace_back(0, y);
        right.emplace_back(gridSize, y);
    }
    addSkirtStrip(top);
    addSkirtStrip(bottom);
    addSkirtStrip(left);
    addSkirtStrip(right);

    // Store mesh data with raw vertices and indices
    mesh = TerrainMesh{nullptr,             // vertexBuffer - will be created when creating drawable
                       nullptr,             // indexBuffer - will be created when creating drawable
                       vertices.size() / 4, // 4 shorts per vertex (x, y, u, v)
                       indices.size(),
                       std::move(vertices),
                       std::move(indices)};

    Log::Info(Event::General,
              "Terrain mesh generated: " + std::to_string(mesh->vertexCount) + " vertices, " +
                  std::to_string(mesh->indexCount) + " indices");
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::createDEMTexture(gfx::Context& context, const DEMData& demData) {
    // Get the DEM image data
    auto imagePtr = demData.getImagePtr();
    if (!imagePtr || imagePtr->size.isEmpty()) {
        Log::Warning(Event::Render, "DEM data has no image");
        return nullptr;
    }

    Log::Info(Event::Render,
              "Creating DEM texture: size=" + std::to_string(imagePtr->size.width) + "x" +
                  std::to_string(imagePtr->size.height) + ", bytes=" + std::to_string(imagePtr->bytes()));

    // DEBUG: Check actual pixel values in the image
    if (imagePtr->data && imagePtr->bytes() > 0) {
        const uint8_t* pixels = imagePtr->data.get();
        // Check first 10 pixels' RGB values
        std::string pixelValues = "First 10 DEM pixels (RGBA): ";
        for (size_t i = 0; i < std::min(size_t(10), imagePtr->bytes() / 4); i++) {
            size_t offset = i * 4;
            pixelValues += "[" + std::to_string(pixels[offset]) + "," + std::to_string(pixels[offset + 1]) + "," +
                           std::to_string(pixels[offset + 2]) + "," + std::to_string(pixels[offset + 3]) + "] ";
        }
        Log::Info(Event::Render, pixelValues);
    }

    // Create a new texture
    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create DEM texture");
        return nullptr;
    }

    // Set the image data
    texture->setImage(imagePtr);
    Log::Info(Event::Render, "DEM texture image data set successfully");

    // Configure sampler - use linear filtering for smooth elevation interpolation
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                      .wrapU = gfx::TextureWrapType::Clamp,
                                      .wrapV = gfx::TextureWrapType::Clamp});

    Log::Info(Event::Render, "DEM texture created and configured successfully");
    return texture;
}

std::shared_ptr<gfx::Texture2D> RenderTerrain::createTestMapTexture(gfx::Context& context) {
    // Create a simple test texture with a checkerboard pattern
    // This will be replaced with actual render-to-texture output later
    const uint32_t size = 512;       // 512x512 texture
    const uint32_t checkerSize = 64; // Size of each checker square

    // Create RGBA pixel data
    auto imageData = std::make_unique<uint8_t[]>(size * size * 4);

    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            uint32_t index = (y * size + x) * 4;

            // Create checkerboard pattern
            bool isWhite = ((x / checkerSize) + (y / checkerSize)) % 2 == 0;

            if (isWhite) {
                // White with full alpha
                imageData[index + 0] = 255; // R
                imageData[index + 1] = 255; // G
                imageData[index + 2] = 255; // B
                imageData[index + 3] = 255; // A
            } else {
                // Light blue with full alpha
                imageData[index + 0] = 100; // R
                imageData[index + 1] = 150; // G
                imageData[index + 2] = 255; // B
                imageData[index + 3] = 255; // A
            }
        }
    }

    // Create PremultipliedImage from raw data
    auto image = std::make_shared<PremultipliedImage>(Size{size, size}, std::move(imageData));

    // Create texture
    auto texture = context.createTexture2D();
    if (!texture) {
        Log::Error(Event::Render, "Failed to create test map texture");
        return nullptr;
    }

    // Set the image
    texture->setImage(image);

    // Configure sampler
    texture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                      .wrapU = gfx::TextureWrapType::Repeat,
                                      .wrapV = gfx::TextureWrapType::Repeat});

    Log::Info(Event::Render, "Test map texture created: " + std::to_string(size) + "x" + std::to_string(size));
    return texture;
}

std::unique_ptr<gfx::Drawable> RenderTerrain::createDrawableForTile(gfx::Context& context,
                                                                    gfx::ShaderRegistry& shaders,
                                                                    const OverscaledTileID& tileID,
                                                                    std::shared_ptr<gfx::Texture2D> demTexture,
                                                                    std::shared_ptr<gfx::Texture2D> mapTexture) {
    // Ensure mesh is generated
    const auto& terrainMesh = getMesh(context);

    if (terrainMesh.vertices.empty() || terrainMesh.indices.empty()) {
        Log::Error(Event::Render, "Terrain mesh is empty, cannot create drawable");
        return nullptr;
    }

    // Get terrain shader
    auto terrainShader = context.getGenericShader(shaders, "TerrainShader");
    if (!terrainShader) {
        Log::Error(Event::Render, "Terrain shader not found");
        return nullptr;
    }

    // Create drawable builder
    auto builder = context.createDrawableBuilder("terrain-tile");
    if (!builder) {
        Log::Error(Event::Render, "Failed to create drawable builder for terrain tile");
        return nullptr;
    }

    // Configure builder - terrain is 3D and writes depth.
    // Translucent pass renders in forward (ascending-index) order; the terrain
    // layer index (-1000) makes it draw first, before symbols.
    // letsgothru/terrain-3d: write depth so elevated symbols behind hills are
    // occluded by the GPU depth test (DepthMaskType::ReadWrite + enableDepth).
    // MLN_NO_TERRAIN_DEPTH disables the depth write (A/B aid for occlusion).
    const bool terrainWritesDepth = !std::getenv("MLN_NO_TERRAIN_DEPTH");
    builder->setShader(terrainShader);
    builder->setRenderPass(RenderPass::Translucent);
    builder->setDepthType(terrainWritesDepth ? gfx::DepthMaskType::ReadWrite : gfx::DepthMaskType::ReadOnly);
    builder->setColorMode(gfx::ColorMode::unblended());
    builder->setEnableDepth(terrainWritesDepth); // test + write depth for occlusion
    builder->setIs3D(false);                     // 2D depth mode (LessEqual); mesh supplies its own z

    // Set vertex data - copy vertices to raw buffer
    std::vector<uint8_t> vertexData(terrainMesh.vertices.size() * sizeof(int16_t));
    std::memcpy(vertexData.data(), terrainMesh.vertices.data(), vertexData.size());
    builder->setRawVertices(std::move(vertexData), terrainMesh.vertexCount, gfx::AttributeDataType::Short4);

    // Set index data and segments
    // Create a single segment covering the entire terrain mesh
    SegmentVector segments;
    segments.emplace_back(0,                       // vertex offset
                          0,                       // index offset
                          terrainMesh.vertexCount, // vertex count
                          terrainMesh.indexCount); // index count

    std::vector<uint16_t> indexData = terrainMesh.indices;
    builder->setSegments(gfx::Triangles(), std::move(indexData), segments.data(), segments.size());

    // Set the DEM texture
    if (demTexture) {
        builder->setTexture(demTexture, 0); // Texture index 0 for DEM
        Log::Info(Event::Render, "DEM texture bound to drawable for tile " + util::toString(tileID));
    } else {
        Log::Warning(Event::Render, "No DEM texture provided for tile " + util::toString(tileID));
    }

    if (!mapTexture) {
        mapTexture = createTestMapTexture(context);
        Log::Warning(Event::Render, "No map texture provided, using test pattern for tile " + util::toString(tileID));
    }
    if (mapTexture) {
        builder->setTexture(mapTexture, 1); // Texture index 1 for map
        Log::Info(Event::Render, "Map texture bound to drawable for tile " + util::toString(tileID));
    } else {
        Log::Warning(Event::Render, "Failed to create test map texture for tile " + util::toString(tileID));
    }

    // Flush to create the drawable
    builder->flush(context);

    // Get the drawable
    auto drawables = builder->clearDrawables();
    if (drawables.empty()) {
        Log::Error(Event::Render, "Failed to create terrain drawable for tile");
        return nullptr;
    }

    // Set tile ID on the drawable
    auto& drawable = drawables[0];
    drawable->setTileID(tileID);

    return std::move(drawable);
}

RenderSource* RenderTerrain::findDEMSource(const UpdateParameters& /*parameters*/) {
    // TODO: Implement source lookup
    // This would iterate through parameters.sources to find the raster-dem source
    // matching impl->sourceID
    return nullptr;
}

void RenderTerrain::activateLayerGroup(bool activate, UniqueChangeRequestVec& changes) {
    if (layerGroup) {
        if (activate) {
            changes.emplace_back(std::make_unique<AddLayerGroupRequest>(layerGroup));
        } else {
            changes.emplace_back(std::make_unique<RemoveLayerGroupRequest>(layerGroup));
        }
    }
}

} // namespace mbgl
