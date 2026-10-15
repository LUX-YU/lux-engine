#pragma once

#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>

namespace buffer_fixture
{
    using namespace lux::render;
    namespace native = lux::render::vulkan;
    struct Config final { std::uint32_t size = 16; bool operator==(const Config&) const noexcept = default; };
    inline constexpr StorageDescriptor description{StorageTypeIdView("test.zero_buffer"), {1, 0}, 15};

    struct Native final
    {
        native::DeviceSession& device;
        Config config;
        native::BufferPermit permit;
        std::optional<native::SubmissionTicket> last_use;

        ~Native() noexcept
        {
            if (!device.borrow(permit)) return;
            const auto result = last_use ? device.releaseCompleted(permit, *last_use) : device.destroyUnsubmitted(permit);
            if (!result) std::terminate();
        }
    };

    inline StorageFactory factory() noexcept
    {
        static const auto size = [](const void* value) noexcept { return static_cast<const Native*>(value)->config.size; };
        static const native::StorageBindingOps bindings{
            [](void* object, VkDeviceSize size, VkBufferUsageFlags usage) noexcept
                -> RenderResult<native::StorageBufferBinding> {
                auto& value = *static_cast<Native*>(object);
                auto* buffer = value.device.borrow(value.permit);
                const bool mismatch = buffer->size() != size || (buffer->usage() & usage) != usage;
                if (mismatch) return lux::cxx::unexpected(RenderError{kStorageMismatch});
                return native::StorageBufferBinding{buffer, object,
                    [](void* instance, native::SubmissionTicket ticket) noexcept {
                        static_cast<Native*>(instance)->last_use = ticket;
                    }
                };
            }
        };
        return {
            &description, EGraphicsApi::VULKAN, &size,
            [](void* device, const void* config) noexcept -> RenderResult<NativeStorage> {
                auto& session = *static_cast<native::DeviceSession*>(device);
                const auto value = *static_cast<const Config*>(config);
                auto candidate = std::make_unique<Native>(session, value);
                auto buffer = session.createBuffer(value.size,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, native::EMemoryAccess::UPLOAD);
                if (!buffer) return lux::cxx::unexpected(buffer.error());
                candidate->permit = std::move(*buffer);
                const std::vector<std::byte> zero(value.size);
                if (auto written = session.write(*session.borrow(candidate->permit), 0, zero); !written)
                    return lux::cxx::unexpected(written.error());
                return NativeStorage{candidate.release(), [](void* instance) noexcept { delete static_cast<Native*>(instance); }};
            },
            [](const void* instance, const void* config) noexcept {
                return static_cast<const Native*>(instance)->config == *static_cast<const Config*>(config);
            }, {}, &bindings
        };
    }

    inline RenderResult<void> install(native::DeviceSession& session) noexcept
    {
        if (auto result = session.resources().installFactory(factory()); !result) return result;
        return session.resources().configureType(description, std::make_shared<const Config>());
    }
}
