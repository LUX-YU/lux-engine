#pragma once

#include <concepts>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/runtime/Objects.hpp>
#include <lux/engine/render/vulkan/transfer/Submission.hpp>
#include <utility>

namespace lux::render::vulkan
{
    // Only Backend's validated attachment resolver constructs these sharing namespaces.
    // The enclosing Session scopes Global and all local SlotKeys across Runtime owners.
    struct ResourceScopeKey
    {
        EResourceScope scope{EResourceScope::GLOBAL};
        RenderSceneId scene;
        RenderViewHandle view;
        RenderEffectId effect;
        RenderTargetId target;
        bool operator==(const ResourceScopeKey&) const noexcept = default;
    };

    template <typename F, typename T>
    concept ResourceFactoryFor = requires(F& factory, const T& existing) {
        { factory() } -> std::same_as<RenderResult<std::unique_ptr<T>>>;
        { factory.isCompatible(existing) } -> std::same_as<bool>;
    };

    inline RenderError resourceError(RenderError error) noexcept
    {
        const bool is_oom = error.type == kNativeFailure &&
                            (error.args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_HOST_MEMORY) ||
                             error.args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        if (is_oom)
        {
            std::terminate();
        }
        return error;
    }

    struct ScopedResourceTag;
    using ScopedResourceId = cxx::SlotKey<ScopedResourceTag>;

    namespace detail
    {
        struct ScopedResourceEntry
        {
            cxx::basic_type_info type;
            ResourceScopeKey scope;
            std::string key;
            ScopedResourceId id;
            std::size_t leases{};
            std::optional<SubmissionTicket> ready;
            std::array<std::optional<SubmissionTicket>, 3> last_use;
            virtual ~ScopedResourceEntry() noexcept = default;
        };

        template <typename T> struct TypedResourceEntry final : ScopedResourceEntry
        {
            std::unique_ptr<T> value;
        };
    } // namespace detail

    // CPU use right only. Physical ownership stays in the Session's ResourceDomain.
    template <typename T> class ResourceLease
    {
    public:
        ResourceLease() noexcept = default;

        ~ResourceLease() noexcept
        {
            release();
        }

        ResourceLease(const ResourceLease&) = delete;
        ResourceLease& operator=(const ResourceLease&) = delete;

        ResourceLease(ResourceLease&& other) noexcept : entry_(std::exchange(other.entry_, nullptr)) {}

        ResourceLease& operator=(ResourceLease&& other) noexcept
        {
            if (this != &other)
            {
                release();
                entry_ = std::exchange(other.entry_, nullptr);
            }
            return *this;
        }

        [[nodiscard]] std::optional<SubmissionTicket> ready() const noexcept
        {
            return entry_->ready;
        }

        [[nodiscard]] std::optional<SubmissionTicket> lastGraphicsUse() const noexcept
        {
            return entry_->last_use[0] ? entry_->last_use[0] : entry_->ready;
        }

        [[nodiscard]] T& get() const noexcept
        {
            return *entry_->value;
        }

        [[nodiscard]] ScopedResourceId id() const noexcept
        {
            return entry_->id;
        }

        [[nodiscard]] std::size_t useCount() const noexcept
        {
            return entry_->leases;
        }

        // Record actual queue admissions even when executeGraph returns a partial-submit error.
        // The small fixed queue set is the existing DeviceContext set, never another timeline.
        void retainAccepted(const std::array<SubmissionQueue*, 3>& queues) noexcept
        {
            for (std::size_t i = 0; i < queues.size(); ++i)
            {
                entry_->last_use[i] = queues[i]->lastSubmitted();
            }
        }

    private:
        friend class ResourceDomain;

        explicit ResourceLease(detail::TypedResourceEntry<T>& entry) noexcept : entry_(&entry)
        {
            ++entry_->leases;
        }

        void release() noexcept
        {
            if (entry_)
            {
                --entry_->leases;
                entry_ = nullptr;
            }
        }

        detail::TypedResourceEntry<T>* entry_{};
    };

    // P2-A's typed entry store, internalized and keyed by Scope/Type/canonical Key.
    // There is ONE store per Session; no Domain tree or parallel ScopedResources owner.
    // Retention is OnLastDetach: collect at cold topology changes, defer pending backing
    // until a later cold collection or Session shutdown. No frame-loop registry walk.
    class ResourceDomain
    {
    public:
        ResourceDomain() noexcept = default;
        ResourceDomain(const ResourceDomain&) = delete;
        ResourceDomain& operator=(const ResourceDomain&) = delete;

        ~ResourceDomain() noexcept
        {
            for (const auto& entry : entries_)
            {
                if (entry->leases != 0)
                {
                    std::terminate();
                }
                for (const auto& ticket : entry->last_use)
                {
                    if (!ticket)
                    {
                        continue;
                    }
                    auto& queue = const_cast<SubmissionQueue&>(ticket->owner());
                    auto done = queue.wait(*ticket, UINT64_MAX);
                    if ((!done || !*done) && !queue.deviceLost())
                    {
                        std::terminate();
                    }
                }
            }
        }

        template <typename T, typename F>
            requires ResourceFactoryFor<F, T>
        [[nodiscard]] RenderResult<ResourceLease<T>> getOrCreate(
            ResourceScopeKey scope,
            std::string_view key,
            F&& factory
        ) noexcept
        {
            if (key.empty())
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            for (const auto& entry : entries_)
            {
                const bool same_type =
                    entry->type.hash() == cxx::type_hash<T>() && entry->type.name() == cxx::type_name<T>();
                if (same_type && entry->scope == scope && entry->key == key)
                {
                    auto& typed = static_cast<detail::TypedResourceEntry<T>&>(*entry);
                    if (!factory.isCompatible(*typed.value))
                    {
                        return cxx::unexpected(RenderError{kDefinitionMismatch});
                    }
                    return ResourceLease<T>{typed};
                }
            }
            auto value = factory();
            if (!value)
            {
                return cxx::unexpected(resourceError(value.error()));
            }
            if (!*value)
            {
                return cxx::unexpected(RenderError{kInvalidArgument});
            }
            auto entry = std::make_unique<detail::TypedResourceEntry<T>>();
            entry->type = cxx::make_basic_type_info<T>();
            entry->scope = scope;
            entry->key = key;
            entry->value = std::move(*value);
            if constexpr (requires { factory.readyTicket(); })
            {
                entry->ready = factory.readyTicket();
                entry->last_use[0] = entry->ready;
            }
            auto* stable = entry.get();
            stable->id = entries_.emplace(std::move(entry));
            return ResourceLease<T>{*stable};
        }

        [[nodiscard]] RenderResult<void> collect() noexcept
        {
            // Erase by saved generational ID, restarting after dense compaction (cold only).
            for (std::size_t i = 0; i < entries_.size();)
            {
                auto& entry = *(entries_.begin() + i)->get();
                bool ready = entry.leases == 0;
                if (ready)
                {
                    for (const auto& ticket : entry.last_use)
                    {
                        if (!ticket)
                        {
                            continue;
                        }
                        auto& queue = const_cast<SubmissionQueue&>(ticket->owner());
                        auto done = queue.wait(*ticket, 0);
                        if (!done && !queue.deviceLost())
                        {
                            return cxx::unexpected(resourceError(done.error()));
                        }
                        ready = ready && (queue.deviceLost() || (done && *done));
                    }
                }
                if (ready)
                {
                    const auto id = entry.id;
                    entries_.erase(id);
                }
                else
                {
                    ++i;
                }
            }
            return {};
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return entries_.size();
        }

    private:
        cxx::SlotMap<std::unique_ptr<detail::ScopedResourceEntry>, ScopedResourceTag> entries_;
    };
} // namespace lux::render::vulkan
