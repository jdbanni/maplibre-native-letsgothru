#include <mbgl/renderer/layers/fill_extrusion_layer_tweaker.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/drawable.hpp>
#include <mbgl/gfx/renderable.hpp>
#include <mbgl/gfx/renderer_backend.hpp>
#include <mbgl/renderer/buckets/fill_extrusion_bucket.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/render_tile.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/paint_property_binder.hpp>
#include <mbgl/renderer/render_terrain.hpp>
#include <mbgl/shaders/fill_extrusion_layer_ubo.hpp>
#include <mbgl/shaders/shader_defines.hpp>
#include <cmath>
#include <cstdlib>
#include <mbgl/shaders/shader_program_base.hpp>
#include <mbgl/style/layers/fill_extrusion_layer_properties.hpp>

#if MLN_RENDER_BACKEND_METAL
#include <mbgl/shaders/mtl/fill_extrusion.hpp>
#endif

namespace mbgl {

using namespace shaders;
using namespace style;

void FillExtrusionLayerTweaker::execute(LayerGroupBase& layerGroup, const PaintParameters& parameters) {
    if (layerGroup.empty()) {
        return;
    }

    auto& context = parameters.context;
    const auto& props = static_cast<const FillExtrusionLayerProperties&>(*evaluatedProperties);
    const auto& evaluated = props.evaluated;
    const auto& crossfade = props.crossfade;
    const auto& state = parameters.state;

#if !defined(NDEBUG)
    const auto label = layerGroup.getName() + "-update-uniforms";
    const auto debugGroup = parameters.encoder->createDebugGroup(label.c_str());
#endif

    // UBO depends on more than just evaluated properties, so we need to update every time,
    // but the resulting buffer can be shared across all the drawables from the layer.
    const FillExtrusionPropsUBO propsUBO = {
        .color = constOrDefault<FillExtrusionColor>(evaluated),
        .light_color = FillExtrusionBucket::lightColor(parameters.evaluatedLight),
        .pad1 = 0,
        .light_position = FillExtrusionBucket::lightPosition(parameters.evaluatedLight, state),
        .base = constOrDefault<FillExtrusionBase>(evaluated),
        .height = constOrDefault<FillExtrusionHeight>(evaluated),
        .light_intensity = FillExtrusionBucket::lightIntensity(parameters.evaluatedLight),
        .vertical_gradient = evaluated.get<FillExtrusionVerticalGradient>() ? 1.0f : 0.0f,
        .opacity = evaluated.get<FillExtrusionOpacity>(),
        .fade = crossfade.t,
        .from_scale = crossfade.fromScale,
        .to_scale = crossfade.toScale,
        .pad2 = 0};
    auto& layerUniforms = layerGroup.mutableUniformBuffers();
    layerUniforms.createOrUpdate(idFillExtrusionPropsUBO, &propsUBO, context);

    propertiesUpdated = false;

    const auto zoom = static_cast<float>(parameters.state.getZoom());
    const auto defPattern = mbgl::Faded<expression::Image>{.from = "", .to = ""};
    const auto fillPatternValue = evaluated.get<FillExtrusionPattern>().constantOr(defPattern);


#if MLN_UBO_CONSOLIDATION
    int i = 0;
    std::vector<FillExtrusionDrawableUBO> drawableUBOVector(layerGroup.getDrawableCount());
    std::vector<FillExtrusionTilePropsUBO> tilePropsUBOVector(layerGroup.getDrawableCount());
#endif

    visitLayerGroupDrawables(layerGroup, [&](gfx::Drawable& drawable) {
        if (!drawable.getTileID() || !checkTweakDrawable(drawable)) {
            return;
        }

        auto* binders = static_cast<FillExtrusionBinders*>(drawable.getBinders());
        const auto* tile = drawable.getRenderTile();
        if (!binders || !tile) {
            assert(false);
            return;
        }

        const UnwrappedTileID tileID = drawable.getTileID()->toUnwrapped();
        const auto& translation = evaluated.get<FillExtrusionTranslate>();
        const auto anchor = evaluated.get<FillExtrusionTranslateAnchor>();
        constexpr bool inViewportPixelUnits = false; // from RenderTile::translatedMatrix
        constexpr bool nearClipped = true;
        // letsgothru/terrain-3d: fill-extrusion is 3D and drawn on screen, NOT draped.
        // getTileMatrix defaults renderToTerrain=true, which returns the flat RTT
        // draping matrix when terrain is active and collapses the buildings -- pass
        // false (as symbols do) to get the real 3D projection.
        constexpr bool aligned = false;
        constexpr bool renderToTerrain = false;
        const auto matrix = getTileMatrix(
            tileID, parameters, translation, anchor, nearClipped, inViewportPixelUnits, drawable, aligned, renderToTerrain);

        const auto tileRatio = 1 / tileID.pixelsToTileUnits(1, state.getIntegerZoom());
        const auto zoomScale = state.zoomScale(tileID.canonical.z);
        const auto nearestZoomScale = state.zoomScale(state.getIntegerZoom() - tileID.canonical.z);
        const auto tileSizeAtNearestZoom = std::floor(util::tileSize_D * nearestZoomScale);
        const auto pixelX = static_cast<int32_t>(tileSizeAtNearestZoom *
                                                 (tileID.canonical.x + tileID.wrap * zoomScale));
        const auto pixelY = static_cast<int32_t>(tileSizeAtNearestZoom * tileID.canonical.y);
        const auto numTiles = std::pow(2, tileID.canonical.z);
        const auto heightFactor = static_cast<float>(-numTiles / util::tileSize_D / 8.0);

        // letsgothru/terrain-3d: bind the covering DEM so the shader lifts the
        // building base onto the surface. has_terrain stays 0 if none covers it.
        std::optional<RenderTerrain::DEMTextureRef> demRef;
        float metersToTileUnitsXExag = 0.0f;
        if (parameters.terrain && !std::getenv("MLN_NO_EXTRUSION_ELEVATION")) {
            demRef = parameters.terrain->getDEMTextureFor(tileID);
            if (demRef) {
                drawable.setTexture(demRef->texture, idFillExtrusionDEMTexture);
                // Unlike symbols/mesh (tile-unit matrices), the fill-extrusion
                // nearClipped matrix maps z as metres -- a sea-level building
                // renders its `height` metres at the correct size. So the surface
                // lift is just demMetres * exaggeration (raw metres), which lands
                // at the same real-world height as the mesh (mesh pre-converts
                // metres->tile-units, then its matrix maps back to the same metres).
                metersToTileUnitsXExag = parameters.terrain->getExaggeration();
            }
        }

        Size textureSize = {0, 0};
        if (const auto& tex = drawable.getTexture(idFillExtrusionImageTexture)) {
            textureSize = tex->getSize();
        }

        const auto hasPattern = !!drawable.getType();
        const auto patternPosA = tile->getPattern(fillPatternValue.from.id());
        const auto patternPosB = tile->getPattern(fillPatternValue.to.id());
        if (hasPattern) {
            binders->setPatternParameters(patternPosA, patternPosB, crossfade);
        }

#if MLN_UBO_CONSOLIDATION
        drawableUBOVector[i] = {
#else
        const FillExtrusionDrawableUBO drawableUBO = {
#endif
            .matrix = util::cast<float>(matrix),
            .pixel_coord_upper = {static_cast<float>(pixelX >> 16), static_cast<float>(pixelY >> 16)},
            .pixel_coord_lower = {static_cast<float>(pixelX & 0xFFFF), static_cast<float>(pixelY & 0xFFFF)},
            .height_factor = heightFactor,
            .tile_ratio = tileRatio,

            .base_t = std::get<0>(binders->get<FillExtrusionBase>()->interpolationFactor(zoom)),
            .height_t = std::get<0>(binders->get<FillExtrusionHeight>()->interpolationFactor(zoom)),
            .color_t = std::get<0>(binders->get<FillExtrusionColor>()->interpolationFactor(zoom)),
            .pattern_from_t = std::get<0>(binders->get<FillExtrusionPattern>()->interpolationFactor(zoom)),
            .pattern_to_t = std::get<0>(binders->get<FillExtrusionPattern>()->interpolationFactor(zoom)),
            .has_terrain = demRef ? 1 : 0,
            .meters_to_tile_x_exag = metersToTileUnitsXExag,
            .dem_scale = demRef ? demRef->demScale : 1.0f,
            .dem_tl = demRef ? std::array<float, 2>{demRef->demTlX, demRef->demTlY} : std::array<float, 2>{0.0f, 0.0f},
        };

#if MLN_UBO_CONSOLIDATION
        tilePropsUBOVector[i] = {
#else
        const FillExtrusionTilePropsUBO tilePropsUBO = {
#endif
            .pattern_from = patternPosA ? util::cast<float>(patternPosA->tlbr()) : std::array<float, 4>{0},
            .pattern_to = patternPosB ? util::cast<float>(patternPosB->tlbr()) : std::array<float, 4>{0},
            .texsize = {static_cast<float>(textureSize.width), static_cast<float>(textureSize.height)},
            .pad1 = 0,
            .pad2 = 0
        };

#if MLN_UBO_CONSOLIDATION
        drawable.setUBOIndex(i++);
#else
        auto& drawableUniforms = drawable.mutableUniformBuffers();
        drawableUniforms.createOrUpdate(idFillExtrusionDrawableUBO, &drawableUBO, context);
        drawableUniforms.createOrUpdate(idFillExtrusionTilePropsUBO, &tilePropsUBO, context);
#endif
    });

#if MLN_UBO_CONSOLIDATION
    const size_t drawableUBOVectorSize = sizeof(FillExtrusionDrawableUBO) * drawableUBOVector.size();
    if (!drawableUniformBuffer || drawableUniformBuffer->getSize() < drawableUBOVectorSize) {
        drawableUniformBuffer = context.createUniformBuffer(
            drawableUBOVector.data(), drawableUBOVectorSize, false, true);
    } else {
        drawableUniformBuffer->update(drawableUBOVector.data(), drawableUBOVectorSize);
    }

    const size_t tilePropsUBOVectorSize = sizeof(FillExtrusionTilePropsUBO) * tilePropsUBOVector.size();
    if (!tilePropsUniformBuffer || tilePropsUniformBuffer->getSize() < tilePropsUBOVectorSize) {
        tilePropsUniformBuffer = context.createUniformBuffer(
            tilePropsUBOVector.data(), tilePropsUBOVectorSize, false, true);
    } else {
        tilePropsUniformBuffer->update(tilePropsUBOVector.data(), tilePropsUBOVectorSize);
    }

    layerUniforms.set(idFillExtrusionDrawableUBO, drawableUniformBuffer);
    layerUniforms.set(idFillExtrusionTilePropsUBO, tilePropsUniformBuffer);
#endif
}

} // namespace mbgl
