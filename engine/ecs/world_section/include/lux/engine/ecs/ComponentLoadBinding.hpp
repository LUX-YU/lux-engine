#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>
#include <lux/engine/ecs/world_section/detail/ComponentLoadSerialization.hpp>
#include <lux/engine/serialization/Traits.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <iterator>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class ComponentLoadSet;
    class ComponentLoadBinding;
    class WorldSectionLoadBatch;
    class WorldSectionLoader;

    namespace detail
    {
        using LoadComponentColumnFn =
            lux::serialization::SerializationResult (*)(
                WorldEdit&,
                std::span<const Entity>,
                const WorldSectionColumnView&,
                WorldSectionLoadScratchBudget,
                lux::serialization::SerializationLimits
            ) noexcept;

        class ColumnEntityIterator final
        {
          public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = Entity;
            using difference_type = std::ptrdiff_t;
            using pointer = void;
            using reference = Entity;

            ColumnEntityIterator() noexcept = default;
            ColumnEntityIterator(
                std::span<const Entity> entities,
                const WorldSectionColumnView& column,
                std::size_t row
            ) noexcept
                : entities_(entities), column_(&column), row_(row)
            {
            }

            [[nodiscard]] Entity operator*() const noexcept
            {
                const std::uint32_t ordinal =
                    column_->ordinalEncoding() ==
                        EWorldSectionOrdinalEncoding::DENSE
                    ? static_cast<std::uint32_t>(row_)
                    : readColumnU32(
                        column_->ordinalBytes(),
                        row_ * sizeof(std::uint32_t)
                    );
                return entities_[ordinal];
            }

            ColumnEntityIterator& operator++() noexcept
            {
                ++row_;
                return *this;
            }

            ColumnEntityIterator operator++(int) noexcept
            {
                ColumnEntityIterator result = *this;
                ++*this;
                return result;
            }

            [[nodiscard]] friend bool operator==(
                const ColumnEntityIterator& left,
                const ColumnEntityIterator& right
            ) noexcept
            {
                return left.column_ == right.column_ &&
                    left.row_ == right.row_;
            }

          private:
            std::span<const Entity> entities_;
            const WorldSectionColumnView* column_{};
            std::size_t row_{};
        };

#if defined(LUX_ECS_WORLD_SECTION_TESTING)
        struct ComponentLoadTestStats final
        {
            static inline std::size_t load_calls{};
            static inline std::size_t storage_lookups{};

            static void reset() noexcept
            {
                load_calls = 0U;
                storage_lookups = 0U;
            }
        };
#endif

        [[nodiscard]] inline lux::serialization::SerializationResult
        invalidColumnValue(std::size_t offset) noexcept
        {
            return lux::cxx::unexpected<
                lux::serialization::SerializationFailure>(
                lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::INVALID_VALUE,
                    offset
                }
            );
        }

    } // namespace detail

    class ComponentLoadBinding final
    {
      public:
        [[nodiscard]] constexpr const ComponentSchema& schema() const noexcept
        {
            return *schema_;
        }

        [[nodiscard]] constexpr EWorldSectionValueEncoding
        valueEncoding() const noexcept
        {
            return value_encoding_;
        }

        [[nodiscard]] constexpr std::uint32_t fixedStride() const noexcept
        {
            return fixed_stride_;
        }

      private:
        template <class Component>
        [[nodiscard]] static lux::serialization::SerializationResult
        loadColumn(
            WorldEdit& edit,
            std::span<const Entity> ordinal_entities,
            const WorldSectionColumnView& column,
            WorldSectionLoadScratchBudget scratch,
            lux::serialization::SerializationLimits limits
        ) noexcept
        {
            try
            {
                detail::require(edit.world_ != nullptr);
                auto& storage =
                    edit.world_->registry_.template storage<Component>();
#if defined(LUX_ECS_WORLD_SECTION_TESTING)
                ++detail::ComponentLoadTestStats::load_calls;
                ++detail::ComponentLoadTestStats::storage_lookups;
#endif
                if constexpr (std::is_empty_v<Component>)
                {
                    if (column.valueEncoding() !=
                        EWorldSectionValueEncoding::TAG)
                        return detail::invalidColumnValue(0U);
                    const detail::ColumnEntityIterator first(
                        ordinal_entities,
                        column,
                        0U
                    );
                    const detail::ColumnEntityIterator last(
                        ordinal_entities,
                        column,
                        column.rowCount()
                    );
                    storage.insert(first, last);
                    return {};
                }

                if (scratch.decode_bytes < sizeof(Component))
                {
                    return lux::cxx::unexpected<
                        lux::serialization::SerializationFailure>(
                        lux::serialization::SerializationFailure{
                            lux::serialization::ESerializationError::LIMIT_EXCEEDED,
                            0U
                        }
                    );
                }
                const std::size_t batch_rows =
                    scratch.decode_bytes / sizeof(Component);
                storage.reserve(storage.size() + column.rowCount());
                std::vector<Component> values;
                values.reserve(std::min(
                    static_cast<std::size_t>(column.rowCount()),
                    batch_rows
                ));
                for (std::size_t batch_begin{};
                     batch_begin < column.rowCount();
                     batch_begin += batch_rows)
                {
                    const std::size_t batch_size = std::min(
                        batch_rows,
                        static_cast<std::size_t>(column.rowCount()) -
                            batch_begin
                    );
                    values.clear();
                    for (std::size_t local{}; local < batch_size; ++local)
                    {
                        const std::size_t row = batch_begin + local;
                        std::span<const std::byte> row_bytes;
                        if (column.valueEncoding() ==
                            EWorldSectionValueEncoding::FIXED)
                        {
                            const std::size_t begin =
                                row * column.fixedStride();
                            row_bytes = column.payload().subspan(
                                begin,
                                column.fixedStride()
                            );
                        }
                        else if (column.valueEncoding() ==
                            EWorldSectionValueEncoding::VARIABLE)
                        {
                            const auto offsets = column.offsetBytes();
                            const std::uint32_t begin = detail::readColumnU32(
                                offsets,
                                row * sizeof(std::uint32_t)
                            );
                            const std::uint32_t end = detail::readColumnU32(
                                offsets,
                                (row + 1U) * sizeof(std::uint32_t)
                            );
                            row_bytes = column.payload().subspan(
                                begin,
                                end - begin
                            );
                        }
                        else
                        {
                            return detail::invalidColumnValue(0U);
                        }

                        values.emplace_back();
                        detail::ComponentLoadReader reader(
                            row_bytes,
                            ordinal_entities,
                            limits
                        );
                        auto decoded = lux::serialization::read(
                            reader,
                            values.back()
                        );
                        if (!decoded)
                            return decoded;
                        if (reader.remaining() != 0U)
                            return detail::invalidColumnValue(reader.offset());
                    }
                    const detail::ColumnEntityIterator first(
                        ordinal_entities,
                        column,
                        batch_begin
                    );
                    const detail::ColumnEntityIterator last(
                        ordinal_entities,
                        column,
                        batch_begin + batch_size
                    );
                    storage.insert(
                        first,
                        last,
                        std::make_move_iterator(values.begin())
                    );
                }
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected<
                    lux::serialization::SerializationFailure>(
                    lux::serialization::SerializationFailure{
                        lux::serialization::ESerializationError::ALLOCATION_FAILURE,
                        0U
                    }
                );
            }
            catch (...)
            {
                return detail::invalidColumnValue(0U);
            }
        }

        constexpr ComponentLoadBinding(
            const ComponentSchema& schema,
            std::uint64_t storage,
            EWorldSectionValueEncoding value_encoding,
            std::uint32_t fixed_stride,
            std::size_t value_size,
            detail::LoadComponentColumnFn load
        ) noexcept
            : schema_(&schema),
              storage_(storage),
              value_encoding_(value_encoding),
              fixed_stride_(fixed_stride),
              value_size_(value_size),
              load_(load)
        {
        }

        template <class Component>
        friend constexpr ComponentLoadBinding bindComponentLoad(
            const ComponentSchema&
        ) noexcept;
        friend class ComponentLoadSet;
        friend class WorldSectionLoadBatch;
        friend class WorldSectionLoader;

        const ComponentSchema* schema_{};
        std::uint64_t storage_{};
        EWorldSectionValueEncoding value_encoding_{};
        std::uint32_t fixed_stride_{};
        std::size_t value_size_{};
        detail::LoadComponentColumnFn load_{};
        std::size_t code_owner_index_{};
    };

    namespace detail
    {
        template <class... Binding>
            requires (std::same_as<
                std::remove_cvref_t<Binding>,
                ComponentLoadBinding> && ...)
        [[nodiscard]] constexpr auto componentLoadBindings(
            Binding&&... binding
        ) noexcept
        {
            return std::array<ComponentLoadBinding, sizeof...(Binding)>{
                std::forward<Binding>(binding)...};
        }
    } // namespace detail

    struct ComponentLoadContribution final
    {
        std::shared_ptr<const void> code_lifetime;
        std::span<const ComponentLoadBinding> bindings;
    };

    template <class Component>
    [[nodiscard]] constexpr ComponentLoadBinding bindComponentLoad(
        const ComponentSchema& schema
    ) noexcept
    {
        static_assert(lux::meta::HasTypeStaticInfo<Component>);
        static_assert(std::default_initializable<Component>);
        static_assert(std::is_nothrow_move_constructible_v<Component>);
        constexpr std::size_t fixed = lux::serialization::WireSizeV<Component>;
        constexpr auto encoding = fixed == 0U
            ? EWorldSectionValueEncoding::TAG
            : fixed == lux::serialization::DynamicWireSize
                ? EWorldSectionValueEncoding::VARIABLE
                : EWorldSectionValueEncoding::FIXED;
        static_assert(
            fixed == lux::serialization::DynamicWireSize ||
            fixed <= std::numeric_limits<std::uint32_t>::max()
        );
        constexpr std::uint32_t stride =
            encoding == EWorldSectionValueEncoding::FIXED
            ? static_cast<std::uint32_t>(fixed)
            : 0U;
        return ComponentLoadBinding{
            schema,
            entt::type_hash<Component>::value(),
            encoding,
            stride,
            sizeof(Component),
            &ComponentLoadBinding::loadColumn<Component>
        };
    }
} // namespace lux::ecs
