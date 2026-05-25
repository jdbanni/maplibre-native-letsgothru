#include <mbgl/renderer/render_target.hpp>

#include <mbgl/gfx/context.hpp>
#include <mbgl/gfx/offscreen_texture.hpp>
#include <mbgl/gfx/render_pass.hpp>
#include <mbgl/renderer/layer_group.hpp>
#include <mbgl/renderer/layer_tweaker.hpp>
#include <mbgl/renderer/paint_parameters.hpp>
#include <mbgl/renderer/render_tree.hpp>

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

    // Run layer tweakers to update any dynamic elements
    parameters.currentLayer = 0;
    visitLayerGroups([&](LayerGroupBase& layerGroup) {
        layerGroup.runTweakers(renderTree, parameters);
        parameters.currentLayer++;
    });

    // draw layer groups, opaque pass
    parameters.pass = RenderPass::Opaque;
    parameters.depthRangeSize = 1 -
                                (numLayerGroups() + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;

    parameters.currentLayer = 0;
    visitLayerGroupsReversed([&](LayerGroupBase& layerGroup) {
        layerGroup.render(orchestrator, parameters);
        parameters.currentLayer++;
    });

    // draw layer groups, translucent pass
    parameters.pass = RenderPass::Translucent;
    parameters.depthRangeSize = 1 -
                                (numLayerGroups() + 2) * PaintParameters::numSublayers * PaintParameters::depthEpsilon;

    parameters.currentLayer = static_cast<uint32_t>(numLayerGroups()) - 1;
    visitLayerGroups([&](LayerGroupBase& layerGroup) {
        layerGroup.render(orchestrator, parameters);
        if (parameters.currentLayer > 0) {
            parameters.currentLayer--;
        }
    });

    parameters.renderPass.reset();
    parameters.encoder->present(*offscreenTexture);

    parameters.scissorRect = prevScissorRect;
}

} // namespace mbgl
