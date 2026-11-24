#pragma once

#include <concepts>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/resource/ResourceCreateContext.hpp>
#include <lux/engine/render/resource/ResourceDefinition.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <memory>
#include <optional>
#include <utility>

namespace lux::render
{
    using StorageOwner = std::unique_ptr<void, void (*)(void*) noexcept>;

    // Immutable implementation metadata. It has no Resource declaration or global identity.
    struct StorageMeta final
    {
        RenderResult<StorageOwner> (*create)(ResourceCreateContext&, ResourceInstanceId, const void*) noexcept = nullptr;
        std::optional<detail::StorageBindingOps> binding_ops;
        cxx::basic_type_info config_type;
        cxx::basic_type_info implementation_type;

        [[nodiscard]] const detail::StorageBindingOps* bindings() const noexcept
        {
            return binding_ops ? &*binding_ops : nullptr;
        }
    };

    struct ResourceRegistration;
    template <class Declaration, class Implementation, auto Create, auto Update = nullptr>
    [[nodiscard]] ResourceRegistration
    makeResourceRegistration(const detail::StorageBindingOps* bindings = nullptr) noexcept;

    struct ResourceRegistration final
    {
        using ApplyFrame = RenderResult<void> (*)(void*, const FrameData&, std::size_t) noexcept;

        ResourceRegistration(const ResourceDefinition* definition, StorageMeta metadata) noexcept
            : descriptor(definition), storage_(std::move(metadata)) {}

        ResourceRegistration(ResourceRegistration&&) noexcept = default;
        ResourceRegistration& operator=(ResourceRegistration&&) noexcept = default;

        [[nodiscard]] const StorageMeta& storage() const & noexcept { return storage_; }
        const StorageMeta& storage() const && = delete;

        const ResourceDefinition* descriptor;
        [[nodiscard]] std::span<const FrameChannel> channels() const & noexcept { return frame_channels_; }
        std::span<const FrameChannel> channels() const && = delete;
        [[nodiscard]] ApplyFrame applyFrame() const noexcept { return apply_frame_; }

    private:
        friend class ResourceRegistry;
        template <class Declaration, class Implementation, auto Create, auto Update>
        friend ResourceRegistration makeResourceRegistration(const detail::StorageBindingOps*) noexcept;
        StorageMeta storage_;
        std::span<const FrameChannel> frame_channels_{};
        ApplyFrame apply_frame_ = nullptr;
    };

    template <class Config, class Implementation, auto Create>
    [[nodiscard]] StorageMeta
    makeStorageMeta(const detail::StorageBindingOps* binding_ops) noexcept
    {
        StorageMeta metadata;
        if (binding_ops) metadata.binding_ops = *binding_ops;
        if constexpr (!std::is_void_v<Config>) metadata.config_type = cxx::make_basic_type_info<Config>();
        metadata.implementation_type = cxx::make_basic_type_info<Implementation>();
        metadata.create = [](ResourceCreateContext& dependencies, ResourceInstanceId id, const void* config)
            noexcept -> RenderResult<StorageOwner>
        {
            auto candidate = [&]() noexcept
            {
                if constexpr (!std::is_void_v<Config>)
                {
                    using Factory = RenderResult<std::unique_ptr<Implementation>> (*)(
                        ResourceCreateContext&, ResourceInstanceId, const Config&
                    ) noexcept;
                    static_assert(std::same_as<decltype(Create), Factory>);
                    return Create(dependencies, id, *static_cast<const Config*>(config));
                }
                else
                {
                    using Factory = RenderResult<std::unique_ptr<Implementation>> (*)(
                        ResourceCreateContext&, ResourceInstanceId
                    ) noexcept;
                    static_assert(std::same_as<decltype(Create), Factory>);
                    return Create(dependencies, id);
                }
            }();
            if (!candidate)
            {
                return cxx::unexpected(candidate.error());
            }
            return StorageOwner{
                candidate->release(),
                [](void* value) noexcept { delete static_cast<Implementation*>(value); }
            };
        };
        return metadata;
    }

    namespace frame_detail
    {
        template <class T> std::span<const T> readColumn(const FrameData& packet, std::size_t index) noexcept
        {
            const auto column = packet.column(index);
            return {static_cast<const T*>(column.data), column.count};
        }
    }

    template <class Declaration, class Implementation, auto Create, auto Update>
    [[nodiscard]] ResourceRegistration makeResourceRegistration(const detail::StorageBindingOps* bindings) noexcept
    {
        ResourceRegistration result{
            &Declaration::descriptor, makeStorageMeta<typename Declaration::Config, Implementation, Create>(bindings)
        };
        if constexpr (Update != nullptr)
        {
            result.frame_channels_ = frameChannels<Declaration>();
            result.apply_frame_ = []<class... T>(std::type_identity<std::tuple<T...>>) noexcept
            {
                using Apply = RenderResult<void> (*)(Implementation&, std::span<const T>...) noexcept;
                using Member = RenderResult<void> (Implementation::*)(std::span<const T>...) noexcept;
                static_assert(std::same_as<decltype(Update), Apply> || std::same_as<decltype(Update), Member>);
                constexpr auto explicit_parameters = sizeof...(T) + std::same_as<decltype(Update), Apply>;
                static_assert(explicit_parameters <= 4, "Group related update fields into typed records");
                return +[](void* owner, const FrameData& packet, std::size_t first) noexcept
                {
                    return [&]<std::size_t... I>(std::index_sequence<I...>) noexcept
                    {
                        return std::invoke(
                            Update, *static_cast<Implementation*>(owner), frame_detail::readColumn<T>(packet, first + I)...
                        );
                    }(std::index_sequence_for<T...>{});
                };
            }(std::type_identity<typename Declaration::PerFrameData>{});
        }
        return result;
    }

    [[nodiscard]] LUX_RENDER_PUBLIC RenderResult<void>
    validateResourceRegistration(const ResourceRegistration&) noexcept;
} // namespace lux::render
