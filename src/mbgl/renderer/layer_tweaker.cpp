#include <mbgl/renderer/layer_tweaker.hpp>

#include <mbgl/map/transform_state.hpp>
#include <mbgl/style/layer_properties.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/shaders/layer_ubo.hpp>
#include <mbgl/util/mat4.hpp>
#include <mbgl/util/containers.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mbgl/util/monotonic_timer.hpp>
#include <chrono>
#endif // MLN_RENDER_BACKEND_METAL

namespace mbgl {

LayerTweaker::LayerTweaker(std::string id_, Immutable<style::LayerProperties> properties)
    : id(std::move(id_)),
      evaluatedProperties(std::move(properties)) {}

bool LayerTweaker::checkTweakDrawable(const gfx::Drawable& drawable) const {
    // Apply to a drawable if it references us, or if doesn't reference anything.
    const auto& tweaker = drawable.getLayerTweaker();
    return !tweaker || tweaker.get() == this;
}

mat4 getTerrainRttPosMatrix(const UnwrappedTileID& tileID, const UnwrappedTileID& terrainTileID) {
    mat4 terrainRttPosMatrix;
    if (tileID == terrainTileID) {
        matrix::ortho(terrainRttPosMatrix, 0, util::EXTENT, util::EXTENT, 0, 0, 1);
    } else if (terrainTileID.canonical.isChildOf(tileID.canonical)) {
        const int dz = terrainTileID.canonical.z - tileID.canonical.z;
        const int dx = terrainTileID.canonical.x - (terrainTileID.canonical.x >> dz << dz);
        const int dy = terrainTileID.canonical.y - (terrainTileID.canonical.y >> dz << dz);
        const int size = util::EXTENT >> dz;
        matrix::ortho(
            terrainRttPosMatrix, 0, size, size, 0, 0, 1); // Note: we are using `size` instead of `EXTENT` here
        matrix::translate(terrainRttPosMatrix, terrainRttPosMatrix, -dx * size, -dy * size, 0);
    } else if (tileID.canonical.isChildOf(terrainTileID.canonical)) {
        const int dz = tileID.canonical.z - terrainTileID.canonical.z;
        const int dx = tileID.canonical.x - (tileID.canonical.x >> dz << dz);
        const int dy = tileID.canonical.y - (tileID.canonical.y >> dz << dz);
        const int size = util::EXTENT >> dz;
        matrix::ortho(terrainRttPosMatrix, 0, util::EXTENT, util::EXTENT, 0, 0, 1);
        matrix::translate(terrainRttPosMatrix, terrainRttPosMatrix, dx * size, dy * size, 0);
        matrix::scale(terrainRttPosMatrix, terrainRttPosMatrix, 1.0 / (1 << dz), 1.0 / (1 << dz), 0);
    }
    // letsgothru/terrain-3d: matrix::ortho() is GL-convention and maps the z=0
    // draped geometry to clip z = -1. Metal/Vulkan/WebGPU clip z to [0,1], so
    // every fragment would be discarded. Draped fills are flat (z=0) and depth
    // is disabled in the RTT, so force the z translation to 0 → clip z = 0.
    terrainRttPosMatrix[14] = 0.0;
    return terrainRttPosMatrix;
}

mat4 LayerTweaker::getTileMatrix(const UnwrappedTileID& tileID,
                                 const PaintParameters& parameters,
                                 const std::array<float, 2>& translation,
                                 style::TranslateAnchorType anchor,
                                 bool nearClipped,
                                 bool inViewportPixelUnits,
                                 const gfx::Drawable& drawable,
                                 bool aligned,
                                 bool renderToTerrain) {
    // letsgothru/terrain-3d render-in-place draping: when rendering into a
    // specific terrain tile's offscreen FBO, use that tile directly. Source
    // tiles that don't overlap it fall through to a zero matrix (default mat4)
    // and are naturally culled, so every draped drawable can be rendered into
    // every FBO and only the overlapping ones survive.
    if (renderToTerrain && parameters.terrainTileID) {
        return getTerrainRttPosMatrix(tileID, *parameters.terrainTileID);
    }
    std::optional<UnwrappedTileID> terrainTileID;
    if (renderToTerrain && parameters.texturePool.getRenderTargetAncestorOrDescendant(tileID, terrainTileID)) {
        return getTerrainRttPosMatrix(tileID, *terrainTileID);
    }
    // from RenderTile::prepare
    mat4 tileMatrix;
    parameters.state.matrixFor(/*out*/ tileMatrix, tileID);
    if (const auto& origin{drawable.getOrigin()}; origin.has_value()) {
        matrix::translate(tileMatrix, tileMatrix, origin->x, origin->y, 0);
    }
    multiplyWithProjectionMatrix(/*in-out*/ tileMatrix, parameters, drawable, nearClipped, aligned);
    return RenderTile::translateVtxMatrix(
        tileID, tileMatrix, translation, anchor, parameters.state, inViewportPixelUnits);
}

void LayerTweaker::updateProperties(Immutable<style::LayerProperties> newProps) {
    evaluatedProperties = std::move(newProps);
    propertiesUpdated = true;
}

void LayerTweaker::multiplyWithProjectionMatrix(/*in-out*/ mat4& matrix,
                                                const PaintParameters& parameters,
                                                [[maybe_unused]] const gfx::Drawable& drawable,
                                                bool nearClipped,
                                                bool aligned) {
    // nearClippedMatrix has near plane moved further, to enhance depth buffer precision
    const auto& projMatrixRef = aligned ? parameters.transformParams.alignedProjMatrix
                                        : (nearClipped ? parameters.transformParams.nearClippedProjMatrix
                                                       : parameters.transformParams.projMatrix);
#if !MLN_RENDER_BACKEND_OPENGL
    // If this drawable is participating in depth testing, offset the
    // projection matrix NDC depth range for the drawable's layer and sublayer.
    if (!drawable.getIs3D() && drawable.getEnableDepth()) {
        // copy and adjust the projection matrix
        mat4 projMatrix = projMatrixRef;
        projMatrix[14] -= ((1 + parameters.currentLayer) * PaintParameters::numSublayers -
                           drawable.getSubLayerIndex()) *
                          PaintParameters::depthEpsilon;
        // multiply with the copy
        matrix::multiply(matrix, projMatrix, matrix);
        // early return
        return;
    }
#endif
    matrix::multiply(matrix, projMatrixRef, matrix);
}

} // namespace mbgl
