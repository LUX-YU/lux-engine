#pragma once

#include <cstdint>
#include <limits>
#include <lux/engine/description/Image.hpp>
#include <lux/engine/render/core/Identity.hpp>
#include <string>

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

    struct GraphResourceKeyTag;
    using GraphResourceKey = cxx::StrongId<GraphResourceKeyTag, std::uint64_t, 0>;

    [[nodiscard]] constexpr PassKey passKey(std::string_view name) noexcept
    {
        return PassKey{name.empty() ? 0 : cxx::Fnv1a64::hash(name)};
    }

    [[nodiscard]] constexpr GraphResourceKey graphResourceKey(std::string_view name) noexcept
    {
        return GraphResourceKey{name.empty() ? 0 : cxx::Fnv1a64::hash(name)};
    }

    // A cold reference to a logical Shader asset variant, never a compiled/native pipeline ID.
    struct ShaderReference
    {
        std::string_view asset;
        std::string_view variant;

        [[nodiscard]] bool isValid() const noexcept
        {
            return !asset.empty() && !variant.empty() && asset.find('#') == std::string_view::npos &&
                   variant.find('#') == std::string_view::npos;
        }

        [[nodiscard]] std::string canonicalName() const noexcept
        {
            return isValid() ? std::string(asset) + "#" + std::string(variant) : std::string{};
        }
    };

    [[nodiscard]] inline ShaderKey shaderKey(ShaderReference reference) noexcept
    {
        const auto name = reference.canonicalName();
        return ShaderKey{name.empty() ? 0 : cxx::Fnv1a64::hash(name)};
    }

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
        rdesc::ETextureFormat format{rdesc::ETextureFormat::RGBA32_SFLOAT};
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

    inline constexpr auto kRemainingBytes = std::numeric_limits<std::uint64_t>::max();
    inline constexpr auto kRemainingSubresources = std::numeric_limits<std::uint32_t>::max();

    struct BufferRange
    {
        std::uint64_t byte_offset{0}, byte_count{kRemainingBytes};

        bool operator==(const BufferRange&) const noexcept = default;
    };

    // Value references only. A fallback is an explicit alternative logical resource,
    // never a null descriptor. Bindings and native backing are not authoring objects.
    struct SampledTexture
    {
        GraphTexture texture{};
        // Shader LOD/layers are relative to this native view. Default covers the complete backing.
        ImageRange range{EAspect::COLOR, 0, kRemainingSubresources, 0, kRemainingSubresources};
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
        BufferRange range{0, sizeof(T)};
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
