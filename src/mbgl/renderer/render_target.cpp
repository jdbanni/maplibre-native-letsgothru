#include <mbgl/renderer/render_target.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_tree.hpp>
#include <mbgl/renderer/render_orchestrator.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/logging.hpp>

#include <cstdlib>

namespace mbgl {

RenderTarget::RenderTarget(gfx::Context& context_, const Size size, const gfx::TextureChannelDataType type)
    : context(context_) {
    offscreenTexture = context.createOffscreenTexture(size, type);
    backgroundColor = Color{0.0f, 0.0f, 0.0f, 1.0f};
}
RenderTarget::RenderTarget(gfx::Context& context_,
                           const Size size,
                           const gfx::TextureChannelDataType type,
                           const Color& backgroundColor_)
    : context(context_),
      backgroundColor(backgroundColor_) {
    offscreenTexture = context.createOffscreenTexture(size, type);
}
RenderTarget::RenderTarget(gfx::Context& context_,
                           const Size size,
                           const gfx::TextureChannelDataType type,
                           const Color& backgroundColor_,
                           bool depthStencil)
    : context(context_),
      backgroundColor(backgroundColor_) {
    // letsgothru terrain: request depth+stencil so fill/line drawables with
    // enableDepth=true (or stencil clipping) don't get silently dropped by
    // Metal pipeline validation when they're moved into RTT sub-groups.
    offscreenTexture = context.createOffscreenTexture(size, type, depthStencil, depthStencil);
}

RenderTarget::~RenderTarget() {}

const gfx::Texture2DPtr& RenderTarget::getTexture() {
    return offscreenTexture->getTexture();
};

void RenderTarget::setNoWaitOnSwap(bool value) {
    if (offscreenTexture) {
        offscreenTexture->getResource<gfx::RenderableResource>().setNoWaitOnSwap(value);
    }
}

bool RenderTarget::addLayerGroup(LayerGroupBasePtr layerGroup, const bool replace) {
    const auto index = layerGroup->getLayerIndex();
    const auto result = layerGroupsByLayerIndex.insert(std::make_pair(index, LayerGroupBasePtr{}));
    if (result.second) {
        // added
        result.first->second = std::move(layerGroup);
        return true;
    } else {
        // not added
        if (replace) {
            result.first->second = std::move(layerGroup);
            return true;
        } else {
            return false;
        }
    }
}

bool RenderTarget::removeLayerGroup(const int32_t layerIndex) {
    const auto hit = layerGroupsByLayerIndex.find(layerIndex);
    if (hit != layerGroupsByLayerIndex.end()) {
        layerGroupsByLayerIndex.erase(hit);
        return true;
    } else {
        return false;
    }
}

size_t RenderTarget::numLayerGroups() const noexcept {
    return layerGroupsByLayerIndex.size();
}

static const LayerGroupBasePtr no_group;

const LayerGroupBasePtr& RenderTarget::getLayerGroup(const int32_t layerIndex) const {
    const auto hit = layerGroupsByLayerIndex.find(layerIndex);
    return (hit == layerGroupsByLayerIndex.end()) ? no_group : hit->second;
}

void RenderTarget::upload(gfx::UploadPass& uploadPass) {
    visitLayerGroups(([&](LayerGroupBase& layerGroup) { layerGroup.upload(uploadPass); }));
}

void RenderTarget::render(RenderOrchestrator& orchestrator, const RenderTree& renderTree, PaintParameters& parameters) {
    // letsgothru/terrain-3d: skip re-rendering a terrain RTT whose draped content
    // is unchanged since it was last rendered. The drape is camera-independent,
    // so panning over the same region reuses the cached FBO texture and avoids
    // the per-RTT commit()+waitUntilCompleted() GPU stall. (Hillshade RTTs have
    // no terrainTileID and always render, as before.)
    // letsgothru/terrain-3d: the fingerprint dirty-skip (reuse an unchanged RTT
    // FBO instead of re-rendering) leaves a stale/blank texture in continuous
    // mode -- the offscreen texture is single-buffered and skipping its
    // commit()/swap() desyncs it, causing flicker and blank frames. It also
    // rarely fires when the DEM is over-zoomed (fingerprint churns every frame),
    // so it costs correctness without buying perf. Disabled by default; opt back
    // in with MLN_RTT_SKIP while the proper fix (batch RTT commits / drop the
    // per-RTT waitUntilCompleted stall) is pending.
    static const bool rttSkipEnabled = std::getenv("MLN_RTT_SKIP") != nullptr;
    if (rttSkipEnabled && terrainTileID && pendingFingerprint == renderedFingerprint) {
        return;
    }
    // letsgothru terrain: explicitly clear depth to 1.0 (far plane) and stencil to 0
    // so fill drawables that use depth/stencil tests start each frame from a sane state.
    // (The offscreen texture only actually has these attachments if it was created with
    //  depthStencil=true via the 5-arg constructor; backends that ignore the flag will
    //  see this as no-op.)
    parameters.renderPass = parameters.encoder->createRenderPass(
        "render target",
        {.renderable = *offscreenTexture,
         .clearColor = backgroundColor,
         .clearDepth = 1.0f,
         .clearStencil = 0});

    const gfx::ScissorRect prevScissorRect = parameters.scissorRect;
    const auto& size = getTexture()->getSize();
    parameters.scissorRect = {.x = 0, .y = 0, .width = size.width, .height = size.height};

    // letsgothru/terrain-3d: bind global uniform buffers for this offscreen pass.
    // Render targets are drawn in drawableTargetsPass, BEFORE the main pass binds
    // globals (renderer_impl.cpp), and each Metal render pass starts with no
    // bindings — so without this the draped drawables' shaders see no globals.
    context.bindGlobalUniformBuffers(*parameters.renderPass);

    if (terrainTileID) {
        // letsgothru/terrain-3d render-in-place draping (GL-JS-style): rather than
        // relocating drawables into this target, render the style's draped layer
        // groups in place with this terrain tile's RTT matrix. Source tiles that
        // don't overlap this terrain tile get a zero matrix and are culled (see
        // LayerTweaker::getTileMatrix), so every draped drawable can be visited
        // for every FBO and only the overlapping ones survive.
        const auto savedTerrainTileID = parameters.terrainTileID;
        parameters.terrainTileID = terrainTileID;
        const auto count = orchestrator.numLayerGroups();

        // letsgothru/terrain-3d DEBUG (MLN_PROXY_DEBUG): count draped drawables that
        // overlap this RTT's terrain tile (==/child/parent) -- i.e. the ones that
        // will produce a non-zero matrix and actually rasterise into the FBO.
        static const bool lgtProxyDebug = std::getenv("MLN_PROXY_DEBUG") != nullptr;
        if (lgtProxyDebug) {
            size_t overlap = 0, totalDraped = 0;
            const auto& tt = terrainTileID->canonical;
            orchestrator.visitLayerGroups([&](LayerGroupBase& lg) {
                if (!lg.shouldRenderToTerrain() || lg.getType() != LayerGroupBase::Type::TileLayerGroup) {
                    return;
                }
                static_cast<TileLayerGroup&>(lg).visitDrawables([&](gfx::Drawable& d) {
                    if (!d.getTileID()) return;
                    totalDraped++;
                    const auto c = d.getTileID()->canonical;
                    if (c == tt || c.isChildOf(tt) || tt.isChildOf(c)) overlap++;
                });
            });
            Log::Warning(Event::Render,
                         "PROXY drape RTT " + util::toString(*terrainTileID) + ": overlap=" +
                             std::to_string(overlap) + "/" + std::to_string(totalDraped) + " draped drawables");
        }

        // letsgothru/terrain-3d: source-tile stencil clipping is meaningless in
        // the per-terrain-tile FBO (the clip masks are projected in screen space).
        // Disable stencil on the draped drawables so they aren't culled here.
        // They are only rendered into the FBO (skipped on screen), so this is safe.
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            if (layerGroup.shouldRenderToTerrain()) {
                visitLayerGroupDrawables(layerGroup, [](gfx::Drawable& d) { d.setEnableStencil(false); });
            }
        });

        parameters.currentLayer = 0;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            if (layerGroup.shouldRenderToTerrain()) {
                layerGroup.runTweakers(renderTree, parameters);
            }
            parameters.currentLayer++;
        });

        parameters.pass = RenderPass::Opaque;
        parameters.depthRangeSize = 1 - (count + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;
        parameters.currentLayer = 0;
        orchestrator.visitLayerGroupsReversed([&](LayerGroupBase& layerGroup) {
            if (layerGroup.shouldRenderToTerrain()) {
                layerGroup.render(orchestrator, parameters);
            }
            parameters.currentLayer++;
        });

        parameters.pass = RenderPass::Translucent;
        parameters.depthRangeSize = 1 - (count + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;
        parameters.currentLayer = count > 0 ? static_cast<uint32_t>(count) - 1 : 0;
        orchestrator.visitLayerGroups([&](LayerGroupBase& layerGroup) {
            if (layerGroup.shouldRenderToTerrain()) {
                layerGroup.render(orchestrator, parameters);
            }
            if (parameters.currentLayer > 0) {
                parameters.currentLayer--;
            }
        });

        parameters.terrainTileID = savedTerrainTileID;
    } else {
        // Existing path (e.g. hillshade RTT): render this target's own sub-groups.
        parameters.currentLayer = 0;
        visitLayerGroups([&](LayerGroupBase& layerGroup) {
            layerGroup.runTweakers(renderTree, parameters);
            parameters.currentLayer++;
        });

        parameters.pass = RenderPass::Opaque;
        parameters.depthRangeSize = 1 - (numLayerGroups() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;
        parameters.currentLayer = 0;
        visitLayerGroupsReversed([&](LayerGroupBase& layerGroup) {
            layerGroup.render(orchestrator, parameters);
            parameters.currentLayer++;
        });

        parameters.pass = RenderPass::Translucent;
        parameters.depthRangeSize = 1 - (numLayerGroups() + 2) * PaintParameters::numSublayers *
                                            PaintParameters::depthEpsilon;
        parameters.currentLayer = static_cast<uint32_t>(numLayerGroups()) - 1;
        visitLayerGroups([&](LayerGroupBase& layerGroup) {
            layerGroup.render(orchestrator, parameters);
            if (parameters.currentLayer > 0) {
                parameters.currentLayer--;
            }
        });
    }

    parameters.renderPass.reset();
    parameters.encoder->present(*offscreenTexture);

    // letsgothru/terrain-3d: record the content we just rendered, so an unchanged
    // fingerprint next frame lets us reuse this FBO.
    renderedFingerprint = pendingFingerprint;

    parameters.scissorRect = prevScissorRect;
}

} // namespace mbgl
