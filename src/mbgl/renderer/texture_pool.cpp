#include <mbgl/renderer/texture_pool.hpp>
#include <mbgl/gfx/context.hpp>
#include <mbgl/util/logging.hpp>

namespace mbgl {
TexturePool::TexturePool(uint32_t tilesize)
    : tileSize(tilesize) {}

TexturePool::~TexturePool() {}

void TexturePool::createRenderTarget(gfx::Context& context, const UnwrappedTileID& id, const Color& backgroundColor) {
    // letsgothru terrain: construct with depthStencil=true so the FBO has
    // depth+stencil attachments. Without these, fill/line drawables with
    // enableDepth=true are silently dropped by Metal pipeline validation and
    // the RTT comes back near-empty -- which is why terrain renders flat.
    auto renderTarget = std::make_shared<RenderTarget>(
        context,
        Size{tileSize, tileSize},
        gfx::TextureChannelDataType::UnsignedByte,
        backgroundColor,
        /*depthStencil=*/true);
    // letsgothru/terrain-3d render-in-place: remember which terrain tile this FBO
    // drapes, so RenderTarget::render can set the per-tile RTT matrix context.
    renderTarget->setTerrainTileID(id);
    renderTargets[id] = std::move(renderTarget);
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTarget(const UnwrappedTileID& id) const {
    return renderTargets.contains(id) ? renderTargets.at(id) : nullptr;
}

std::shared_ptr<RenderTarget> TexturePool::getRenderTargetAncestorOrDescendant(
    const UnwrappedTileID& id, std::optional<UnwrappedTileID>& terrainTileID) const {
    terrainTileID = std::nullopt;
    std::shared_ptr<RenderTarget> bestRenderTarget;
    for (const auto& [tileID, renderTarget] : renderTargets) {
        if (tileID == id || tileID.isChildOf(id) || id.isChildOf(tileID)) {
            if (!terrainTileID || tileID.canonical.z > terrainTileID->canonical.z) {
                terrainTileID = tileID;
                bestRenderTarget = renderTarget;
            }
        }
    }
    return bestRenderTarget;
}
} // namespace mbgl
