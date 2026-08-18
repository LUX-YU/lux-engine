#pragma once
// ============================================================================
//  ResourceOperationCommon.hpp — shapes shared across resource op payloads
// ============================================================================

#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>   // TypeId
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>       // RMeshHandle
#include <lux/engine/function/render/Capacity.hpp>

#include <cstdint>
#include <type_traits>

namespace lux::render
{
    namespace type_ids
    {
        /// 共享异步上传 worker 发出的"网格已上传"回复 id。归**共享上传基础设施**
        /// 所有(RenderServer 的 drain 路径发它),特性侧只是复用 —— 所以它和
        /// MeshUploadedReply 一起放在共享资源操作词汇里,而不是留在线协议头:
        /// 留在那里会逼 MeshStackOperation.hpp(L4)为一个常量包含整个 L5 协议头。
        inline constexpr TypeId ReplyMeshUploaded = 10;
    } // namespace type_ids

    /// 共享异步上传路径的网格上传回复。发出者是 RenderServer 的共享上传 worker
    /// (网格 + 纹理传输),不是已下放到特性的 mesh upload op。
    struct MeshUploadedReply
    {
        RMeshHandle handle{};
        uint32_t status{0}; // 0 = success
        /// Present on admission/allocation failure; the previous published
        /// mesh/revision remains valid.
        lux::render::CapacityShortfallWire capacity_shortfall{};
    };
    static_assert(std::is_trivially_copyable_v<MeshUploadedReply>);

    /// Wire payload for "destroy the resource named by `handle`". Every
    /// resource kind's Destroy*Payload is an alias of this template, so they
    /// share one definition while remaining distinct types (each keeps its own
    /// type_id for command dispatch). Trivially copyable whenever HandleT is.
    template <class HandleT>
    struct DestroyResourcePayload
    {
        HandleT handle{};
    };
} // namespace lux::render
