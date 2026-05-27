#pragma once

#include <mbgl/util/size.hpp>

#include <memory>
#include <cassert>

namespace mbgl {
namespace gfx {

class RenderableResource {
protected:
    explicit RenderableResource() = default;

public:
    virtual ~RenderableResource() = default;
    RenderableResource(const RenderableResource&) = delete;
    RenderableResource& operator=(const RenderableResource&) = delete;

    virtual void bind() = 0;

    // letsgothru/terrain-3d: for intermediate render targets (terrain drape RTTs)
    // sampled only on-GPU by a later pass in the same frame, the command queue
    // already orders this before that pass, so the per-swap CPU waitUntilCompleted
    // is pure stall (one per visible tile). Set true to skip it. Targets read back
    // to the CPU (headless readStillImage) must keep waiting. Default no-op; only
    // backends with such a stall (Metal) honour it.
    virtual void setNoWaitOnSwap(bool) {}
};

class Renderable {
public:
    enum class SwapBehaviour {
        NoFlush,
        Flush
    };

protected:
    Renderable(const Size size_, std::unique_ptr<RenderableResource> resource_)
        : size(size_),
          resource(std::move(resource_)) {}
    virtual ~Renderable() = default;

public:
    Size getSize() const { return size; }

    template <typename T>
    T& getResource() const {
        assert(resource);
        return static_cast<T&>(*resource);
    }

    bool hasResource() const { return resource != nullptr; }

    void setResource(std::unique_ptr<RenderableResource> resource_) { resource = std::move(resource_); }

    virtual void wait() {}

    bool operator!=(const Renderable& other) const { return resource.get() != other.resource.get(); }

protected:
    Size size;
    std::unique_ptr<RenderableResource> resource;
};

} // namespace gfx
} // namespace mbgl
