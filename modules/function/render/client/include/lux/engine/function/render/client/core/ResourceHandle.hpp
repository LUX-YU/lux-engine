#pragma once
/**
 * @file ResourceHandle.hpp
 * @brief Thin, generational handles for render-thread resource types.
 *
 * Each handle is a tagged SlotKey (index + generation).  Payload data
 * that was previously carried on the handle (family, shading_model,
 * set_binding, asset_id) now lives inside the owning *Resources class.
 *
 * NOTE: This file defines the *render-thread-internal* handles (MeshHandle,
 * TextureHandle, MaterialHandle, LightHandle).  These use different tag
 * types from the *cross-thread* handles in RenderResourceHandle.hpp
 * (RMeshHandle, etc.) — the two sets are intentionally distinct types
 * to enforce thread-ownership boundaries at compile time.
 */
#include <lux/cxx/container/SlotMap.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace lux::render
{
    /**
     * @brief A strongly-typed handle wrapping a uint32_t index.
     *
     * Two handles with different Tag types are distinct types and cannot
     * be implicitly converted or compared.
     */
    template <typename Tag> struct TypedHandle
    {
        uint32_t index{std::numeric_limits<uint32_t>::max()};

        constexpr TypedHandle() noexcept = default;
        constexpr explicit TypedHandle(uint32_t idx) noexcept : index(idx)
        {
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return index != std::numeric_limits<uint32_t>::max();
        }

        constexpr auto operator<=>(const TypedHandle&) const noexcept = default;

        struct Hash
        {
            std::size_t operator()(TypedHandle h) const noexcept
            {
                return std::hash<uint32_t>{}(h.index);
            }
        };
    };

    /**
     * @brief A strongly-typed generational handle alias.
     */
    template <typename Tag> using TypedSlotHandle = lux::cxx::SlotKey<Tag>;

    // ── Tag types for compile-time handle discrimination ──
    struct MeshHandleTag
    {
    };
    struct TextureHandleTag
    {
    };
    struct MaterialHandleTag
    {
    };
    struct LightHandleTag
    {
    };
    struct ShaderHandleTag
    {
    };
    /// 轨迹的标识。此前定义在 TrajectoryOperation.hpp 里,导致存储层
    /// (TrajectoryResources)为了一个句柄就要包含整个操作头 —— 而那个头还带着
    /// 线协议载荷与 RenderFrameSession/RenderRequest 的前置声明,是自下而上的依赖。
    /// 标识类型本就属于基础层,与 MaterialHandle 等同处。
    struct TrajectoryHandleTag
    {
    };

    using MeshHandle = lux::cxx::SlotKey<MeshHandleTag>;
    using TextureHandle = lux::cxx::SlotKey<TextureHandleTag>;
    using MaterialHandle = lux::cxx::SlotKey<MaterialHandleTag>;
    using LightHandle = lux::cxx::SlotKey<LightHandleTag>;
    using ShaderHandle = lux::cxx::SlotKey<ShaderHandleTag>;
    using TrajectoryHandle = lux::cxx::SlotKey<TrajectoryHandleTag>;
}
