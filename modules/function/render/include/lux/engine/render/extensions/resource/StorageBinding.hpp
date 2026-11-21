#pragma once

#include <lux/engine/render/visibility.h>

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

    // Intrusive, non-owning change relationship. The consumer embeds this node in its actual
    // record; the Storage keeps only the list head. Heads and linked consumers have stable addresses.
    class LUX_RENDER_PUBLIC StorageSubscription final
    {
    public:
        StorageSubscription() noexcept = default;

        ~StorageSubscription() noexcept
        {
            reset();
        }

        StorageSubscription(const StorageSubscription&) = delete;
        StorageSubscription& operator=(const StorageSubscription&) = delete;

        StorageSubscription(StorageSubscription&& other) noexcept
        {
            move(other);
        }

        StorageSubscription& operator=(StorageSubscription&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                move(other);
            }
            return *this;
        }

        void* receiver = nullptr;
        // UINT32_MAX denotes structural disconnection of all physical slices.
        void (*changed)(void*, std::uint32_t slot, const ResourceInput&) noexcept = nullptr;

        void attach(StorageSubscription*& head) noexcept
        {
            reset();
            next_ = head;
            previous_ = &head;
            if (head)
            {
                head->previous_ = &next_;
            }
            head = this;
        }

        void reset() noexcept
        {
            if (previous_)
            {
                *previous_ = next_;
                if (next_)
                {
                    next_->previous_ = previous_;
                }
                previous_ = nullptr;
                next_ = nullptr;
            }
        }

        static void publish(StorageSubscription* head, std::uint32_t slot, const ResourceInput& resource) noexcept
        {
            while (head)
            {
                auto* next = head->next_;
                head->changed(head->receiver, slot, resource);
                head = next;
            }
        }

        static void disconnect(StorageSubscription*& head, const ResourceInput& resource = {}) noexcept
        {
            while (head)
            {
                auto* node = head;
                node->reset();
                node->changed(node->receiver, UINT32_MAX, resource);
            }
        }

    private:
        void move(StorageSubscription& other) noexcept
        {
            receiver = other.receiver;
            changed = other.changed;
            next_ = std::exchange(other.next_, nullptr);
            previous_ = std::exchange(other.previous_, nullptr);
            if (previous_)
            {
                *previous_ = this;
            }
            if (next_)
            {
                next_->previous_ = &next_;
            }
        }

        StorageSubscription* next_ = nullptr;
        StorageSubscription** previous_ = nullptr;
    };

    struct StorageGraphImport final
    {
        VGraphResourceDesc description;
        std::vector<VGraphRange> initial_ranges;
        EGraphUse initial_use = EGraphUse::UNSPECIFIED;
        // When set, the graph may only read the projection in this use.
        std::optional<EGraphUse> read_use{};
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

    struct ReplicaFrame final
    {
        std::uint32_t slot;
        std::uint64_t epoch;
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
            void*, std::uint32_t, const VGraphResourceDesc&, StorageSubscription&, std::span<ResourceInput>
        ) noexcept;
        ResolveGraphImport resolveGraphImport = nullptr;
        const StorageSnapshotOps* cpu_snapshot = nullptr;

        // Each attempted preparation settles once, including zero-copy and partially staged failures.
        // A nonzero accepted_copy is the native receipt for this participant's staged ranges.
        void (*settleFrame)(void*, const ReplicaSettlement&) noexcept = nullptr;
    };

} // namespace lux::render::detail
