#include <mbgl/renderer/layers/terrain_layer_tweaker.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <mbgl/util/convert.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/logging.hpp>

namespace mbgl {

using namespace shaders;

void TerrainLayerTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    Log::Info(Event::Render,
              "TerrainLayerTweaker::execute called, layerGroup.empty()=" + std::to_string(layerGroup.empty()) +
                  ", terrain=" + std::to_string(terrain != nullptr));

    if (layerGroup.empty() || !terrain) {
        Log::Warning(Event::Render, "TerrainLayerTweaker::execute early return - empty or no terrain");
        return;
    }

    Log::Info(Event::Render,
              "TerrainLayerTweaker processing " + std::to_string(layerGroup.getDrawableCount()) + " drawables");

    auto& context = parameters.context;

#if defined(DEBUG)
    const auto label = layerGroup.getName() + "-update-uniforms";
    const auto debugGroup = parameters.encoder->createDebugGroup(label.c_str());
#endif

    // letsgothru/terrain-3d: convert elevation metres -> tile-space units. At zoom z
    // and latitude lat, 1 metre ≈ (2^z * EXTENT) / (cos(lat) * earth_circumference)
    // tile units. We bake this into a per-tile exaggeration (below) so the shader
    // needs no projection knowledge.
    //
    // CRITICAL: this MUST use each tile's OWN canonical zoom, not the view zoom.
    // The tile matrix maps z in the tile's own zoom units; above the DEM maxzoom
    // tiles are over-scaled (canonical zoom capped). A single layer-wide value
    // made the whole mesh flatten/double then snap back during zoom/rotate
    // transitions when tiles of different zooms briefly coexisted -- hence the
    // per-drawable computation in the loop.
    const float baseExaggeration = terrain->getExaggeration();
    constexpr float EARTH_CIRCUMFERENCE_M = 40075016.686f;
    constexpr float EXTENT_F = 8192.0f;
    const float latRad = static_cast<float>(parameters.state.getLatLng().latitude() * M_PI / 180.0);
    const float cosLat = std::max(0.05f, std::cos(latRad));
    const float exagPerTileFactor = baseExaggeration * EXTENT_F / (cosLat * EARTH_CIRCUMFERENCE_M);

    // Populate layer-level UBO (exaggeration here is unused by the shader now; it
    // reads the per-drawable value below).
    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    const TerrainEvaluatedPropsUBO propsUBO = {
        .exaggeration = baseExaggeration, .elevation_offset = 0.0f, .pad1 = 0.0f, .pad2 = 0.0f};
    layerUniforms.createOrUpdate(idTerrainEvaluatedPropsUBO, &propsUBO, context);

#if MLN_UBO_CONSOLIDATION
    int i = 0;
    std::vector<TerrainDrawableUBO> drawableUBOVector(layerGroup.getDrawableCount());
#endif

    // Visit each drawable to populate per-drawable UBOs
    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        if (!drawable.getTileID()) {
            return;
        }

        const UnwrappedTileID tileID = drawable.getTileID()->toUnwrapped();

        // Calculate transformation matrix for this terrain tile
        // This uses the same matrix calculation as other layers
        mat4 matrix = parameters.matrixForTile(tileID);

        // letsgothru/terrain-3d: per-tile exaggeration using this tile's own zoom.
        const float tileExaggeration = std::exp2(static_cast<float>(tileID.canonical.z)) * exagPerTileFactor;

#if !MLN_UBO_CONSOLIDATION
        auto& drawableUniforms = drawable.mutableUniformBuffers();
#endif

#if MLN_UBO_CONSOLIDATION
        drawableUBOVector[i] = {
#else
        const TerrainDrawableUBO drawableUBO = {
#endif
            .matrix = util::cast<float>(matrix),
            .exaggeration = tileExaggeration,
            .pad0 = 0.0f,
            .pad1 = 0.0f,
            .pad2 = 0.0f,
        };

#if !MLN_UBO_CONSOLIDATION
        drawableUniforms.createOrUpdate(idTerrainDrawableUBO, &drawableUBO, context);
#endif

#if MLN_UBO_CONSOLIDATION
        drawable.setUBOIndex(i++);
#endif
    });

#if MLN_UBO_CONSOLIDATION
    const size_t drawableUBOVectorSize = sizeof(TerrainDrawableUBO) * drawableUBOVector.size();
    if (!drawableUniformBuffer || drawableUniformBuffer->getSize() < drawableUBOVectorSize) {
        drawableUniformBuffer = context.createUniformBuffer(
            drawableUBOVector.data(), drawableUBOVectorSize, false, true);
    } else {
        drawableUniformBuffer->update(drawableUBOVector.data(), drawableUBOVectorSize);
    }

    layerUniforms.set(idTerrainDrawableUBO, drawableUniformBuffer);
#endif
}

} // namespace mbgl
