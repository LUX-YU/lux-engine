#pragma once

#include <lux/engine/render/resource/Factory.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render
{
    namespace registration_detail { struct NativeBinding; struct CommandRegistrationAudit; }

    class CommandRegistration final : public CommandHandler
    {
    public:
        CommandRegistration() noexcept = default;
        CommandRegistration(CommandHandler handler) noexcept : CommandHandler(handler) {}
        [[nodiscard]] bool gpuCompletion() const noexcept { return gpu_completion_; }
        void bindNative(void* owner) noexcept
        {
            if (bind_native_) static_cast<CommandHandler&>(*this) = bind_native_(owner);
        }

    private:
        friend struct registration_detail::NativeBinding;
        friend struct registration_detail::CommandRegistrationAudit;
        CommandHandler (*bind_native_)(void*) noexcept = nullptr;
        bool gpu_completion_ = false;
    };

    // Cold declaration-to-implementation binding. The factory remains the only
    // native instance owner; the handlers are immutable borrowed metadata.
    struct StorageRegistration final
    {
        // Registration is constructed once. Memberwise replacement would release
        // the old factory code pin before its configuration's foreign deleter.
        StorageRegistration& operator=(const StorageRegistration&) = delete;

        const StorageDescriptor* description;
        std::span<const CommandRegistration> handlers;
        StorageFactory factory;
        bool optional = false;
        std::shared_ptr<const void> configuration;
        StorageCppType configuration_type;
    };

    // The selected Native module supplies a specialization. A declaration does
    // not silently choose a backend or create a second resource directory.
    template <typename T>
    [[nodiscard]] StorageRegistration nativeStorageBinding() noexcept;

}
