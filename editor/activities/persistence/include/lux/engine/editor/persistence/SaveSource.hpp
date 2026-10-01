#pragma once

#include <lux/engine/editor/persistence/EncodeJob.hpp>

namespace lux::editor::persistence
{
    class ISaveSource
    {
    public:
        virtual ~ISaveSource() = default;
        [[nodiscard]] virtual PersistenceResult<SaveSourceInfo> describe() const = 0;
        [[nodiscard]] virtual PersistenceResult<FrozenSave> captureForSave(
            const SaveSourceInfo&,
            const SaveRequest&,
            std::size_t max_bytes
        ) = 0;
        [[nodiscard]] virtual EAdoption accept(SaveReceipt&&) noexcept = 0;
    };
    class SaveService;
    // Revocation cuts off future role calls immediately, including between describe and capture.
    // The adapter must outlive any callback already on the stack; the token does not own it.
    class SaveSourceRegistration final
    {
    public:
        ~SaveSourceRegistration();
        SaveSourceRegistration(SaveSourceRegistration&&) noexcept;
        SaveSourceRegistration& operator=(SaveSourceRegistration&&) noexcept;
        SaveSourceRegistration(const SaveSourceRegistration&) = delete;
        SaveSourceRegistration& operator=(const SaveSourceRegistration&) = delete;

    private:
        friend class SaveService;
        struct State;
        explicit SaveSourceRegistration(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
}
