#include <lux/engine/simulation/SimulationAssetCodec.hpp>

#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace lux::simulation
{
    namespace
    {
        constexpr std::uint32_t kWireVersion = 2U;
        constexpr std::size_t kHeaderBytes = 32U;
        constexpr std::size_t kDirectoryEntryBytes = 32U;
        constexpr std::size_t kGlobalRecordBytes = 40U;
        constexpr std::size_t kSystemTypeRecordBytes = 96U;
        constexpr std::size_t kInstanceRecordBytes = 32U;
        constexpr std::size_t kOrdinalRecordBytes = 8U;
        constexpr std::size_t kEventRecordBytes = 40U;
        constexpr std::size_t kDependencyRecordBytes = 32U;
        constexpr std::uint64_t kNoString =
            std::numeric_limits<std::uint64_t>::max();

        enum class ESection : std::uint32_t
        {
            STRINGS = 1U,
            GLOBAL_DATA = 2U,
            SYSTEM_TYPES = 3U,
            INSTANCES = 4U,
            CAPABILITIES = 5U,
            EXECUTION_POINTS = 6U,
            EVENTS = 7U,
            DEPENDENCIES = 8U,
            PAYLOAD = 9U,
        };

        constexpr std::size_t kSectionCount = 9U;

        struct Section final
        {
            ESection kind{};
            std::size_t offset{};
            std::size_t size{};
            std::size_t count{};
        };

        [[nodiscard]] auto codecFailure() noexcept
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetCodecError::CODEC_FAILURE
            );
        }

        [[nodiscard]] bool toSize(
            std::uint64_t value,
            std::size_t& result
        ) noexcept
        {
            if (value > std::numeric_limits<std::size_t>::max())
                return false;
            result = static_cast<std::size_t>(value);
            return true;
        }

        [[nodiscard]] bool addSize(
            std::size_t& total,
            std::size_t value
        ) noexcept
        {
            if (value > std::numeric_limits<std::size_t>::max() - total)
                return false;
            total += value;
            return true;
        }

        [[nodiscard]] bool multiplySize(
            std::size_t count,
            std::size_t stride,
            std::size_t& result
        ) noexcept
        {
            if (count != 0U &&
                stride > std::numeric_limits<std::size_t>::max() / count)
            {
                return false;
            }
            result = count * stride;
            return true;
        }

        class WireWriter final
        {
          public:
            explicit WireWriter(std::vector<std::byte>& bytes) noexcept
                : writer_(bytes)
            {
            }

            void u32(std::uint32_t value) noexcept
            {
                if (ok_)
                    ok_ = static_cast<bool>(writer_.writeUnsigned(value));
            }

            void u64(std::uint64_t value) noexcept
            {
                if (ok_)
                    ok_ = static_cast<bool>(writer_.writeUnsigned(value));
            }

            void bytes(std::span<const std::byte> value) noexcept
            {
                if (ok_)
                    ok_ = static_cast<bool>(writer_.writeBytes(value));
            }

            void string(std::string_view value) noexcept
            {
                u64(value.size());
                bytes({
                    reinterpret_cast<const std::byte*>(value.data()),
                    value.size()});
            }

            [[nodiscard]] bool ok() const noexcept { return ok_; }

          private:
            lux::serialization::BinaryWriter writer_;
            bool ok_{true};
        };

        class WireReader final
        {
          public:
            explicit WireReader(std::span<const std::byte> bytes) noexcept
                : reader_(bytes)
            {
            }

            [[nodiscard]] bool u32(std::uint32_t& value) noexcept
            {
                auto result = reader_.readUnsigned<std::uint32_t>();
                if (!result)
                    return false;
                value = *result;
                return true;
            }

            [[nodiscard]] bool u64(std::uint64_t& value) noexcept
            {
                auto result = reader_.readUnsigned<std::uint64_t>();
                if (!result)
                    return false;
                value = *result;
                return true;
            }

            [[nodiscard]] bool bytes(std::span<std::byte> value) noexcept
            {
                return static_cast<bool>(reader_.readBytes(value));
            }

            [[nodiscard]] std::size_t remaining() const noexcept
            {
                return reader_.remaining();
            }

          private:
            lux::serialization::BinaryReader reader_;
        };

        struct StringTable final
        {
            void add(std::string_view value)
            {
                if (!value.empty())
                    values.push_back(value);
            }

            void canonicalize()
            {
                std::sort(values.begin(), values.end());
                values.erase(
                    std::unique(values.begin(), values.end()),
                    values.end()
                );
            }

            [[nodiscard]] std::size_t ordinal(std::string_view value) const
            {
                return static_cast<std::size_t>(std::distance(
                    values.begin(),
                    std::lower_bound(values.begin(), values.end(), value)
                ));
            }

            std::vector<std::string_view> values;
        };

        struct DraftGlobal final
        {
            std::size_t schema{};
            std::uint64_t hash{};
            std::uint32_t version{};
            std::size_t payload_offset{};
            std::size_t payload_size{};
        };

        struct DraftType final
        {
            std::size_t name{};
            std::uint64_t hash{};
            std::uint32_t version{};
            std::size_t configuration_schema{kNoString};
            std::uint64_t configuration_hash{};
            std::uint32_t configuration_version{};
            std::size_t capability_first{};
            std::size_t capability_count{};
            std::size_t point_first{};
            std::size_t point_count{};
            std::size_t event_first{};
            std::size_t event_count{};
        };

        struct DraftInstance final
        {
            std::size_t name{};
            std::size_t type{};
            std::size_t configuration_offset{};
            std::size_t configuration_size{};
        };

        struct DraftEvent final
        {
            std::size_t name{};
            std::size_t point{};
            std::size_t payload_schema{kNoString};
            std::uint64_t payload_hash{};
            std::uint32_t payload_version{};
        };

        struct DraftDependency final
        {
            std::size_t before_system{};
            std::size_t before_point{};
            std::size_t after_system{};
            std::size_t after_point{};
        };

        struct Draft final
        {
            std::vector<std::string> strings;
            std::vector<DraftGlobal> globals;
            std::vector<DraftType> types;
            std::vector<DraftInstance> instances;
            std::vector<std::size_t> capabilities;
            std::vector<std::size_t> points;
            std::vector<DraftEvent> events;
            std::vector<DraftDependency> dependencies;
            std::span<const std::byte> payload;
        };

        [[nodiscard]] bool readSize(
            WireReader& reader,
            std::size_t& value
        ) noexcept
        {
            std::uint64_t raw{};
            return reader.u64(raw) && toSize(raw, value);
        }

        [[nodiscard]] bool fixedSection(
            const Section& section,
            std::size_t stride
        ) noexcept
        {
            std::size_t required{};
            return multiplySize(section.count, stride, required) &&
                required == section.size;
        }

        [[nodiscard]] std::span<const std::byte> sectionBytes(
            std::span<const std::byte> input,
            const Section& section
        ) noexcept
        {
            return input.subspan(section.offset, section.size);
        }

        [[nodiscard]] bool parseDirectory(
            std::span<const std::byte> input,
            std::array<Section, kSectionCount>& sections
        ) noexcept
        {
            if (input.size() <
                kHeaderBytes + kSectionCount * kDirectoryEntryBytes)
            {
                return false;
            }
            WireReader reader(input);
            std::uint32_t magic{};
            std::uint32_t version{};
            std::uint32_t section_count{};
            std::uint32_t reserved{};
            std::uint64_t directory_offset{};
            std::uint64_t file_size{};
            if (!reader.u32(magic) || !reader.u32(version) ||
                !reader.u32(section_count) || !reader.u32(reserved) ||
                !reader.u64(directory_offset) || !reader.u64(file_size) ||
                magic != SimulationAssetPrimaryMagic ||
                version != kWireVersion || section_count != kSectionCount ||
                reserved != 0U || directory_offset != kHeaderBytes ||
                file_size != input.size())
            {
                return false;
            }

            std::size_t expected_offset =
                kHeaderBytes + kSectionCount * kDirectoryEntryBytes;
            for (std::size_t index{}; index < sections.size(); ++index)
            {
                std::uint32_t kind{};
                std::uint32_t entry_reserved{};
                std::uint64_t offset{};
                std::uint64_t size{};
                std::uint64_t count{};
                if (!reader.u32(kind) || !reader.u32(entry_reserved) ||
                    !reader.u64(offset) || !reader.u64(size) ||
                    !reader.u64(count) ||
                    kind != static_cast<std::uint32_t>(index + 1U) ||
                    entry_reserved != 0U ||
                    !toSize(offset, sections[index].offset) ||
                    !toSize(size, sections[index].size) ||
                    !toSize(count, sections[index].count) ||
                    sections[index].offset != expected_offset ||
                    sections[index].size > input.size() - expected_offset)
                {
                    return false;
                }
                sections[index].kind = static_cast<ESection>(kind);
                expected_offset += sections[index].size;
            }
            return expected_offset == input.size();
        }

        [[nodiscard]] bool parseStrings(
            std::span<const std::byte> bytes,
            std::size_t count,
            Draft& draft
        )
        {
            WireReader reader(bytes);
            draft.strings.reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                std::size_t size{};
                if (!readSize(reader, size) || size == 0U ||
                    size > reader.remaining())
                {
                    return false;
                }
                std::string value(size, '\0');
                if (!reader.bytes({
                        reinterpret_cast<std::byte*>(value.data()),
                        value.size()}))
                {
                    return false;
                }
                if (!draft.strings.empty() && draft.strings.back() >= value)
                    return false;
                draft.strings.push_back(std::move(value));
            }
            return reader.remaining() == 0U;
        }

        [[nodiscard]] bool parseGlobals(
            std::span<const std::byte> bytes,
            std::size_t count,
            const Draft& strings,
            Draft& draft
        )
        {
            WireReader reader(bytes);
            draft.globals.reserve(count);
            std::size_t payload_cursor{};
            for (std::size_t index{}; index < count; ++index)
            {
                DraftGlobal value;
                std::uint32_t reserved{};
                if (!readSize(reader, value.schema) ||
                    !reader.u64(value.hash) || !reader.u32(value.version) ||
                    !reader.u32(reserved) ||
                    !readSize(reader, value.payload_offset) ||
                    !readSize(reader, value.payload_size) ||
                    value.schema >= strings.strings.size() ||
                    value.hash != lux::cxx::Fnv1a64::hash(
                        strings.strings[value.schema]
                    ) || value.version == 0U || reserved != 0U ||
                    value.payload_offset != payload_cursor ||
                    !addSize(payload_cursor, value.payload_size))
                {
                    return false;
                }
                if (!draft.globals.empty())
                {
                    const auto& previous = draft.globals.back();
                    if (std::tie(previous.hash, previous.schema) >=
                        std::tie(value.hash, value.schema))
                    {
                        return false;
                    }
                }
                draft.globals.push_back(value);
            }
            return reader.remaining() == 0U;
        }

        [[nodiscard]] bool parseOrdinals(
            std::span<const std::byte> bytes,
            std::size_t count,
            std::size_t string_count,
            std::vector<std::size_t>& output
        )
        {
            WireReader reader(bytes);
            output.reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                std::size_t value{};
                if (!readSize(reader, value) || value >= string_count)
                    return false;
                output.push_back(value);
            }
            return reader.remaining() == 0U;
        }

        [[nodiscard]] bool parseTypes(
            std::span<const std::byte> bytes,
            std::size_t count,
            const Draft& draft_values,
            Draft& draft
        )
        {
            WireReader reader(bytes);
            draft.types.reserve(count);
            std::size_t capability_cursor{};
            std::size_t point_cursor{};
            std::size_t event_cursor{};
            for (std::size_t index{}; index < count; ++index)
            {
                DraftType value;
                std::uint32_t reserved_a{};
                std::uint32_t reserved_b{};
                if (!readSize(reader, value.name) ||
                    !reader.u64(value.hash) || !reader.u32(value.version) ||
                    !reader.u32(reserved_a) ||
                    !readSize(reader, value.configuration_schema) ||
                    !reader.u64(value.configuration_hash) ||
                    !reader.u32(value.configuration_version) ||
                    !reader.u32(reserved_b) ||
                    !readSize(reader, value.capability_first) ||
                    !readSize(reader, value.capability_count) ||
                    !readSize(reader, value.point_first) ||
                    !readSize(reader, value.point_count) ||
                    !readSize(reader, value.event_first) ||
                    !readSize(reader, value.event_count) ||
                    value.name >= draft_values.strings.size() ||
                    value.hash != lux::cxx::Fnv1a64::hash(
                        draft_values.strings[value.name]
                    ) || value.version == 0U || reserved_a != 0U ||
                    reserved_b != 0U ||
                    value.capability_first != capability_cursor ||
                    value.point_first != point_cursor ||
                    value.event_first != event_cursor ||
                    !addSize(capability_cursor, value.capability_count) ||
                    !addSize(point_cursor, value.point_count) ||
                    !addSize(event_cursor, value.event_count))
                {
                    return false;
                }
                if (value.configuration_schema == kNoString)
                {
                    if (value.configuration_hash != 0U ||
                        value.configuration_version != 0U)
                    {
                        return false;
                    }
                }
                else if (value.configuration_schema >=
                             draft_values.strings.size() ||
                         value.configuration_hash != lux::cxx::Fnv1a64::hash(
                             draft_values.strings[
                                 value.configuration_schema]) ||
                         value.configuration_version == 0U)
                {
                    return false;
                }
                if (!draft.types.empty())
                {
                    const auto& previous = draft.types.back();
                    if (std::tie(previous.hash, previous.name) >=
                        std::tie(value.hash, value.name))
                    {
                        return false;
                    }
                }
                draft.types.push_back(value);
            }
            return reader.remaining() == 0U &&
                capability_cursor == draft.capabilities.size() &&
                point_cursor == draft.points.size() &&
                event_cursor == draft.events.size();
        }

        [[nodiscard]] bool parseEvents(
            std::span<const std::byte> bytes,
            std::size_t count,
            const Draft& strings,
            Draft& draft
        )
        {
            WireReader reader(bytes);
            draft.events.reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                DraftEvent value;
                std::uint32_t reserved{};
                if (!readSize(reader, value.name) ||
                    !readSize(reader, value.point) ||
                    !readSize(reader, value.payload_schema) ||
                    !reader.u64(value.payload_hash) ||
                    !reader.u32(value.payload_version) ||
                    !reader.u32(reserved) ||
                    value.name >= strings.strings.size() || reserved != 0U)
                {
                    return false;
                }
                if (value.payload_schema == kNoString)
                {
                    if (value.payload_hash != 0U || value.payload_version != 0U)
                        return false;
                }
                else if (value.payload_schema >= strings.strings.size() ||
                         value.payload_hash != lux::cxx::Fnv1a64::hash(
                             strings.strings[value.payload_schema]) ||
                         value.payload_version == 0U)
                {
                    return false;
                }
                draft.events.push_back(value);
            }
            return reader.remaining() == 0U;
        }

        [[nodiscard]] bool parseInstances(
            std::span<const std::byte> bytes,
            std::size_t count,
            const Draft& values,
            std::size_t initial_payload_cursor,
            Draft& draft
        )
        {
            WireReader reader(bytes);
            draft.instances.reserve(count);
            std::size_t payload_cursor = initial_payload_cursor;
            for (std::size_t index{}; index < count; ++index)
            {
                DraftInstance value;
                if (!readSize(reader, value.name) ||
                    !readSize(reader, value.type) ||
                    !readSize(reader, value.configuration_offset) ||
                    !readSize(reader, value.configuration_size) ||
                    value.name >= values.strings.size() ||
                    value.type >= values.types.size() ||
                    value.configuration_offset != payload_cursor ||
                    !addSize(payload_cursor, value.configuration_size))
                {
                    return false;
                }
                if (!draft.instances.empty() &&
                    values.strings[draft.instances.back().name] >=
                        values.strings[value.name])
                {
                    return false;
                }
                draft.instances.push_back(value);
            }
            return reader.remaining() == 0U;
        }

        [[nodiscard]] bool parseDependencies(
            std::span<const std::byte> bytes,
            std::size_t count,
            const Draft& values,
            Draft& draft
        )
        {
            WireReader reader(bytes);
            draft.dependencies.reserve(count);
            for (std::size_t index{}; index < count; ++index)
            {
                DraftDependency value;
                if (!readSize(reader, value.before_system) ||
                    !readSize(reader, value.before_point) ||
                    !readSize(reader, value.after_system) ||
                    !readSize(reader, value.after_point) ||
                    value.before_system >= values.instances.size() ||
                    value.after_system >= values.instances.size())
                {
                    return false;
                }
                const auto& before_type = values.types[
                    values.instances[value.before_system].type];
                const auto& after_type = values.types[
                    values.instances[value.after_system].type];
                if (value.before_point >= before_type.point_count ||
                    value.after_point >= after_type.point_count)
                {
                    return false;
                }
                if (!draft.dependencies.empty())
                {
                    const auto& previous = draft.dependencies.back();
                    const auto key = [&](const DraftDependency& dependency)
                    {
                        const auto& before =
                            values.instances[dependency.before_system];
                        const auto& after =
                            values.instances[dependency.after_system];
                        const auto& before_meta = values.types[before.type];
                        const auto& after_meta = values.types[after.type];
                        return std::tuple{
                            std::string_view{values.strings[before.name]},
                            std::string_view{values.strings[values.points[
                                before_meta.point_first +
                                dependency.before_point]]},
                            std::string_view{values.strings[after.name]},
                            std::string_view{values.strings[values.points[
                                after_meta.point_first +
                                dependency.after_point]]}};
                    };
                    if (key(previous) >= key(value))
                        return false;
                }
                draft.dependencies.push_back(value);
            }
            return reader.remaining() == 0U;
        }

        [[nodiscard]] bool validateTypeRanges(const Draft& draft) noexcept
        {
            for (const auto& type : draft.types)
            {
                for (std::size_t index{}; index < type.capability_count; ++index)
                {
                    const auto current = draft.capabilities[
                        type.capability_first + index];
                    for (std::size_t previous{}; previous < index; ++previous)
                    {
                        if (current == draft.capabilities[
                                type.capability_first + previous])
                        {
                            return false;
                        }
                    }
                }
                for (std::size_t index{}; index < type.point_count; ++index)
                {
                    const auto current = draft.points[type.point_first + index];
                    for (std::size_t previous{}; previous < index; ++previous)
                    {
                        if (current == draft.points[type.point_first + previous])
                            return false;
                    }
                }
                for (std::size_t index{}; index < type.event_count; ++index)
                {
                    const auto& current = draft.events[type.event_first + index];
                    if (current.point >= type.point_count)
                        return false;
                    for (std::size_t previous{}; previous < index; ++previous)
                    {
                        if (current.name ==
                            draft.events[type.event_first + previous].name)
                        {
                            return false;
                        }
                    }
                }
            }
            return true;
        }

        [[nodiscard]] lux::cxx::expected<SimulationDescription,
                                         lux::asset::EAssetCodecError>
        materializeDraft(const Draft& draft) noexcept
        {
            SimulationDescriptionBuilder builder;
            for (const auto& data : draft.globals)
            {
                const SimulationDataSchemaId schema{
                    data.hash,
                    draft.strings[data.schema]};
                if (!builder.addData(
                        schema,
                        data.version,
                        draft.payload.subspan(
                            data.payload_offset,
                            data.payload_size
                        )
                    ))
                {
                    return codecFailure();
                }
            }

            for (const auto& instance : draft.instances)
            {
                const auto& type = draft.types[instance.type];
                std::vector<std::string_view> capabilities;
                std::vector<SystemExecutionPoint> points;
                std::vector<SystemEventDescription> events;
                capabilities.reserve(type.capability_count);
                points.reserve(type.point_count);
                events.reserve(type.event_count);
                for (std::size_t index{}; index < type.capability_count; ++index)
                {
                    capabilities.push_back(draft.strings[draft.capabilities[
                        type.capability_first + index]]);
                }
                for (std::size_t index{}; index < type.point_count; ++index)
                {
                    points.push_back(SystemExecutionPoint{
                        draft.strings[draft.points[type.point_first + index]]});
                }
                for (std::size_t index{}; index < type.event_count; ++index)
                {
                    const auto& event = draft.events[type.event_first + index];
                    const bool empty_payload = event.payload_schema == kNoString;
                    const std::string_view payload_name = empty_payload
                        ? std::string_view{}
                        : std::string_view{draft.strings[event.payload_schema]};
                    events.push_back(SystemEventDescription{
                        draft.strings[event.name],
                        points[event.point].name,
                        payload_name,
                        event.payload_version,
                        empty_payload
                            ? lux::cxx::typeToken<void>()
                            : lux::cxx::TypeToken{
                                  event.payload_hash,
                                  payload_name}});
                }
                const std::string_view configuration_schema =
                    type.configuration_schema == kNoString
                    ? std::string_view{}
                    : std::string_view{
                          draft.strings[type.configuration_schema]};
                const SystemDescription description{
                    draft.strings[type.name],
                    type.version,
                    configuration_schema,
                    type.configuration_version,
                    capabilities,
                    points,
                    events};
                if (!builder.addSystem(
                        draft.strings[instance.name],
                        description,
                        draft.payload.subspan(
                            instance.configuration_offset,
                            instance.configuration_size
                        )
                    ))
                {
                    return codecFailure();
                }
            }

            for (const auto& dependency : draft.dependencies)
            {
                const auto& before = draft.instances[dependency.before_system];
                const auto& after = draft.instances[dependency.after_system];
                const auto& before_type = draft.types[before.type];
                const auto& after_type = draft.types[after.type];
                if (!builder.addDependency(
                        draft.strings[before.name],
                        draft.strings[draft.points[
                            before_type.point_first + dependency.before_point]],
                        draft.strings[after.name],
                        draft.strings[draft.points[
                            after_type.point_first + dependency.after_point]]
                    ))
                {
                    return codecFailure();
                }
            }
            auto built = std::move(builder).build();
            if (!built)
                return codecFailure();
            return std::move(*built);
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::DecodedAsset,
            lux::asset::EAssetCodecError>
        decodeSimulation(
            std::span<const std::byte> input,
            const lux::asset::AssetDecodeContext& context
        ) noexcept
        {
            if (input.size() > context.limits.max_input_bytes)
                return codecFailure();
            try
            {
                std::array<Section, kSectionCount> sections;
                if (!parseDirectory(input, sections) ||
                    !fixedSection(sections[1], kGlobalRecordBytes) ||
                    !fixedSection(sections[2], kSystemTypeRecordBytes) ||
                    !fixedSection(sections[3], kInstanceRecordBytes) ||
                    !fixedSection(sections[4], kOrdinalRecordBytes) ||
                    !fixedSection(sections[5], kOrdinalRecordBytes) ||
                    !fixedSection(sections[6], kEventRecordBytes) ||
                    !fixedSection(sections[7], kDependencyRecordBytes) ||
                    sections[8].count != sections[8].size)
                {
                    return codecFailure();
                }

                std::size_t decoded_bytes = sizeof(SimulationDescription);
                if (!addSize(decoded_bytes, input.size()))
                    return codecFailure();
                constexpr std::array overheads{
                    sizeof(std::string), sizeof(DraftGlobal),
                    sizeof(DraftType), sizeof(DraftInstance),
                    sizeof(std::size_t), sizeof(std::size_t),
                    sizeof(DraftEvent), sizeof(DraftDependency)};
                for (std::size_t index{}; index < overheads.size(); ++index)
                {
                    std::size_t value{};
                    if (!multiplySize(sections[index].count, overheads[index], value) ||
                        !addSize(decoded_bytes, value))
                    {
                        return codecFailure();
                    }
                }
                if (decoded_bytes > context.limits.max_decoded_bytes)
                    return codecFailure();

                Draft draft;
                if (!parseStrings(
                        sectionBytes(input, sections[0]),
                        sections[0].count,
                        draft
                    ) ||
                    !parseGlobals(
                        sectionBytes(input, sections[1]),
                        sections[1].count,
                        draft,
                        draft
                    ) ||
                    !parseOrdinals(
                        sectionBytes(input, sections[4]),
                        sections[4].count,
                        draft.strings.size(),
                        draft.capabilities
                    ) ||
                    !parseOrdinals(
                        sectionBytes(input, sections[5]),
                        sections[5].count,
                        draft.strings.size(),
                        draft.points
                    ) ||
                    !parseEvents(
                        sectionBytes(input, sections[6]),
                        sections[6].count,
                        draft,
                        draft
                    ) ||
                    !parseTypes(
                        sectionBytes(input, sections[2]),
                        sections[2].count,
                        draft,
                        draft
                    ) ||
                    !validateTypeRanges(draft))
                {
                    return codecFailure();
                }

                std::size_t global_payload_bytes{};
                for (const auto& global : draft.globals)
                {
                    if (!addSize(global_payload_bytes, global.payload_size))
                        return codecFailure();
                }
                if (!parseInstances(
                        sectionBytes(input, sections[3]),
                        sections[3].count,
                        draft,
                        global_payload_bytes,
                        draft
                    ) ||
                    !parseDependencies(
                        sectionBytes(input, sections[7]),
                        sections[7].count,
                        draft,
                        draft
                    ))
                {
                    return codecFailure();
                }
                std::size_t payload_partition = global_payload_bytes;
                for (const auto& instance : draft.instances)
                {
                    if (!addSize(
                            payload_partition,
                            instance.configuration_size
                        ))
                    {
                        return codecFailure();
                    }
                }
                if (payload_partition != sections[8].size)
                    return codecFailure();
                draft.payload = sectionBytes(input, sections[8]);

                auto description = materializeDraft(draft);
                if (!description ||
                    description->retainedBytes() >
                        context.limits.max_decoded_bytes)
                {
                    return codecFailure();
                }
                const auto retained_bytes = description->retainedBytes();
                auto payload = std::make_shared<const SimulationDescription>(
                    std::move(*description)
                );
                return lux::asset::DecodedAsset{
                    std::move(payload),
                    retained_bytes};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetCodecError::OUT_OF_MEMORY
                );
            }
            catch (...)
            {
                return codecFailure();
            }
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<std::byte>,
            lux::asset::EAssetCodecError>
        encodeSimulation(
            const void* payload,
            const lux::asset::AssetEncodeContext& context
        ) noexcept
        {
            if (payload == nullptr)
                return codecFailure();
            const auto& description =
                *static_cast<const SimulationDescription*>(payload);
            try
            {
                std::vector<SimulationSystemView> types;
                types.reserve(description.systemCount());
                for (std::size_t index{}; index < description.systemCount(); ++index)
                {
                    const auto system = description.systemAt(index);
                    if (std::none_of(
                            types.begin(),
                            types.end(),
                            [&](SimulationSystemView candidate) noexcept
                            {
                                return candidate.type() == system.type();
                            }
                        ))
                    {
                        types.push_back(system);
                    }
                }
                std::sort(
                    types.begin(),
                    types.end(),
                    [](SimulationSystemView left,
                       SimulationSystemView right) noexcept
                    {
                        return SystemTypeIdLess{}(left.type(), right.type());
                    }
                );

                StringTable strings;
                for (const auto& schema : description.schemas())
                    strings.add(schema.name);
                for (const auto type : types)
                {
                    strings.add(type.type().name);
                    strings.add(type.configurationSchemaName());
                    for (std::size_t index{}; index < type.capabilityCount(); ++index)
                        strings.add(type.capabilityAt(index));
                    for (std::size_t index{}; index < type.executionPointCount(); ++index)
                        strings.add(type.executionPointAt(index).name());
                    for (std::size_t index{}; index < type.eventCount(); ++index)
                    {
                        const auto event = type.eventAt(index);
                        strings.add(event.name());
                        strings.add(event.payloadSchemaName());
                    }
                }
                for (std::size_t index{}; index < description.systemCount(); ++index)
                    strings.add(description.systemAt(index).instanceName());
                strings.canonicalize();

                std::size_t capability_count{};
                std::size_t point_count{};
                std::size_t event_count{};
                for (const auto type : types)
                {
                    if (!addSize(capability_count, type.capabilityCount()) ||
                        !addSize(point_count, type.executionPointCount()) ||
                        !addSize(event_count, type.eventCount()))
                    {
                        return codecFailure();
                    }
                }

                std::size_t string_bytes{};
                for (const auto value : strings.values)
                {
                    if (!addSize(string_bytes, sizeof(std::uint64_t)) ||
                        !addSize(string_bytes, value.size()))
                    {
                        return codecFailure();
                    }
                }
                std::size_t payload_bytes = description.payloadBytes();
                if (!addSize(
                        payload_bytes,
                        description.configurationPayloadBytes()
                    ))
                {
                    return codecFailure();
                }
                std::array<Section, kSectionCount> sections{
                    Section{ESection::STRINGS, 0U, string_bytes, strings.values.size()},
                    Section{ESection::GLOBAL_DATA, 0U, 0U, description.dataCount()},
                    Section{ESection::SYSTEM_TYPES, 0U, 0U, types.size()},
                    Section{ESection::INSTANCES, 0U, 0U, description.systemCount()},
                    Section{ESection::CAPABILITIES, 0U, 0U, capability_count},
                    Section{ESection::EXECUTION_POINTS, 0U, 0U, point_count},
                    Section{ESection::EVENTS, 0U, 0U, event_count},
                    Section{ESection::DEPENDENCIES, 0U, 0U, description.dependencyCount()},
                    Section{ESection::PAYLOAD, 0U, payload_bytes, payload_bytes}};
                constexpr std::array<std::size_t, kSectionCount> strides{
                    0U,
                    kGlobalRecordBytes,
                    kSystemTypeRecordBytes,
                    kInstanceRecordBytes,
                    kOrdinalRecordBytes,
                    kOrdinalRecordBytes,
                    kEventRecordBytes,
                    kDependencyRecordBytes,
                    0U};
                for (std::size_t index = 1U; index + 1U < sections.size(); ++index)
                {
                    if (!multiplySize(
                            sections[index].count,
                            strides[index],
                            sections[index].size
                        ))
                    {
                        return codecFailure();
                    }
                }
                std::size_t encoded_bytes =
                    kHeaderBytes + kSectionCount * kDirectoryEntryBytes;
                for (auto& section : sections)
                {
                    section.offset = encoded_bytes;
                    if (!addSize(encoded_bytes, section.size))
                        return codecFailure();
                }
                if (encoded_bytes > context.limits.max_encoded_bytes)
                    return codecFailure();

                std::vector<std::byte> bytes;
                bytes.reserve(encoded_bytes);
                WireWriter writer(bytes);
                writer.u32(SimulationAssetPrimaryMagic);
                writer.u32(kWireVersion);
                writer.u32(kSectionCount);
                writer.u32(0U);
                writer.u64(kHeaderBytes);
                writer.u64(encoded_bytes);
                for (const auto& section : sections)
                {
                    writer.u32(static_cast<std::uint32_t>(section.kind));
                    writer.u32(0U);
                    writer.u64(section.offset);
                    writer.u64(section.size);
                    writer.u64(section.count);
                }
                for (const auto value : strings.values)
                    writer.string(value);

                std::size_t payload_cursor{};
                for (std::size_t index{}; index < description.dataCount(); ++index)
                {
                    const auto data = description.dataAt(index);
                    writer.u64(strings.ordinal(data.schema().name));
                    writer.u64(data.schema().hash);
                    writer.u32(data.version());
                    writer.u32(0U);
                    writer.u64(payload_cursor);
                    writer.u64(data.payload().size());
                    payload_cursor += data.payload().size();
                }

                std::size_t capability_first{};
                std::size_t point_first{};
                std::size_t event_first{};
                for (const auto type : types)
                {
                    writer.u64(strings.ordinal(type.type().name));
                    writer.u64(type.type().hash);
                    writer.u32(type.version());
                    writer.u32(0U);
                    writer.u64(type.configurationSchemaName().empty()
                        ? kNoString
                        : strings.ordinal(type.configurationSchemaName()));
                    writer.u64(type.configurationSchemaHash());
                    writer.u32(type.configurationSchemaVersion());
                    writer.u32(0U);
                    writer.u64(capability_first);
                    writer.u64(type.capabilityCount());
                    writer.u64(point_first);
                    writer.u64(type.executionPointCount());
                    writer.u64(event_first);
                    writer.u64(type.eventCount());
                    capability_first += type.capabilityCount();
                    point_first += type.executionPointCount();
                    event_first += type.eventCount();
                }

                for (std::size_t index{}; index < description.systemCount(); ++index)
                {
                    const auto system = description.systemAt(index);
                    const auto type = std::lower_bound(
                        types.begin(),
                        types.end(),
                        system.type(),
                        [](SimulationSystemView candidate,
                           const SystemTypeId& id) noexcept
                        {
                            return SystemTypeIdLess{}(candidate.type(), id);
                        }
                    );
                    writer.u64(strings.ordinal(system.instanceName()));
                    writer.u64(static_cast<std::size_t>(
                        std::distance(types.begin(), type)
                    ));
                    writer.u64(payload_cursor);
                    writer.u64(system.configurationPayload().size());
                    payload_cursor += system.configurationPayload().size();
                }

                for (const auto type : types)
                {
                    for (std::size_t index{}; index < type.capabilityCount(); ++index)
                        writer.u64(strings.ordinal(type.capabilityAt(index)));
                }
                for (const auto type : types)
                {
                    for (std::size_t index{}; index < type.executionPointCount(); ++index)
                        writer.u64(strings.ordinal(type.executionPointAt(index).name()));
                }
                for (const auto type : types)
                {
                    for (std::size_t index{}; index < type.eventCount(); ++index)
                    {
                        const auto event = type.eventAt(index);
                        writer.u64(strings.ordinal(event.name()));
                        std::size_t point{};
                        while (type.executionPointAt(point).name() !=
                               event.dispatchPoint().name())
                        {
                            ++point;
                        }
                        writer.u64(point);
                        writer.u64(event.payloadSchemaName().empty()
                            ? kNoString
                            : strings.ordinal(event.payloadSchemaName()));
                        writer.u64(event.payloadSchemaHash());
                        writer.u32(event.payloadSchemaVersion());
                        writer.u32(0U);
                    }
                }
                for (std::size_t index{}; index < description.dependencyCount(); ++index)
                {
                    const auto dependency = description.dependencyAt(index);
                    const auto before_system = dependency.before().system();
                    const auto after_system = dependency.after().system();
                    std::size_t before_ordinal{};
                    while (description.systemAt(before_ordinal).instanceName() !=
                           before_system.instanceName())
                    {
                        ++before_ordinal;
                    }
                    std::size_t after_ordinal{};
                    while (description.systemAt(after_ordinal).instanceName() !=
                           after_system.instanceName())
                    {
                        ++after_ordinal;
                    }
                    std::size_t before_point{};
                    while (before_system.executionPointAt(before_point).name() !=
                           dependency.before().name())
                    {
                        ++before_point;
                    }
                    std::size_t after_point{};
                    while (after_system.executionPointAt(after_point).name() !=
                           dependency.after().name())
                    {
                        ++after_point;
                    }
                    writer.u64(before_ordinal);
                    writer.u64(before_point);
                    writer.u64(after_ordinal);
                    writer.u64(after_point);
                }

                for (std::size_t index{}; index < description.dataCount(); ++index)
                    writer.bytes(description.dataAt(index).payload());
                for (std::size_t index{}; index < description.systemCount(); ++index)
                    writer.bytes(description.systemAt(index).configurationPayload());
                if (!writer.ok() || bytes.size() != encoded_bytes)
                    return codecFailure();
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetCodecError::OUT_OF_MEMORY
                );
            }
            catch (...)
            {
                return codecFailure();
            }
        }
    }

    lux::asset::AssetCodecDescriptor simulationAssetCodecDescriptor(
        std::shared_ptr<const void> code_lifetime
    )
    {
        return lux::asset::AssetCodecDescriptor{
            lux::asset::AssetTypeId::fromName(SimulationAssetCanonicalName),
            std::string(SimulationAssetCanonicalName),
            SimulationAssetPrimaryMagic,
            0U,
            lux::cxx::typeToken<SimulationDescription>(),
            &decodeSimulation,
            &encodeSimulation,
            std::move(code_lifetime)};
    }
}
