#pragma once

#include <lux/engine/platform/ipc/IpcMessage.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/runtime/commands/Command.hpp>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace lux::render
{
    namespace runtime_detail
    {
        struct WindowCommand final
        {
            CommandId command;
            platform::ipc::IpcMessage request;
        };
        using VWindowUpdate = std::variant<FrameData, WindowCommand>;
    }

    // Move the existing update storage through IPC and back to the client in the lead Job receipt.
    // An ordinary single Run needs no allocation for an empty update list.
    class WindowUpdates final
    {
    public:
        WindowUpdates() noexcept = default;
        WindowUpdates(FrameData packet) noexcept : first_(std::move(packet)) {}
        WindowUpdates(WindowUpdates&& other) noexcept
            : first_(std::exchange(other.first_, std::nullopt)), entries_(std::move(other.entries_)) {}

        WindowUpdates& operator=(WindowUpdates&& other) noexcept
        {
            if (this != &other)
            {
                first_ = std::exchange(other.first_, std::nullopt);
                entries_ = std::move(other.entries_);
            }
            return *this;
        }

        void append(FrameData packet) noexcept
        {
            if (empty()) first_.emplace(std::move(packet));
            else storage().emplace_back(std::move(packet));
        }

        template <RenderCommand C> requires std::same_as<typename C::Response, AckResponse>
        void append(CommandId id, typename C::Request input) noexcept
        {
            runtime_detail::WindowCommand update{id, {}};
            update.request.emplace<typename C::Request>(std::move(input));
            storage().emplace_back(std::move(update));
        }

        [[nodiscard]] bool empty() const noexcept { return !first_ && (!entries_ || entries_->empty()); }

        void clear() noexcept
        {
            first_.reset();
            if (entries_) entries_->clear();
        }

        [[nodiscard]] FrameData* packet(std::size_t index) & noexcept
        {
            if (first_)
            {
                if (index == 0) return &*first_;
                --index;
            }
            return entries_ && index < entries_->size() ? std::get_if<FrameData>(&(*entries_)[index]) : nullptr;
        }

        template <class Visitor> RenderResult<void> visit(Visitor&& visitor) & noexcept
        {
            if (first_)
                if (auto result = visitor(*first_); !result) return result;
            if (entries_)
                for (auto& item : *entries_)
                    if (auto result = std::visit(visitor, item); !result) return result;
            return {};
        }

    private:
        using Entries = std::vector<runtime_detail::VWindowUpdate>;
        std::optional<FrameData> first_;
        std::unique_ptr<Entries> entries_;

        Entries& storage() noexcept
        {
            if (!entries_) entries_ = std::make_unique<Entries>();
            return *entries_;
        }
    };
} // namespace lux::render
