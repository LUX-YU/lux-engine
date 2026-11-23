#pragma once

#include <lux/engine/render/visibility.h>
#include <lux/engine/render/resource/ReplicaMutation.hpp>

#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/ProgramInputRevision.hpp>
#include <lux/engine/render/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/resource/ResourceBinding.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lux::render::detail
{
    class ResourceUploader;

    struct StorageGraphImport final
    {
        VGraphResourceDesc description;
        std::vector<VGraphRange> initial_ranges;
        EGraphUse initial_use = EGraphUse::UNSPECIFIED;
        // When set, the graph may only read the projection in this use.
        std::optional<EGraphUse> read_use{};
        // The owner can grant an exclusive writer for bytes within this Buffer projection.
        bool final_buffer_output = false;
        // The owner supplies concurrent native images in their declared read layout and physical queue frontiers.
        bool concurrent_image_read = false;
        [[nodiscard]] static StorageGraphImport bufferOutput(std::uint64_t bytes, EGraphUse boundary) noexcept
        {
            return {GraphBufferDesc{bytes}, {}, boundary, {}, true};
        }
        bool operator==(const StorageGraphImport&) const noexcept = default;
    };

    struct StorageSnapshotOps final
    {
        cxx::basic_type_info type;
        RenderResult<RevisionedValue<const void*>> (*select)(void*) noexcept;
        ProgramInputRevision& (*program_input)(void*) noexcept = nullptr;

        // The Resource keeps its selected value valid throughout the synchronous preparation on the device owner.
        template <class Storage, class T, auto Select, ProgramInputRevision Storage::*Revision = nullptr>
            requires requires(Storage& storage) {
                { std::invoke(Select, storage) } noexcept -> std::same_as<RenderResult<SnapshotValue<T>>>;
            }
        [[nodiscard]] static constexpr StorageSnapshotOps make() noexcept
        {
            StorageSnapshotOps ops{
                cxx::make_basic_type_info<T>(),
                [](void* storage) noexcept -> RenderResult<RevisionedValue<const void*>>
                {
                    auto selected = std::invoke(Select, *static_cast<Storage*>(storage));
                    if (!selected) return cxx::unexpected(selected.error());
                    return RevisionedValue<const void*>{std::addressof(selected->value.get()), selected->revision};
                }
            };
            if constexpr (Revision != nullptr)
            {
                ops.program_input = [](void* storage) noexcept -> ProgramInputRevision&
                {
                    return std::invoke(Revision, *static_cast<Storage*>(storage));
                };
            }
            return ops;
        }
    };

    struct ReplicaCopyRange final
    {
        std::size_t first, count;
    };

    struct ReplicaSettlement final
    {
        ReplicaFrame frame;
        ReplicaCopyRange copies;
        SubmissionPoint accepted_copy;
        RenderError error;
    };

    struct StorageBindingOps final
    {
        // Begin one preparation attempt on the device owner. False requests another attempt after progress.
        using PrepareFrame = RenderResult<bool> (*)(void*, ReplicaFrame, ResourceUploader&) noexcept;
        PrepareFrame prepareFrame = nullptr;
        // Cold borrow. The producer advances it before any attempt that may dirty a replica,
        // including a failed upload which leaves repair work. No callback is needed to read it.
        const std::uint64_t* (*preparationRevision)(const void*) noexcept = nullptr;

        // A projection identifies one complete graph binding of this owner. Distinct projections must keep
        // disjoint backing through replacements. Shape, initialized ranges and boundary use remain stable.
        using DescribeGraphImport = RenderResult<StorageGraphImport> (*)(const void*, std::uint32_t) noexcept;
        DescribeGraphImport describeGraphImport = nullptr;
        using ResolveGraphImport = RenderResult<void> (*)(
            void*, std::uint32_t, const VGraphResourceDesc&, std::span<ResourceInput>
        ) noexcept;
        ResolveGraphImport resolveGraphImport = nullptr;
        const StorageSnapshotOps* cpu_snapshot = nullptr;

        // Each attempted preparation settles once, including zero-copy and partially staged failures.
        // A nonzero accepted_copy is the native receipt for this participant's staged ranges.
        void (*settleFrame)(void*, const ReplicaSettlement&) noexcept = nullptr;
    };

} // namespace lux::render::detail
