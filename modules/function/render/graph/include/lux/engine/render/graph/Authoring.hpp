#pragma once

#include <cstdint>
#include <lux/engine/render/core/Identity.hpp>

namespace lux::render
{
    struct GraphTextureTag;
    struct GraphBufferTag;
    struct GraphSamplerTag;
    struct GraphPassKeyTag;
    struct GraphShaderKeyTag;
    using GraphTexture = cxx::StrongId<GraphTextureTag, std::uint32_t, 0>;
    using GraphBuffer = cxx::StrongId<GraphBufferTag, std::uint32_t, 0>;
    using GraphSampler = cxx::StrongId<GraphSamplerTag, std::uint64_t, 0>;
    using PassKey = cxx::StrongId<GraphPassKeyTag, std::uint64_t, 0>;
    using ShaderKey = cxx::StrongId<GraphShaderKeyTag, std::uint64_t, 0>;

    enum class EPersistentScope
    {
        NONE,
        RUNTIME,
        SCENE,
        VIEW
    };
    enum class EExecutionScope
    {
        SCENE,
        VIEW,
        TARGET
    };
    enum class EPassKind
    {
        GRAPHICS,
        COMPUTE,
        TRANSFER,
        HOST_READBACK
    };
    enum class ETextureDimension
    {
        D1,
        D2,
        D3,
        CUBE
    };
    enum class ETextureFormat
    {
        R32_FLOAT,
        RGBA32_FLOAT,
        RGBA8_UNORM,
        D32_FLOAT,
        D24_STENCIL8
    };
    enum class EExtentKind
    {
        ABSOLUTE,
        TARGET_RELATIVE,
        DYNAMIC
    };
    enum class ELoadOp
    {
        DISCARD,
        LOAD,
        CLEAR
    };
    enum class EStoreOp
    {
        DISCARD,
        STORE
    };
    enum class EAspect : std::uint32_t
    {
        COLOR = 1,
        DEPTH = 2,
        STENCIL = 4,
        DEPTH_STENCIL = 6
    };

    struct TextureDesc
    {
        ETextureFormat format{ETextureFormat::RGBA32_FLOAT};
        ETextureDimension dimension{ETextureDimension::D2};
        EExtentKind extent_kind{EExtentKind::ABSOLUTE};
        std::uint32_t width{1}, height{1}, depth{1}, mip_count{1}, array_layers{1}, samples{1};

        bool operator==(const TextureDesc&) const noexcept = default;
    };

    struct BufferDesc
    {
        std::uint64_t byte_size{1};
        std::uint32_t alignment{1};

        bool operator==(const BufferDesc&) const noexcept = default;
    };

    struct ImageRange
    {
        EAspect aspect{EAspect::COLOR};
        std::uint32_t base_mip{0}, mip_count{1}, base_layer{0}, layer_count{1};

        bool operator==(const ImageRange&) const noexcept = default;
    };

    struct BufferRange
    {
        std::uint64_t byte_offset{0}, byte_count{1};

        bool operator==(const BufferRange&) const noexcept = default;
    };

    // Value references only. A fallback is an explicit alternative logical resource,
    // never a null descriptor. Bindings and native backing are not authoring objects.
    struct SampledTexture
    {
        GraphTexture texture{};
        ImageRange range{};
        GraphTexture fallback{};
    };

    struct StorageTexture
    {
        GraphTexture texture{};
        ImageRange range{};
        GraphTexture fallback{};
    };

    struct TransferTexture
    {
        GraphTexture texture{};
        ImageRange range{};
        GraphTexture fallback{};
    };

    struct Attachment
    {
        GraphTexture texture{};
        ImageRange range{};
        GraphTexture fallback{};
        ELoadOp load{ELoadOp::CLEAR};
        EStoreOp store{EStoreOp::STORE};
        float clear[4]{};
    };

    struct DepthStencilAttachment
    {
        GraphTexture texture{};
        ImageRange range{EAspect::DEPTH, 0, 1, 0, 1};
        GraphTexture fallback{};
        ELoadOp load{ELoadOp::CLEAR};
        EStoreOp store{EStoreOp::STORE};
        ELoadOp stencil_load{ELoadOp::DISCARD};
        EStoreOp stencil_store{EStoreOp::DISCARD};
        float clear_depth{1.0f};
        std::uint32_t clear_stencil{};
    };

    struct ResolveAttachment
    {
        GraphTexture texture{};
        ImageRange range{};
        GraphTexture fallback{};
    };

    template <typename T> struct UniformBuffer
    {
        GraphBuffer buffer{};
        BufferRange range{};
        GraphBuffer fallback{};
    };

    template <typename T> struct StorageBuffer
    {
        GraphBuffer buffer{};
        BufferRange range{};
        GraphBuffer fallback{};
    };

    struct TransferBuffer
    {
        GraphBuffer buffer{};
        BufferRange range{};
        GraphBuffer fallback{};
    };

    struct VertexBuffer
    {
        GraphBuffer buffer{};
        BufferRange range{};
        GraphBuffer fallback{};
    };

    struct IndexBuffer
    {
        GraphBuffer buffer{};
        BufferRange range{};
        GraphBuffer fallback{};
    };

    struct IndirectBuffer
    {
        GraphBuffer buffer{};
        BufferRange range{};
        GraphBuffer fallback{};
    };

    struct SamplerHandle
    {
        GraphSampler sampler{};
    };
} // namespace lux::render
