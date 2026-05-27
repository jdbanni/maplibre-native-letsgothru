#pragma once

#include <mbgl/shaders/layer_ubo.hpp>

namespace mbgl {
namespace shaders {

struct alignas(16) TerrainDrawableUBO {
    /*  0 */ std::array<float, 4 * 4> matrix;
    /* 64 */ float exaggeration; // letsgothru: per-tile (uses the tile's own zoom)
    // letsgothru/terrain-3d: DEM uv transform so a (proxy) terrain tile can sample
    // a covering DEM tile: uv = dem_tl + (pos/EXTENT) * dem_scale. Identity
    // (0,0 / 1) when the mesh tile == the DEM tile.
    /* 68 */ float dem_scale;
    /* 72 */ std::array<float, 2> dem_tl;
    /* 80 */
};
static_assert(sizeof(TerrainDrawableUBO) == 5 * 16);

struct alignas(16) TerrainTilePropsUBO {
    /*  0 */ std::array<float, 2> dem_tl;
    /*  8 */ float dem_scale;
    /* 12 */ float pad1;
    /* 16 */
};
static_assert(sizeof(TerrainTilePropsUBO) == 16);

/// Evaluated properties that do not depend on the tile
struct alignas(16) TerrainEvaluatedPropsUBO {
    /*  0 */ float exaggeration;
    /*  4 */ float elevation_offset;
    /*  8 */ float pad1;
    /* 12 */ float pad2;
    /* 16 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 16);

} // namespace shaders
} // namespace mbgl
