#pragma once

#include <mbgl/gfx/types.hpp>
#include <mbgl/util/size.hpp>
#include <mbgl/util/color.hpp>
#include <mbgl/tile/tile_id.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace mbgl {

namespace gfx {
class Context;
class Texture2D;
class OffscreenTexture;
class UploadPass;
using Texture2DPtr = std::shared_ptr<Texture2D>;
} // namespace gfx

class LayerGroupBase;
class PaintParameters;
class RenderOrchestrator;
class RenderTree;

using LayerGroupBasePtr = std::shared_ptr<LayerGroupBase>;

/// Render target class
class RenderTarget {
public:
    RenderTarget(gfx::Context& context, const Size size, const gfx::TextureChannelDataType type);
    RenderTarget(gfx::Context& context,
                 const Size size,
                 const gfx::TextureChannelDataType type,
                 const Color& backgroundColor);
    /// Construct a RenderTarget with explicit depth/stencil attachment control.
    /// letsgothru: terrain RTT FBOs need depth so fill drawables with enableDepth=true
    /// pass Metal pipeline validation.
    RenderTarget(gfx::Context& context,
                 const Size size,
                 const gfx::TextureChannelDataType type,
                 const Color& backgroundColor,
                 bool depthStencil);
    ~RenderTarget();

    /// Get the render target texture
    const gfx::Texture2DPtr& getTexture();

    /// @brief Add a layer group to the render target
    /// @param replace Flag to replace if exists
    /// @return whether added
    bool addLayerGroup(LayerGroupBasePtr, bool replace);

    /// @brief Remove a layer group
    /// @param layerIndex index of the layer to remove
    /// @return whether removed
    bool removeLayerGroup(const int32_t layerIndex);

    /// Get the layer group count
    size_t numLayerGroups() const noexcept;

    /// @brief  Get a specific layer group by index
    /// @param layerIndex index
    /// @return the layer group if existant, othewise a shared null pointer
    const LayerGroupBasePtr& getLayerGroup(const int32_t layerIndex) const;

    /// Execute the given function for each contained layer group
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroups(Func f) {
        for (auto& pair : layerGroupsByLayerIndex) {
            if (pair.second) {
                f(*pair.second);
            }
        }
    }

    /// Execute the given function for each contained layer group in reversed order
    template <typename Func /* void(LayerGroupBase&) */>
    void visitLayerGroupsReversed(Func f) {
        for (auto rit = layerGroupsByLayerIndex.rbegin(); rit != layerGroupsByLayerIndex.rend(); ++rit) {
            if (rit->second) {
                f(*rit->second);
            }
        }
    }

    /// Upload the layer groups
    void upload(gfx::UploadPass& uploadPass);

    /// Render the layer groups
    void render(RenderOrchestrator&, const RenderTree&, PaintParameters&);

    /// letsgothru/terrain-3d render-in-place: the terrain tile this FBO drapes.
    void setTerrainTileID(const UnwrappedTileID& id) { terrainTileID = id; }

    /// letsgothru/terrain-3d: per-frame content fingerprint for dirty-tracking.
    /// The draped content is camera-independent, so an unchanged fingerprint
    /// means the FBO can be reused (skip re-render + the per-RTT GPU sync).
    void resetFingerprint() { pendingFingerprint = 0; }
    void addFingerprint(std::size_t h) { pendingFingerprint = pendingFingerprint * 1000003u + (h + 1); }

protected:
    gfx::Context& context;
    std::unique_ptr<gfx::OffscreenTexture> offscreenTexture;
    using LayerGroupMap = std::map<int32_t, LayerGroupBasePtr>;
    LayerGroupMap layerGroupsByLayerIndex;
    Color backgroundColor;
    std::optional<UnwrappedTileID> terrainTileID;
    std::size_t pendingFingerprint = 0;
    std::size_t renderedFingerprint = static_cast<std::size_t>(-1); // sentinel: never rendered
};

} // namespace mbgl
