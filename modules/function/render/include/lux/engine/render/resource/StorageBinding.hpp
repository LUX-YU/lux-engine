#pragma once

#include <lux/engine/render/visibility.h>
#include <lux/engine/render/graph/DrawList.hpp>

#include <lux/engine/description/ShaderBlock.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/resource/ResourceBinding.hpp>
#include <lux/engine/render/resource/ResourceProjection.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lux::render { class RasterProgramSet; }

namespace lux::render::detail
{
    // A Resource selects its replica only for an already admitted device frame.
    struct ReplicaFrame final
    {
        std::uint32_t slot;
        std::uint64_t epoch;
    };

    class ResourceUploader;

    struct ResourceViewOps final
    {
        cxx::basic_type_info type;
        RenderResult<RevisionedValue<const void*>> (*select)(void*) noexcept;

        // The Resource keeps its selected value valid throughout the synchronous preparation on the device owner.
        template <class Storage, class T, auto Select>
            requires requires(Storage& storage) {
                { std::invoke(Select, storage) } noexcept -> std::same_as<RenderResult<ResourceViewValue<T>>>;
            }
        [[nodiscard]] static constexpr ResourceViewOps make() noexcept
        {
            return {
                cxx::make_basic_type_info<T>(),
                [](void* storage) noexcept -> RenderResult<RevisionedValue<const void*>>
                {
                    auto selected = std::invoke(Select, *static_cast<Storage*>(storage));
                    if (!selected) return cxx::unexpected(selected.error());
                    return RevisionedValue<const void*>{std::addressof(selected->value.get()), selected->revision};
                }
            };
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
        // A projection identifies one complete graph binding of this owner. Distinct projections must keep
        // disjoint backing through replacements. Shape, initialized ranges and boundary use remain stable.
        using DescribeGraphImport = RenderResult<ResourceProjection> (*)(const void*, std::uint32_t) noexcept;
        DescribeGraphImport describeGraphImport = nullptr;
        using ResolveGraphImport = RenderResult<void> (*)(
            void*, std::uint32_t, const VGraphResourceDesc&, std::span<ResourceInput>
        ) noexcept;
        ResolveGraphImport resolveGraphImport = nullptr;
        std::optional<ResourceViewOps> cpu_view;

        // The resource owns resident state for each replica. A receipt settles only the selected changes.
        // Each attempted preparation settles once, including zero-copy and partially staged failures.
        // A nonzero accepted_copy is the native receipt for this participant's staged ranges.
        void (*settleFrame)(void*, const ReplicaSettlement&) noexcept = nullptr;

        // Called once per selected Resource on the device owner before upload or output acquisition can suspend.
        // PER_RUN consumes its pending payload. A persistent Resource may select a stable value for later readers.
        void (*selectRun)(void*, ReplicaFrame) noexcept = nullptr;
        bool (*hasRequiredInput)(const void*) noexcept = nullptr;

        // A missing slot previews pending data on the owner before admission. selectRun fixes that same
        // list for the admitted slot. Both views are synchronous borrows.
        using ReadDrawList = RasterStreamData (*)(const void*, std::optional<std::uint32_t> slot) noexcept;
        ReadDrawList readDrawList = nullptr;

        // Cold link to one logical byte count per FIF slot. The resource owns these stable addresses
        // and sets the admitted slot during preparation. An unsupported field returns an empty span.
        std::span<const std::uint64_t> (*bindBufferExtent)(const void*, std::uint32_t field) noexcept = nullptr;

        // Cold projection. The set has a stable address for the lifetime of this Resource instance.
        const RasterProgramSet& (*rasterPrograms)(const void*) noexcept = nullptr;
    };

} // namespace lux::render::detail
