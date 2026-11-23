#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <exception>

namespace lux::render
{
    struct FrameData::Impl final
    {
        struct OwnedColumn final
        {
            const frame_detail::ColumnOps* operations;
            std::unique_ptr<void, void (*)(void*) noexcept> values;
        };

        FrameLayout layout;
        std::vector<OwnedColumn> columns;
        EPhase phase = EPhase::WRITABLE;
    };

    FrameData::FrameData() noexcept = default;

    FrameData::FrameData(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    FrameData::~FrameData() noexcept
    {
        requireUnsealed();
    }

    FrameData::FrameData(FrameData&& other) noexcept
    {
        other.requireUnsealed();
        impl_ = std::move(other.impl_);
    }

    FrameData& FrameData::operator=(FrameData&& other) noexcept
    {
        requireUnsealed();
        other.requireUnsealed();
        if (this != &other) impl_ = std::move(other.impl_);
        return *this;
    }

    void FrameData::requireUnsealed() const noexcept
    {
        if (sealed()) std::terminate();
    }

    RenderResult<FrameData>
    FrameData::create(
        FrameLayout layout,
        std::span<const frame_detail::ColumnOps* const> operations,
        std::span<const std::size_t> capacities
    ) noexcept
    {
        const bool is_invalid_layout = !layout.valid() || operations.size() != layout.channels().size();
        const bool is_invalid_capacities = !capacities.empty() && capacities.size() != operations.size();
        if (is_invalid_layout || is_invalid_capacities) return cxx::unexpected(RenderError{kFrameContract});
        auto result = std::make_unique<Impl>();
        result->layout = std::move(layout);
        result->columns.reserve(operations.size());
        for (std::size_t i = 0; i < operations.size(); ++i)
        {
            const auto* operation = operations[i];
            if (!operation) return cxx::unexpected(RenderError{kFrameContract});
            Impl::OwnedColumn column{operation, {operation->create(), operation->destroy}};
            if (!capacities.empty())
            {
                if (auto reserved = operation->reserve(column.values.get(), capacities[i]); !reserved)
                    return cxx::unexpected(reserved.error());
            }
            result->columns.push_back(std::move(column));
        }
        return FrameData{std::move(result)};
    }

    bool FrameData::writable() const noexcept
    {
        return impl_ && impl_->phase == EPhase::WRITABLE;
    }

    bool FrameData::sealed() const noexcept
    {
        return impl_ && impl_->phase != EPhase::WRITABLE;
    }

    bool FrameData::beginSubmission() noexcept
    {
        if (!impl_ || impl_->phase != EPhase::SEALED) return false;
        impl_->phase = EPhase::IN_FLIGHT;
        return true;
    }

    void FrameData::completeSubmission() noexcept
    {
        if (!impl_ || impl_->phase != EPhase::IN_FLIGHT) std::terminate();
        impl_->phase = EPhase::WRITABLE;
    }

    const FrameLayout& FrameData::layout() const & noexcept
    {
        if (!impl_) std::terminate();
        return impl_->layout;
    }

    FrameLayoutKey FrameData::layoutKey() const noexcept
    {
        return impl_ ? impl_->layout.key() : FrameLayoutKey{};
    }

    std::size_t FrameData::findChannel(cxx::basic_type_info declaration, cxx::basic_type_info element) const noexcept
    {
        return impl_ ? impl_->layout.find(declaration, element) : SIZE_MAX;
    }

    FrameData::Column FrameData::column(std::size_t index) const & noexcept
    {
        if (!impl_ || index >= impl_->columns.size()) std::terminate();
        const auto& column = impl_->columns[index];
        return {column.operations->data(column.values.get()), column.operations->size(column.values.get())};
    }

    std::size_t FrameData::capacity(std::size_t index) const noexcept
    {
        if (!impl_ || index >= impl_->columns.size()) std::terminate();
        const auto& column = impl_->columns[index];
        return column.operations->capacity(column.values.get());
    }

    RenderResult<void*> FrameData::append(std::size_t index, std::size_t count) noexcept
    {
        const bool is_invalid_column = !impl_ || index >= impl_->columns.size();
        if (is_invalid_column || !writable()) return cxx::unexpected(RenderError{kFrameContract});
        auto& column = impl_->columns[index];
        return column.operations->append(column.values.get(), count);
    }

    RenderResult<void> FrameData::reserve(std::size_t index, std::size_t count) noexcept
    {
        const bool is_invalid_column = !impl_ || index >= impl_->columns.size();
        if (is_invalid_column || !writable()) return cxx::unexpected(RenderError{kFrameContract});
        auto& column = impl_->columns[index];
        return column.operations->reserve(column.values.get(), count);
    }

    RenderResult<void> FrameData::seal() noexcept
    {
        if (!writable()) return cxx::unexpected(RenderError{kFrameContract});
        impl_->phase = EPhase::SEALED;
        return {};
    }

    void FrameData::release() noexcept
    {
        if (!impl_ || impl_->phase != EPhase::SEALED) std::terminate();
        impl_->phase = EPhase::WRITABLE;
    }

    void FrameData::reset() noexcept
    {
        requireUnsealed();
        if (!impl_) return;
        for (auto& column : impl_->columns) column.operations->clear(column.values.get());
    }
} // namespace lux::render
