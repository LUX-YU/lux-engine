#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/device/NativeProgramId.hpp>
#include <lux/engine/render/extensions/resource/ResourceIdentity.hpp>
#include <lux/engine/render/visibility.h>
#include <concepts>
#include <functional>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;

    namespace detail
    {
        class DeviceSession;
        class RasterProgramInputs;
    }

    // Embed one in the Resource item that owns these shader variants. Actual draw users have independent uses.
    class LUX_RENDER_PUBLIC ResourceProgramUses final
    {
    public:
        explicit ResourceProgramUses(detail::DeviceSession& device) noexcept : device_(device) {}
        ~ResourceProgramUses() noexcept;
        ResourceProgramUses(const ResourceProgramUses&) = delete;
        ResourceProgramUses& operator=(const ResourceProgramUses&) = delete;

    private:
        friend class ResourceProgramSource;
        void remember(detail::NativeProgramId) noexcept;
        detail::DeviceSession& device_;
        std::vector<detail::NativeProgramId> programs_;
    };

    // A cold publication address. The owner and item generations are resolved again after driver return.
    class LUX_RENDER_PUBLIC ResourceProgramSource final
    {
    public:
        ResourceProgramSource() noexcept = default;

        template <class Storage, auto Lookup, class Tag>
            requires requires(Storage& storage, ResourceId<Tag> id)
            {
                { std::invoke(Lookup, storage, id) } noexcept -> std::same_as<ResourceProgramUses*>;
            }
        [[nodiscard]] static ResourceProgramSource from(ResourceId<Tag> id) noexcept
        {
            using Identity = extension_detail::ResourceIdentity;
            const auto local = Identity::local(id);
            ResourceProgramSource source;
            source.resource_ = Identity::parent(id);
            source.item_ = {local.index, local.gen};
            source.resolve_ = [](const ResourceRegistry& registry, const ResourceProgramSource& address) noexcept
                -> ResourceProgramUses*
            {
                auto* object = borrow(registry, address.resource_, cxx::make_basic_type_info<Storage>());
                if (!object) return nullptr;
                const cxx::SlotKey<Tag> key{address.item_.index, address.item_.gen};
                const auto item = Identity::make<ResourceId<Tag>>(address.resource_, key);
                return std::invoke(Lookup, *static_cast<Storage*>(object), item);
            };
            return source;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return resolve_ != nullptr; }
        [[nodiscard]] ResourceInstanceId resource() const noexcept { return resource_; }

    private:
        friend class RasterProgramBuildContext;
        friend class detail::RasterProgramInputs;

        [[nodiscard]] bool available(const ResourceRegistry&, const detail::DeviceSession&) const noexcept;
        static void* borrow(const ResourceRegistry&, ResourceInstanceId, cxx::basic_type_info) noexcept;
        [[nodiscard]] ResourceProgramUses* find(const ResourceRegistry&, const detail::DeviceSession&) const noexcept;
        void retainAdopted(const ResourceRegistry&, const detail::DeviceSession&, detail::NativeProgramId) const noexcept;
        using Resolve = ResourceProgramUses* (*)(const ResourceRegistry&, const ResourceProgramSource&) noexcept;
        ResourceInstanceId resource_;
        cxx::SlotKey<> item_;
        Resolve resolve_ = nullptr;
    };
} // namespace lux::render
