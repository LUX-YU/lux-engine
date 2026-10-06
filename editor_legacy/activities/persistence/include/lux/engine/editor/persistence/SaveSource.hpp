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
    class PreparedSaveSourceRegistration;
    // Revocation cuts off future role calls immediately, including between describe and capture.
    // registerSource borrows an adapter that must outlive active callbacks. prepareSource owns its adapter;
    // revocation disables calls while active dispatch/operations retain the source and its external code pin.
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
        friend class PreparedSaveSourceRegistration;
        struct State;
        explicit SaveSourceRegistration(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
    // Hidden role reservation. Abandonment revokes without invoking the source.
    // Move assignment clears the previous reservation under its previous code owner.
    class PreparedSaveSourceRegistration final
    {
    public:
        ~PreparedSaveSourceRegistration();
        PreparedSaveSourceRegistration(PreparedSaveSourceRegistration&&) noexcept;
        PreparedSaveSourceRegistration& operator=(PreparedSaveSourceRegistration&&) noexcept;
        PreparedSaveSourceRegistration(const PreparedSaveSourceRegistration&) = delete;
        PreparedSaveSourceRegistration& operator=(const PreparedSaveSourceRegistration&) = delete;

    private:
        friend class SaveService;
        explicit PreparedSaveSourceRegistration(std::shared_ptr<SaveSourceRegistration::State>) noexcept;
        std::shared_ptr<SaveSourceRegistration::State> state_;
    };
}
