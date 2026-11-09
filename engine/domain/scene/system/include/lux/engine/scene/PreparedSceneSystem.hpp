#pragma once

#include <lux/engine/scene/ResolvedSceneDependency.hpp>
#include <lux/engine/system/SystemDataContract.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <memory>
#include <span>
#include <utility>
#include <optional>

namespace lux::scene
{
    // The legacy factories still return this owned provider value. Configuration and input spans
    // borrow the candidate data; this value has no scheduling or source-retention protocol.
    class PreparedSceneSystem final
    {
    public:
        PreparedSceneSystem() noexcept = default;

        PreparedSceneSystem(PreparedSceneSystem&& other) noexcept
        {
            swap(other);
        }

        PreparedSceneSystem& operator=(PreparedSceneSystem&& other) noexcept
        {
            PreparedSceneSystem previous(std::move(other));
            swap(previous);
            return *this;
        }

        PreparedSceneSystem(const PreparedSceneSystem&) = delete;
        PreparedSceneSystem& operator=(const PreparedSceneSystem&) = delete;

        // Configuration and input facts are explicit borrows into the address-stable candidate.
        template <class T, class Configuration>
        [[nodiscard]] static PreparedSceneSystem
        create(
            std::unique_ptr<T>&& value,
            ResolvedSceneDependency provider,
            const Configuration& configuration,
            std::optional<std::span<const system::SystemDataContract>> inputs = std::nullopt
        ) noexcept
        {
            PreparedSceneSystem result;
            result.value_ = {value.release(), +[](void* object) noexcept { delete static_cast<T*>(object); }};
            result.provider_ = provider;
            result.configuration_type_ = cxx::make_basic_type_info<Configuration>();
            result.configuration_ = &configuration;
            result.inputs_ = inputs;
            return result;
        }

        [[nodiscard]] const ResolvedSceneDependency& provider() const noexcept
        {
            return provider_;
        }

        [[nodiscard]] std::optional<std::span<const system::SystemDataContract>> dataInputs() const noexcept
        {
            return inputs_;
        }

        [[nodiscard]] const void* configuration(cxx::type_info type) const noexcept
        {
            return configuration_type_ == type ? configuration_ : nullptr;
        }

    private:
        void swap(PreparedSceneSystem& other) noexcept
        {
            using std::swap;
            swap(value_, other.value_);
            swap(provider_, other.provider_);
            swap(inputs_, other.inputs_);
            swap(configuration_type_, other.configuration_type_);
            swap(configuration_, other.configuration_);
        }

        cxx::type_info configuration_type_;
        const void* configuration_ = nullptr;
        std::optional<std::span<const system::SystemDataContract>> inputs_;
        std::unique_ptr<void, void (*)(void*) noexcept> value_{nullptr, nullptr};
        ResolvedSceneDependency provider_;
    };
} // namespace lux::scene
