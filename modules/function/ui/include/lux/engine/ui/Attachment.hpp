#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/object/LuxObject.hpp>
#include <memory>
#include <lux/cxx/core/function_ref.hpp>
#include <lux/engine/ui/Ids.hpp>

namespace lux::ui
{
    class Root;
    class Pane;
    // Object lifetime is supplied by ObjectState. The epoch distinguishes successive UI attachments
    // of the same external Pane, so queued input cannot cross an unmount/remount boundary.
    class PaneHandle final
    {
    public:
        [[nodiscard]] bool valid() const noexcept
        {
            return pane_.isValid() && root_.isValid() && attachment_ != 0;
        }
        friend bool operator==(const PaneHandle&, const PaneHandle&) = default;

    private:
        friend class Root;
        object::ObjectId pane_, root_;
        std::uint64_t attachment_{};
    };
    struct WindowVisibility final
    {
        Pane* pane{};
        bool visible{};
    };
    namespace detail
    {
        struct AttachmentState;
    }
    enum class EAttachmentError : std::uint8_t
    {
        WRONG_THREAD,
        WRONG_DISPATCHER,
        BUSY,
        CLOSED,
        ALREADY_ATTACHED,
        OCCUPIED,
        NOT_ATTACHED,
        INVALID_TREE,
        DUPLICATE_ID,
        CAPACITY,
        STALE_PREPARATION
    };
    // A one-shot, non-owning preparation. Destroying Root/candidate invalidates it.
    class LUX_FUNCTION_PUBLIC PreparedAttachment final
    {
    public:
        ~PreparedAttachment() noexcept;
        PreparedAttachment(PreparedAttachment&&) noexcept;
        PreparedAttachment& operator=(PreparedAttachment&&) noexcept;
        PreparedAttachment(const PreparedAttachment&) = delete;
        PreparedAttachment& operator=(const PreparedAttachment&) = delete;

    private:
        friend class Root;
        explicit PreparedAttachment(std::unique_ptr<detail::AttachmentState>) noexcept;
        std::unique_ptr<detail::AttachmentState> state_;
    };
    struct AttachmentCommit final
    {
        bool mounted{};
        object::SignalDelivery notifications;
    };
    struct AttachmentChanged final
    {
        PaneId pane;
        bool mounted{};
    };
}
