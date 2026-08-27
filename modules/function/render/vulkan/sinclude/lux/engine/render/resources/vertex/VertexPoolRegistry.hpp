#pragma once
/**
 * @file VertexPoolRegistry.hpp
 * @brief Bindless descriptor set for the IVertexSource array.
 *
 * Stage R1.4 of the render-refactor.
 *
 * Responsibility:
 *
 *   - Own the descriptor set for EDescriptorSetSlot::VertexPool (set 7,
 *     defined in DescriptorSetLayoutContract.hpp). The set has a single
 *     binding (binding 0) of type STORAGE_BUFFER with descriptorCount =
 *     kVertexPoolMaxCount, marked PARTIALLY_BOUND + UPDATE_AFTER_BIND.
 *
 *   - Allocate stable pool ids to registered IVertexSources. The id is
 *     the slot index in the bindless array; once assigned, it appears in
 *     every VertexSourceHandle the source hands out, and shaders index
 *     into the array with it.
 *
 *   - Write descriptors when sources are registered / unregistered.
 *     UPDATE_AFTER_BIND lets us update freely without waiting for a
 *     fence; the validation layer is happy because the set is created
 *     with UPDATE_AFTER_BIND_POOL.
 *
 * Pipeline integration (R2+):
 *
 *   Pipelines that want to read vertices via the bindless array build
 *   their pipeline layout with GeneralDescriptorSetLayout's set 7
 *   layout, and at recordings bind:
 *       vkCmdBindDescriptorSets(... set = 7, vertex_pool_registry.descriptorSet() ...)
 *
 *   Shaders declare:
 *       layout(set = 7, binding = 0, std430) readonly buffer VertexPool {
 *           Vertex data[];
 *       } vertex_pools[];
 *       Vertex v = vertex_pools[instance.vertex_pool_id].data[base + gl_VertexIndex];
 *
 * No barrier choreography needed at this layer — RG declares producer
 * (compute write to transient pool) → consumer (vertex shader read)
 * dependencies and inserts barriers automatically.
 */

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/render/core/RenderErrorSink.hpp>
#include <lux/engine/function/render/client/core/RenderErrorList.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/descriptor/DomainWriteTarget.hpp>
#include <lux/engine/render/resources/vertex/IVertexSource.hpp>
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    class DeviceContext;
    class GeneralDescriptorSetLayout;
    class SceneDescriptorArena;

    class LUX_FUNCTION_PUBLIC VertexPoolRegistry
    {
    public:
        VertexPoolRegistry() = default;
        ~VertexPoolRegistry();

        VertexPoolRegistry(const VertexPoolRegistry&) = delete;
        VertexPoolRegistry& operator=(const VertexPoolRegistry&) = delete;

        /// Initialize. Registers the set-7 layout with DescriptorService
        /// (mirroring GeneralDescriptorSetLayout's layout so the resulting
        /// VkDescriptorSet can be bound by pipelines built with either),
        /// allocates the descriptor set, and zeros all slots.
        ///
        /// Returns false on DescriptorService failure.
        [[nodiscard]] bool
        init(DeviceContext& device_ctx, DescriptorService& descriptor_svc, SceneDescriptorArena& arena);

        void shutdown();

        /// Register a source. Picks the lowest free slot, assigns it as the
        /// source's bindless pool id via source.setBindlessPoolId(), writes
        /// the descriptor, and returns the assigned id. Returns ~0u if
        /// the registry is full (kVertexPoolMaxCount slots occupied).
        ///
        /// The source must remain alive (and its buffer() handle must
        /// remain valid) until unregisterSource is called.
        std::uint32_t registerSource(IVertexSource& source);

        /// Release the slot. The descriptor write is NOT rewritten — the
        /// slot is left as-is (UPDATE_AFTER_BIND + PARTIALLY_BOUND means
        /// stale descriptors are valid as long as no shader indexes into
        /// them). Callers must ensure no live instance references the
        /// freed pool id.
        void unregisterSource(std::uint32_t pool_id);

        /// Re-write the descriptor for an already-registered slot — e.g. after
        /// the source's backing buffer was (re)created or grew. Safe at any time
        /// (UPDATE_AFTER_BIND). No-op if @p pool_id isn't currently registered.
        /// Used by producers whose buffer becomes valid / reallocates after
        /// registration (e.g. the global VBO as a static input pool).
        void refreshSource(std::uint32_t pool_id);

        /// True if @p pool_id refers to a currently-registered source.
        [[nodiscard]] bool isRegistered(std::uint32_t pool_id) const noexcept;

        /// 池是否已就绪。此前调用方拿 descriptorSet() 非空当这个判据用 ——
        /// per-set 实例退休后改问这个,语义也更直白。
        [[nodiscard]] bool isInitialized() const noexcept
        {
            return initialized_;
        }

        /// 设置域写目标(句柄组 + 域内偏移)。后域集是唯一写目标。
        [[nodiscard]] Expected<void>
        setDomainWriteTarget(std::span<const VkDescriptorSet> sets, uint32_t binding_offset);

        /// The VkDescriptorSetLayout — same handle as
        /// GeneralDescriptorSetLayout::getLayout(EDescriptorSetSlot::VertexPool)
        /// when the registry is initialized against the same DescriptorService.
        [[nodiscard]] VkDescriptorSetLayout descriptorSetLayout() const noexcept;

    public:
        /// 自发上报的去处(非拥有)。注册表满是每帧的资源仲裁结果 —— 没有调用方
        /// 可以处置,但也不能不可见。由拥有它的 feature 在 attach 期装上。
        void setErrorSink(RenderErrorSink* sink) noexcept
        {
            error_sink_ = sink;
        }

    private:
        RenderErrorSink* error_sink_{nullptr};
        DeviceContext* device_ctx_{nullptr};
        DescriptorService* descriptor_svc_{nullptr};
        DescriptorLayoutId layout_id_{kInvalidDescriptorLayoutId};

        /// 域写目标:逐 slice 句柄 + 域内偏移(绑成一体,见 DomainWriteTarget)。
        DomainWriteTarget domain_{};

        /// One pointer per slot. nullptr = free.
        std::array<IVertexSource*, kVertexPoolMaxCount> slots_{};

        bool initialized_{false};

        void writeDescriptor(std::uint32_t pool_id, IVertexSource& source);
    };

} // namespace lux::render
