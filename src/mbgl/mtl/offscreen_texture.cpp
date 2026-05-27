#include <mbgl/mtl/offscreen_texture.hpp>
#include <mbgl/mtl/context.hpp>
#include <mbgl/mtl/renderable_resource.hpp>
#include <mbgl/mtl/renderer_backend.hpp>
#include <mbgl/mtl/texture2d.hpp>

#include <Metal/Metal.hpp>

namespace mbgl {
namespace mtl {

class OffscreenTextureResource final : public RenderableResource {
public:
    OffscreenTextureResource(Context& context_,
                             const Size size_,
                             const gfx::TextureChannelDataType type_,
                             bool depth,
                             [[maybe_unused]] bool stencil)
        : context(context_),
          size(size_),
          type(type_) {
        assert(!size.isEmpty());
        colorTexture = context.createTexture2D();
        colorTexture->setSize(size);
        colorTexture->setFormat(gfx::TexturePixelType::RGBA, type);
        colorTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                               .wrapU = gfx::TextureWrapType::Clamp,
                                               .wrapV = gfx::TextureWrapType::Clamp});
        static_cast<Texture2D*>(colorTexture.get())
            ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);

        if (depth) {
            depthTexture = context.createTexture2D();
            depthTexture->setSize(size);
            depthTexture->setFormat(gfx::TexturePixelType::Depth, gfx::TextureChannelDataType::Float);
            depthTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                   .wrapU = gfx::TextureWrapType::Clamp,
                                                   .wrapV = gfx::TextureWrapType::Clamp});
            static_cast<Texture2D*>(depthTexture.get())
                ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);
        }

        // On iOS simulator, the depth target is PixelFormatDepth32Float_Stencil8
#if !TARGET_OS_SIMULATOR
        if (stencil) {
            stencilTexture = context.createTexture2D();
            stencilTexture->setSize(size);
            stencilTexture->setFormat(gfx::TexturePixelType::Stencil, gfx::TextureChannelDataType::UnsignedByte);
            stencilTexture->setSamplerConfiguration({.filter = gfx::TextureFilterType::Linear,
                                                     .wrapU = gfx::TextureWrapType::Clamp,
                                                     .wrapV = gfx::TextureWrapType::Clamp});
            static_cast<Texture2D*>(stencilTexture.get())
                ->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget);
        }
#endif

        context.renderingStats().numFrameBuffers++;
    }

    ~OffscreenTextureResource() noexcept override { context.renderingStats().numFrameBuffers--; }

    void bind() override {
        assert(context.getBackend().getCommandQueue());
        commandBuffer = NS::RetainPtr(context.getBackend().getCommandQueue()->commandBuffer());
        colorTexture->create();

        renderPassDescriptor = NS::TransferPtr(MTL::RenderPassDescriptor::alloc()->init());
        if (auto* colorTarget = renderPassDescriptor->colorAttachments()->object(0)) {
            colorTarget->setTexture(static_cast<Texture2D*>(colorTexture.get())->getMetalTexture());
        }

        if (depthTexture) {
            depthTexture->create();
            if (auto* depthTarget = renderPassDescriptor->depthAttachment()) {
                depthTarget->setTexture(static_cast<Texture2D*>(depthTexture.get())->getMetalTexture());
            }
        }
        if (stencilTexture) {
            stencilTexture->create();
            if (auto* stencilTarget = renderPassDescriptor->stencilAttachment()) {
                stencilTarget->setTexture(static_cast<Texture2D*>(stencilTexture.get())->getMetalTexture());
            }
        }
    }

    void setNoWaitOnSwap(bool value) override { noWaitOnSwap = value; }

    void swap() override {
        assert(commandBuffer);
        commandBuffer->commit();
        // letsgothru/terrain-3d: terrain drape RTTs are sampled on-GPU by the
        // terrain mesh later this frame; the queue orders this commit before that
        // pass, so the CPU wait is unnecessary (and costs one stall per tile). CPU
        // readback paths (headless) leave noWaitOnSwap false and still wait here.
        if (!noWaitOnSwap) {
            commandBuffer->waitUntilCompleted();
        }
        commandBuffer.reset();
        renderPassDescriptor.reset();
    }

    PremultipliedImage readStillImage() {
        assert(static_cast<Texture2D*>(colorTexture.get())->getMetalTexture());

        auto data = std::make_unique<uint8_t[]>(colorTexture->getDataSize());
        MTL::Region region = MTL::Region::Make2D(0, 0, size.width, size.height);
        NS::UInteger bytesPerRow = size.width * colorTexture->getPixelStride();

        static_cast<Texture2D*>(colorTexture.get())->getMetalTexture()->getBytes(data.get(), bytesPerRow, region, 0);

        return {size, std::move(data)};
    }

    gfx::Texture2DPtr& getTexture() {
        assert(colorTexture);
        return colorTexture;
    }

    const RendererBackend& getBackend() const override { return context.getBackend(); }

    const MTLCommandBufferPtr& getCommandBuffer() const override { return commandBuffer; }

    MTLBlitPassDescriptorPtr getUploadPassDescriptor() const override {
        return NS::TransferPtr(MTL::BlitPassDescriptor::alloc()->init());
    }

    const MTLRenderPassDescriptorPtr& getRenderPassDescriptor() const override {
        assert(renderPassDescriptor);
        return renderPassDescriptor;
    }

private:
    Context& context;
    const Size size;
    const gfx::TextureChannelDataType type;
    gfx::Texture2DPtr colorTexture;
    gfx::Texture2DPtr depthTexture;
    gfx::Texture2DPtr stencilTexture;
    MTLCommandBufferPtr commandBuffer;
    MTLRenderPassDescriptorPtr renderPassDescriptor;
    bool noWaitOnSwap = false; // letsgothru/terrain-3d: see setNoWaitOnSwap
};

OffscreenTexture::OffscreenTexture(
    Context& context, const Size size_, const gfx::TextureChannelDataType type, bool depth, bool stencil)
    : gfx::OffscreenTexture(size, std::make_unique<OffscreenTextureResource>(context, size_, type, depth, stencil)) {}

bool OffscreenTexture::isRenderable() {
    assert(false);
    return true;
}

PremultipliedImage OffscreenTexture::readStillImage() {
    return getResource<OffscreenTextureResource>().readStillImage();
}

const gfx::Texture2DPtr& OffscreenTexture::getTexture() {
    return getResource<OffscreenTextureResource>().getTexture();
}

} // namespace mtl
} // namespace mbgl
