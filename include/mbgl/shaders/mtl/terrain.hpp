#pragma once

#include <mbgl/shaders/terrain_layer_ubo.hpp>
#include <mbgl/shaders/shader_source.hpp>
#include <mbgl/shaders/mtl/shader_program.hpp>

namespace mbgl {
namespace shaders {

constexpr auto terrainShaderPrelude = R"(

enum {
    idTerrainDrawableUBO = idDrawableReservedVertexOnlyUBO,
    idTerrainTilePropsUBO = idDrawableReservedFragmentOnlyUBO,
    idTerrainEvaluatedPropsUBO = drawableReservedUBOCount,
    terrainUBOCount
};

struct alignas(16) TerrainDrawableUBO {
    /*  0 */ float4x4 matrix;
    /* 64 */
};
static_assert(sizeof(TerrainDrawableUBO) == 4 * 16, "wrong size");

struct alignas(16) TerrainTilePropsUBO {
    /*  0 */ float2 dem_tl;
    /*  8 */ float dem_scale;
    /* 12 */ float pad1;
    /* 16 */
};
static_assert(sizeof(TerrainTilePropsUBO) == 16, "wrong size");

/// Evaluated properties that do not depend on the tile
struct alignas(16) TerrainEvaluatedPropsUBO {
    /*  0 */ float exaggeration;
    /*  4 */ float elevation_offset;
    /*  8 */ float pad1;
    /* 12 */ float pad2;
    /* 16 */
};
static_assert(sizeof(TerrainEvaluatedPropsUBO) == 16, "wrong size");

)";

template <>
struct ShaderSource<BuiltIn::TerrainShader, gfx::Backend::Type::Metal> {
    static constexpr auto name = "TerrainShader";
    static constexpr auto vertexMainFunction = "vertexMain";
    static constexpr auto fragmentMainFunction = "fragmentMain";

    static const std::array<AttributeInfo, 2> attributes;
    static constexpr std::array<AttributeInfo, 0> instanceAttributes{};
    static const std::array<TextureInfo, 2> textures;

    static constexpr auto prelude = terrainShaderPrelude;
    static constexpr auto source = R"(

struct VertexStage {
    short2 pos [[attribute(terrainUBOCount + 0)]];
    short2 texture_pos [[attribute(terrainUBOCount + 1)]];
};

struct FragmentStage {
    float4 position [[position, invariant]];
    float2 uv;
    float elevation;
    float elevationMeters;
};

FragmentStage vertex vertexMain(thread const VertexStage vertx [[stage_in]],
                                device const uint32_t& uboIndex [[buffer(idGlobalUBOIndex)]],
                                device const TerrainDrawableUBO* drawableVector [[buffer(idTerrainDrawableUBO)]],
                                device const TerrainEvaluatedPropsUBO& props [[buffer(idTerrainEvaluatedPropsUBO)]],
                                texture2d<float, access::sample> demTexture [[texture(0)]],
                                sampler demSampler [[sampler(0)]]) {

    device const TerrainDrawableUBO& drawable = drawableVector[uboIndex];

    // Convert vertex position to normalized texture coordinates [0, 1]
    // The mesh was generated with coordinates from 0 to EXTENT (8192)
    float2 pos = float2(vertx.pos);
    float2 uv = pos / 8192.0;

    // Sample DEM texture to get raw RGBA values
    float4 demSample = demTexture.sample(demSampler, uv);

    // letsgothru/terrain-3d: decode Terrarium (Mapzen) encoding.
    // height = (R*256 + G + B/256) - 32768
    // DEM values are in range [0, 1] so convert back to [0, 255]
    // (Upstream PR #4190 assumed Mapbox terrain-rgb; we feed terrarium tiles.)
    float r = demSample.r * 255.0;
    float g = demSample.g * 255.0;
    float b = demSample.b * 255.0;

    // Calculate elevation in meters
    float elevationMeters = (r * 256.0 + g + b / 256.0) - 32768.0;

    // Apply exaggeration for visible relief (default: 1.0, can be set higher for dramatic effect)
    float elevation = elevationMeters * props.exaggeration;

    // Create 3D position with elevation as Z coordinate
    float4 position = drawable.matrix * float4(pos.x, pos.y, elevation, 1.0);

    // Pack elevation value for fragment shader visualization
    float packedValue = elevation;

    return {
        .position    = position,
        .uv          = uv,
        .elevation   = packedValue,  // Pass packed RGBA to detect any non-zero values
        .elevationMeters = elevationMeters,
    };
}

// letsgothru/terrain-3d: decode one Terrarium DEM sample (normalized [0,1]
// RGBA) to elevation in metres. Mirrors the vertex-stage decode.
static inline float lgtDecodeTerrarium(float4 s) {
    return (s.r * 255.0 * 256.0 + s.g * 255.0 + s.b * 255.0 / 256.0) - 32768.0;
}

half4 fragment fragmentMain(FragmentStage in [[stage_in]],
                            device const TerrainEvaluatedPropsUBO& props [[buffer(idTerrainEvaluatedPropsUBO)]],
                            texture2d<float, access::sample> demTexture [[texture(0)]],
                            sampler demSampler [[sampler(0)]],
                            texture2d<float, access::sample> mapTexture [[texture(1)]],
                            sampler mapSampler [[sampler(1)]]) {
#if defined(OVERDRAW_INSPECTOR)
    return half4(1.0);
#endif

    // --- Hillshade from the DEM gradient ---------------------------------
    // Estimate the surface normal from the elevation of the four neighbouring
    // DEM texels, then light it with a fixed directional source. This makes the
    // 3D relief read clearly even where the draped basemap is near-uniform.
    const float2 texel = float2(1.0 / float(demTexture.get_width()),
                                1.0 / float(demTexture.get_height()));
    const float hL = lgtDecodeTerrarium(demTexture.sample(demSampler, in.uv - float2(texel.x, 0.0)));
    const float hR = lgtDecodeTerrarium(demTexture.sample(demSampler, in.uv + float2(texel.x, 0.0)));
    const float hD = lgtDecodeTerrarium(demTexture.sample(demSampler, in.uv - float2(0.0, texel.y)));
    const float hU = lgtDecodeTerrarium(demTexture.sample(demSampler, in.uv + float2(0.0, texel.y)));

    // Vertical scale relative to horizontal texel spacing; folded with the
    // terrain exaggeration so the shading tracks the geometric relief.
    const float zScale = 0.04 * max(props.exaggeration, 0.1);
    const float3 normal = normalize(float3((hL - hR) * zScale, (hD - hU) * zScale, 1.0));
    // Light from the NW, moderately high — the cartographic hillshade convention.
    const float3 lightDir = normalize(float3(-0.5, 0.5, 0.7));
    const float diffuse = saturate(dot(normal, lightDir));
    // Ambient floor so shadowed faces keep some of the surface colour.
    const float shade = 0.5 + 0.55 * diffuse;

    // --- Surface colour ---------------------------------------------------
    // Sample the draped basemap (render-to-texture). Y is flipped to match the
    // GL convention used when populating the FBO.
    float4 mapColor = mapTexture.sample(mapSampler, float2(in.uv.x, 1.0 - in.uv.y));

    float3 base;
    if (mapColor.a > 0.01) {
        base = float3(mapColor.rgb);
    } else {
        // Fallback elevation ramp for when no basemap has been draped yet.
        const float n = clamp((in.elevation - 500.0) / 3500.0, 0.0, 1.0);
        if (n < 0.33) {
            base = mix(float3(0.2, 0.4, 0.8), float3(0.3, 0.7, 0.3), n / 0.33);
        } else if (n < 0.66) {
            base = mix(float3(0.3, 0.7, 0.3), float3(0.6, 0.5, 0.3), (n - 0.33) / 0.33);
        } else {
            base = mix(float3(0.6, 0.5, 0.3), float3(0.95, 0.95, 0.95), (n - 0.66) / 0.34);
        }
    }

    return half4(half3(base * shade), 1.0);
}
)";
};

} // namespace shaders
} // namespace mbgl
