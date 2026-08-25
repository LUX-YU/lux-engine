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
                WorldMutation&,
                std::span<const Entity>,
                std::span<const Entity>,
                const WorldSectionColumnView&,
                WorldSectionLoadScratchBudget,
                lux::serialization::SerializationLimits
            ) noexcept;

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
            WorldMutation& edit,
            std::span<const Entity> row_entities,
            std::span<const Entity> ordinal_entities,
            const WorldSectionColumnView& column,
            WorldSectionLoadScratchBudget scratch,
            lux::serialization::SerializationLimits limits
        ) noexcept
        {
            if (row_entities.size() != column.rowCount())
                return detail::invalidColumnValue(0U);

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
                    trackMembership(
                        edit,
                        entt::type_hash<Component>::value(),
                        row_entities
                    );
                    storage.insert(row_entities.begin(), row_entities.end());
                    return {};
                }

                const std::size_t batch_rows = std::max<std::size_t>(
                    scratch.decode_bytes /
                        std::max<std::size_t>(sizeof(Component), 1U),
                    1U
                );
                storage.reserve(storage.size() + row_entities.size());
                std::vector<Component> values;
                values.reserve(std::min(row_entities.size(), batch_rows));
                for (std::size_t batch_begin{};
                     batch_begin < row_entities.size();
                     batch_begin += batch_rows)
                {
                    const std::size_t batch_size = std::min(
                        batch_rows,
                        row_entities.size() - batch_begin
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
                    trackMembership(
                        edit,
                        entt::type_hash<Component>::value(),
                        row_entities.subspan(batch_begin, batch_size)
                    );
                    storage.insert(
                        row_entities.begin() + batch_begin,
                        row_entities.begin() + batch_begin + batch_size,
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
            detail::LoadComponentColumnFn load
        ) noexcept
            : schema_(&schema),
              storage_(storage),
              value_encoding_(value_encoding),
              fixed_stride_(fixed_stride),
              load_(load)
        {
        }

        LUX_ENGINE_ECS_WORLD_SECTION_PUBLIC static void trackMembership(
            WorldMutation& edit,
            std::uint64_t storage,
            std::span<const Entity> entities
        ) noexcept;

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
        detail::LoadComponentColumnFn load_{};
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
            &ComponentLoadBinding::loadColumn<Component>
        };
    }
} // namespace lux::ecs
