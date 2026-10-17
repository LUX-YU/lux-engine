#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>

namespace buffer_fixture
{
    using namespace lux::render;
    namespace native = lux::render::vulkan;
    struct Config final { std::uint32_t size = 16; bool operator==(const Config&) const noexcept = default; };
    inline constexpr StorageDescriptor description{StorageNameView("test.zero_buffer")};

    struct Native final
    {
        native::DeviceSession& device;
        Config config;
        native::BufferPermit permit;
        native::SubmissionRequirements last_use;

        ~Native() noexcept
        {
            if (!device.borrow(permit)) return;
            const auto result = last_use.empty() ? device.destroyUnsubmitted(permit) : device.releaseCompleted(permit, last_use);
            if (!result) std::terminate();
        }
    };

    struct Declaration final
    {
        using Config = buffer_fixture::Config;
        using Ops = std::uint32_t (*)(const void*) noexcept;
        static inline constexpr auto description = buffer_fixture::description;
    };

    inline StorageFactory factory() noexcept
    {
        static const Declaration::Ops size = [](const void* value) noexcept { return static_cast<const Native*>(value)->config.size; };
        static const native::StorageBindingOps bindings{
            [](void* object, const native::NativeResourcePlan& plan, std::span<const std::byte>,
                native::PreparedStorageBinding& binding) noexcept -> RenderResult<void> {
                auto& value = *static_cast<Native*>(object);
                auto* buffer = value.device.borrow(value.permit);
                const auto size = std::get<GraphBufferDesc>(plan.description).size;
                const bool mismatch = buffer->size() != size || (buffer->usage() & plan.buffer_usage) != plan.buffer_usage;
                if (mismatch) return lux::cxx::unexpected(RenderError{kStorageMismatch});
                binding.buffer = buffer;
                binding.last_use = &value.last_use;
                return {};
            }
        };
        static const auto create = +[](native::DeviceSession& session, StorageId, const Config& value) noexcept
            -> RenderResult<std::unique_ptr<Native>> {
            auto candidate = std::make_unique<Native>(session, value);
            auto buffer = session.createBuffer(value.size,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, native::EMemoryAccess::UPLOAD);
            if (!buffer) return lux::cxx::unexpected(buffer.error());
            candidate->permit = std::move(*buffer);
            const std::vector<std::byte> zero(value.size);
            if (auto written = session.write(*session.borrow(candidate->permit), 0, zero); !written)
                return lux::cxx::unexpected(written.error());
            return candidate;
        };
        static const auto compatible = +[](const Native& instance, const Config& config) noexcept {
            return instance.config == config;
        };
        return makeStorageFactory<Declaration, Native, native::DeviceSession, create, compatible>(size, &bindings);
    }

    inline RenderResult<void> install(native::DeviceSession& session) noexcept
    {
        auto result = session.resources().installFactory(factory(), Config{});
        if (!result) return lux::cxx::unexpected(result.error());
        return {};
    }
}
