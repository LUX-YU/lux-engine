#pragma once

#include <lux/engine/world/WorldDataSchemaId.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/world/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::world
{
    class WorldDescription;
    class WorldDescriptionBuilder;

    class LUX_ENGINE_WORLD_PUBLIC WorldDataView final
    {
    public:
        WorldDataView() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return world_ != nullptr;
        }

        [[nodiscard]] const WorldDataSchemaId &schema() const noexcept;
        [[nodiscard]] std::uint32_t version() const noexcept;
        [[nodiscard]] std::span<const std::byte> payload() const noexcept;

    private:
        WorldDataView(const WorldDescription &world, std::size_t data_index) noexcept;

        const WorldDescription *world_{};
        std::size_t data_index_{};

        friend class WorldDescription;
        friend class WorldObjectView;
    };

    class LUX_ENGINE_WORLD_PUBLIC WorldObjectView final
    {
    public:
        WorldObjectView() noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return world_ != nullptr;
        }

        [[nodiscard]] WorldObjectId id() const noexcept;
        [[nodiscard]] std::size_t   dataCount() const noexcept;
        [[nodiscard]] WorldDataView dataAt(std::size_t index) const noexcept;
        [[nodiscard]] WorldDataView findData(const WorldDataSchemaId &schema) const noexcept;

    private:
        WorldObjectView(const WorldDescription &world, std::size_t object_index) noexcept;

        const WorldDescription *world_{};
        std::size_t object_index_{};

        friend class WorldDescription;
    };

    class LUX_ENGINE_WORLD_PUBLIC WorldDescription final
    {
    public:
        WorldDescription() noexcept = default;
        WorldDescription(WorldDescription &&) noexcept = default;
        WorldDescription &operator=(WorldDescription &&) noexcept = default;
        ~WorldDescription() = default;

        WorldDescription(const WorldDescription &) = delete;
        WorldDescription &operator=(const WorldDescription &) = delete;

        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t objectCount() const noexcept;
        [[nodiscard]] std::size_t dataCount() const noexcept;
        [[nodiscard]] std::size_t payloadBytes() const noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;
        [[nodiscard]] std::span<const WorldDataSchemaId> schemas() const noexcept;
        [[nodiscard]] WorldObjectView objectAt(std::size_t index) const noexcept;
        [[nodiscard]] WorldObjectView findObject(WorldObjectId id) const noexcept;

    private:
        struct ObjectRecord final
        {
            WorldObjectId id;
            std::size_t first_data{};
            std::size_t data_count{};
        };

        struct DataRecord final
        {
            std::size_t schema_ordinal{};
            std::uint32_t version{};
            std::size_t payload_offset{};
            std::size_t payload_size{};
        };

        std::vector<WorldDataSchemaId> schemas_;
        std::vector<ObjectRecord> objects_;
        std::vector<DataRecord> data_;
        std::vector<std::byte> payload_;

        friend class WorldDataView;
        friend class WorldObjectView;
        friend class WorldDescriptionBuilder;
    };
}
