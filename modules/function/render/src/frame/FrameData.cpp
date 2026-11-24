#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <exception>

namespace lux::render
{
    struct FrameData::Impl final
    {
        struct OwnedColumn final
        {
            std::size_t batch;
            const frame_detail::ColumnOps* operations;
            std::unique_ptr<void, void (*)(void*) noexcept> values;
        };

        std::vector<Batch> batches;
        std::vector<OwnedColumn> columns;
    };

    FrameData::FrameData() noexcept = default;

    FrameData::FrameData(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    FrameData::~FrameData() noexcept = default;

    FrameData::FrameData(FrameData&&) noexcept = default;

    FrameData& FrameData::operator=(FrameData&&) noexcept = default;

    RenderResult<FrameData>
    FrameData::create(
        std::vector<Batch> batches,
        std::span<const frame_detail::ColumnOps* const> operations,
        std::span<const std::size_t> capacities
    ) noexcept
    {
        if (!capacities.empty() && capacities.size() != operations.size())
            return cxx::unexpected(RenderError{kFrameContract});
        auto result = std::make_unique<Impl>();
        result->batches = std::move(batches);
        result->columns.reserve(operations.size());
        for (std::size_t batch = 0; batch < result->batches.size(); ++batch)
        {
            for (std::size_t n = 0; n < result->batches[batch].channels.size(); ++n)
            {
                const auto i = result->columns.size();
                const auto* ops = operations[i];
                Impl::OwnedColumn column{batch, ops, {ops->create(), ops->destroy}};
                if (!capacities.empty())
                {
                    if (auto reserved = ops->reserve(column.values.get(), capacities[i]); !reserved)
                        return cxx::unexpected(reserved.error());
                }
                result->columns.push_back(std::move(column));
            }
        }
        return FrameData{std::move(result)};
    }

    std::span<const FrameData::Batch> FrameData::batches() const & noexcept
    {
        return impl_ ? std::span<const Batch>{impl_->batches} : std::span<const Batch>{};
    }

    bool FrameData::matchesChannel(std::size_t index, cxx::basic_type_info declaration, cxx::basic_type_info element) const noexcept
    {
        if (!impl_ || index >= impl_->columns.size()) return false;
        const auto& batch = impl_->batches[impl_->columns[index].batch];
        const auto& schema = batch.channels[index - batch.first_column];
        return schema.declaration == declaration && schema.element == element &&
            schema.declaration.name() == declaration.name() && schema.element.name() == element.name();
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
        if (is_invalid_column) return cxx::unexpected(RenderError{kFrameContract});
        auto& column = impl_->columns[index];
        return column.operations->append(column.values.get(), count);
    }

    RenderResult<void> FrameData::reserve(std::size_t index, std::size_t count) noexcept
    {
        const bool is_invalid_column = !impl_ || index >= impl_->columns.size();
        if (is_invalid_column) return cxx::unexpected(RenderError{kFrameContract});
        auto& column = impl_->columns[index];
        return column.operations->reserve(column.values.get(), count);
    }

    void FrameData::reset() noexcept
    {
        if (!impl_) return;
        for (auto& column : impl_->columns) column.operations->clear(column.values.get());
    }
} // namespace lux::render
