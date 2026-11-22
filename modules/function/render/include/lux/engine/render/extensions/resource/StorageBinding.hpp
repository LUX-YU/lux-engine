#pragma once

#include <lux/engine/render/visibility.h>
#include <lux/engine/render/extensions/resource/ReplicaMutation.hpp>

#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <lux/engine/render/extensions/graph/GraphResource.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lux::render::detail
{
    class ResourceUploader;

    // The legacy Resolve signature remains available to the Editor SDK consumer.
    class StorageSubscription;

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
        using Selection = std::unique_ptr<void, void (*)(void*) noexcept>;

        cxx::basic_type_info type;
        Selection (*create)() noexcept;
        RenderResult<std::uint64_t> (*capture)(const void*, void*) noexcept;
        // Optional owner-side revision for program selection. A selectable revision starts at one.
        std::uint64_t (*revision)(const void*) noexcept = nullptr;

        // Capture fills Job-owned reusable memory on the Server owner. Copy every value
        // needed by the producer, including any variable-length data.
        template <
            class Storage, class T, RenderResult<std::uint64_t> (Storage::*Capture)(T&) const noexcept,
            std::uint64_t Storage::*Revision = nullptr
        >
        [[nodiscard]] static constexpr StorageSnapshotOps make() noexcept
        {
            StorageSnapshotOps ops{
                cxx::make_basic_type_info<T>(),
                []() noexcept -> Selection
                {
                    return {new T{}, [](void* output) noexcept { delete static_cast<T*>(output); }};
                },
                [](const void* storage, void* output) noexcept
                {
                    return (static_cast<const Storage*>(storage)->*Capture)(*static_cast<T*>(output));
                }
            };
            if constexpr (Revision != nullptr)
            {
                ops.revision = [](const void* storage) noexcept { return static_cast<const Storage*>(storage)->*Revision; };
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
        using Resolve = RenderResult<
            void> (*)(void*, const VGraphResourceDesc&, const void*, StorageSubscription&, std::span<ResourceInput>) noexcept;
        Resolve resolve = nullptr;
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
