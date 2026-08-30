#include <lux/engine/simulation/SimulationAssetCodec.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::simulation
{
    using lux::asset::EAssetCodecError;

    namespace detail
    {
        constexpr std::uint32_t kWireVersion{5U};
        constexpr std::uint32_t kSectionCount{10U};
        constexpr std::uint32_t kDirectoryEntryBytes{24U};
        constexpr std::uint64_t kHeaderBytes{32U};
        constexpr std::uint64_t kDirectoryOffset{kHeaderBytes};
        constexpr std::uint64_t kSectionDataOffset{
            kDirectoryOffset + kSectionCount * kDirectoryEntryBytes};
        constexpr std::uint32_t kNoString{
            std::numeric_limits<std::uint32_t>::max()};

        enum class ESection : std::uint32_t
        {
            STRINGS = 1U,
            GLOBAL_DATA,
            SYSTEM_TYPES,
            INSTANCES,
            CAPABILITIES,
            HOOKS,
            SIGNATURE_TYPES,
            EVENTS,
            SYSTEM_DEPENDENCIES,
            PAYLOAD,
        };

        constexpr std::array<std::uint32_t, kSectionCount> kRecordBytes{
            0U, 32U, 56U, 32U, 4U, 24U, 16U, 40U, 8U, 0U};

        class Bytes final
        {
          public:
            void u8(std::uint8_t value)
            {
                values.push_back(static_cast<std::byte>(value));
            }

            void u32(std::uint32_t value)
            {
                for (std::uint32_t shift{}; shift < 32U; shift += 8U)
                    u8(static_cast<std::uint8_t>(value >> shift));
            }

            void u64(std::uint64_t value)
            {
                for (std::uint32_t shift{}; shift < 64U; shift += 8U)
                    u8(static_cast<std::uint8_t>(value >> shift));
            }

            void raw(std::span<const std::byte> source)
            {
                values.insert(values.end(), source.begin(), source.end());
            }

            void raw(std::string_view source)
            {
                const auto* first =
                    reinterpret_cast<const std::byte*>(source.data());
                values.insert(values.end(), first, first + source.size());
            }

            std::vector<std::byte> values;
        };

        [[nodiscard]] bool readU8(
            std::span<const std::byte> bytes,
            std::size_t& offset,
            std::uint8_t& value
        ) noexcept
        {
            if (offset >= bytes.size())
                return false;
            value = std::to_integer<std::uint8_t>(bytes[offset++]);
            return true;
        }

        [[nodiscard]] bool readU32(
            std::span<const std::byte> bytes,
            std::size_t& offset,
            std::uint32_t& value
        ) noexcept
        {
            value = 0U;
            for (std::uint32_t shift{}; shift < 32U; shift += 8U)
            {
                std::uint8_t byte{};
                if (!readU8(bytes, offset, byte))
                    return false;
                value |= static_cast<std::uint32_t>(byte) << shift;
            }
            return true;
        }

        [[nodiscard]] bool readU64(
            std::span<const std::byte> bytes,
            std::size_t& offset,
            std::uint64_t& value
        ) noexcept
        {
            value = 0U;
            for (std::uint32_t shift{}; shift < 64U; shift += 8U)
            {
                std::uint8_t byte{};
                if (!readU8(bytes, offset, byte))
                    return false;
                value |= static_cast<std::uint64_t>(byte) << shift;
            }
            return true;
        }

        [[nodiscard]] bool rangeValid(
            std::uint32_t first,
            std::uint32_t count,
            std::size_t total
        ) noexcept
        {
            return first <= total && count <= total - first;
        }

        class StringTable final
        {
          public:
            void add(std::string_view value)
            {
                if (!value.empty())
                    strings_.emplace_back(value);
            }

            void finish()
            {
                std::sort(strings_.begin(), strings_.end());
                strings_.erase(
                    std::unique(strings_.begin(), strings_.end()),
                    strings_.end()
                );
            }

            [[nodiscard]] std::uint32_t ordinal(std::string_view value) const noexcept
            {
                if (value.empty())
                    return kNoString;
                const auto found = std::lower_bound(
                    strings_.begin(),
                    strings_.end(),
                    value
                );
                if (found == strings_.end() || *found != value)
                {
                    valid_ = false;
                    return kNoString;
                }
                return static_cast<std::uint32_t>(
                    std::distance(strings_.begin(), found));
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return valid_;
            }

            [[nodiscard]] Bytes encode() const
            {
                Bytes result;
                result.u32(static_cast<std::uint32_t>(strings_.size()));
                for (const auto& value : strings_)
                {
                    result.u32(static_cast<std::uint32_t>(value.size()));
                    result.raw(value);
                }
                return result;
            }

          private:
            std::vector<std::string> strings_;
            mutable bool valid_{true};
        };

        struct TypeSource final
        {
            SystemTypeId type;
            SimulationSystemView system;
        };

        void collectStrings(
            const SimulationDescription& description,
            StringTable& strings,
            std::vector<TypeSource>& types
        )
        {
            for (std::size_t index{}; index < description.dataCount(); ++index)
                strings.add(description.dataAt(index).schema().name);
            for (std::size_t index{}; index < description.systemCount(); ++index)
            {
                const auto system = description.systemAt(index);
                strings.add(system.instanceName());
                if (std::none_of(
                        types.begin(),
                        types.end(),
                        [&](const auto& value) noexcept
                        {
                            return value.type == system.type();
                        }))
                {
                    types.push_back({system.type(), system});
                }
            }
            std::sort(
                types.begin(),
                types.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    return SystemTypeIdLess{}(left.type, right.type);
                }
            );
            for (const auto& source : types)
            {
                const auto system = source.system;
                strings.add(source.type.name);
                strings.add(system.configurationSchemaName());
                for (std::size_t index{}; index < system.capabilityCount(); ++index)
                    strings.add(system.capabilityAt(index));
                for (std::size_t index{}; index < system.hookPointCount(); ++index)
                {
                    const auto hook = system.hookPointAt(index);
                    strings.add(hook.name());
                    for (std::size_t item{}; item < hook.parameterCount(); ++item)
                        strings.add(hook.parameterAt(item).canonical_name);
                }
                for (std::size_t index{}; index < system.eventCount(); ++index)
                {
                    strings.add(system.eventAt(index).name());
                    strings.add(system.eventAt(index).payloadSchemaName());
                }
            }
            strings.finish();
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<std::byte>,
            EAssetCodecError>
        encodeDescription(
            const SimulationDescription& description,
            std::size_t max_encoded_bytes
        ) noexcept
        {
            try
            {
                StringTable strings;
                std::vector<TypeSource> types;
                collectStrings(description, strings, types);

                std::array<Bytes, kSectionCount> sections;
                sections[0] = strings.encode();
                auto& data = sections[1];
                auto& system_types = sections[2];
                auto& instances = sections[3];
                auto& capabilities = sections[4];
                auto& hooks = sections[5];
                auto& signatures = sections[6];
                auto& events = sections[7];
                auto& dependencies = sections[8];
                auto& binary_payload = sections[9];

                for (std::size_t index{}; index < description.dataCount(); ++index)
                {
                    const auto value = description.dataAt(index);
                    data.u64(value.schema().hash);
                    data.u32(strings.ordinal(value.schema().name));
                    data.u32(value.version());
                    data.u64(binary_payload.values.size());
                    data.u64(value.payload().size());
                    binary_payload.raw(value.payload());
                }

                for (const auto& source : types)
                {
                    const auto system = source.system;
                    const auto capability_first = static_cast<std::uint32_t>(
                        capabilities.values.size() / kRecordBytes[4]);
                    for (std::size_t index{}; index < system.capabilityCount(); ++index)
                        capabilities.u32(strings.ordinal(system.capabilityAt(index)));

                    const auto hook_first = static_cast<std::uint32_t>(
                        hooks.values.size() / kRecordBytes[5]);
                    for (std::size_t index{}; index < system.hookPointCount(); ++index)
                    {
                        const auto hook = system.hookPointAt(index);
                        const auto parameter_first = static_cast<std::uint32_t>(
                            signatures.values.size() / kRecordBytes[6]);
                        for (std::size_t item{}; item < hook.parameterCount(); ++item)
                        {
                            const auto type = hook.parameterAt(item);
                            signatures.u64(type.type_id);
                            signatures.u32(strings.ordinal(type.canonical_name));
                            signatures.u32(static_cast<std::uint32_t>(type.pass));
                        }
                        hooks.u64(hook.id().value);
                        hooks.u32(strings.ordinal(hook.name()));
                        hooks.u32(parameter_first);
                        hooks.u32(static_cast<std::uint32_t>(
                            hook.parameterCount()));
                        hooks.u32(0U);
                    }

                    const auto event_first = static_cast<std::uint32_t>(
                        events.values.size() / kRecordBytes[7]);
                    for (std::size_t index{}; index < system.eventCount(); ++index)
                    {
                        const auto event = system.eventAt(index);
                        std::uint32_t local_hook{};
                        while (local_hook < system.hookPointCount() &&
                               system.hookPointAt(local_hook).id() !=
                                   event.dispatchHook().id())
                        {
                            ++local_hook;
                        }
                        events.u64(event.id().value);
                        events.u32(strings.ordinal(event.name()));
                        events.u32(hook_first + local_hook);
                        events.u32(static_cast<std::uint32_t>(event.route()));
                        events.u32(strings.ordinal(event.payloadSchemaName()));
                        events.u32(event.payloadSchemaVersion());
                        events.u64(event.payloadType());
                        events.u32(0U);
                    }

                    system_types.u64(source.type.hash);
                    system_types.u32(strings.ordinal(source.type.name));
                    system_types.u32(system.version());
                    system_types.u32(strings.ordinal(
                        system.configurationSchemaName()));
                    system_types.u32(system.configurationSchemaVersion());
                    system_types.u64(system.configurationSchemaHash());
                    system_types.u32(capability_first);
                    system_types.u32(static_cast<std::uint32_t>(
                        system.capabilityCount()));
                    system_types.u32(hook_first);
                    system_types.u32(static_cast<std::uint32_t>(
                        system.hookPointCount()));
                    system_types.u32(event_first);
                    system_types.u32(static_cast<std::uint32_t>(
                        system.eventCount()));
                }

                for (std::size_t index{}; index < description.systemCount(); ++index)
                {
                    const auto system = description.systemAt(index);
                    const auto type = std::lower_bound(
                        types.begin(),
                        types.end(),
                        system.type(),
                        [](const auto& value, const SystemTypeId& id) noexcept
                        {
                            return SystemTypeIdLess{}(value.type, id);
                        }
                    );
                    instances.u64(system.instanceId().value);
                    instances.u32(strings.ordinal(system.instanceName()));
                    instances.u32(static_cast<std::uint32_t>(
                        std::distance(types.begin(), type)));
                    instances.u64(binary_payload.values.size());
                    instances.u64(system.configurationPayload().size());
                    binary_payload.raw(system.configurationPayload());
                }

                for (std::size_t index{}; index < description.dependencyCount(); ++index)
                {
                    const auto dependency = description.dependencyAt(index);
                    std::uint32_t before{}, after{};
                    for (std::size_t system{};
                         system < description.systemCount(); ++system)
                    {
                        const auto candidate = description.systemAt(system);
                        if (candidate.instanceId() ==
                            dependency.before().instanceId())
                        {
                            before = static_cast<std::uint32_t>(system);
                        }
                        if (candidate.instanceId() ==
                            dependency.after().instanceId())
                        {
                            after = static_cast<std::uint32_t>(system);
                        }
                    }
                    dependencies.u32(before);
                    dependencies.u32(after);
                }

                if (!strings.valid())
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);

                Bytes output;
                output.u32(SimulationAssetPrimaryMagic);
                output.u32(kWireVersion);
                output.u32(kSectionCount);
                output.u32(kDirectoryEntryBytes);
                output.u64(kDirectoryOffset);
                std::uint64_t total_size{kSectionDataOffset};
                for (const auto& section : sections)
                {
                    if (section.values.size() >
                        std::numeric_limits<std::uint64_t>::max() - total_size)
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    total_size += section.values.size();
                }
                output.u64(total_size);
                std::uint64_t offset{kSectionDataOffset};
                for (std::size_t index{}; index < sections.size(); ++index)
                {
                    output.u32(static_cast<std::uint32_t>(index + 1U));
                    output.u32(kRecordBytes[index]);
                    output.u64(offset);
                    output.u64(sections[index].values.size());
                    offset += sections[index].values.size();
                }
                for (const auto& section : sections)
                    output.raw(section.values);
                if (output.values.size() != total_size ||
                    output.values.size() > max_encoded_bytes)
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                return std::move(output.values);
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EAssetCodecError::OUT_OF_MEMORY);
            }
            catch (...)
            {
                return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
            }
        }

        struct SectionView final
        {
            std::uint32_t record_bytes{};
            std::span<const std::byte> bytes;
        };

        struct TypeWire final
        {
            std::uint64_t hash{};
            std::uint32_t name{};
            std::uint32_t version{};
            std::uint32_t configuration_name{};
            std::uint32_t configuration_version{};
            std::uint64_t configuration_hash{};
            std::uint32_t capability_first{};
            std::uint32_t capability_count{};
            std::uint32_t hook_first{};
            std::uint32_t hook_count{};
            std::uint32_t event_first{};
            std::uint32_t event_count{};
        };

        struct InstanceWire final
        {
            SystemInstanceId id;
            std::uint32_t name{};
            std::uint32_t type{};
            std::uint64_t payload_offset{};
            std::uint64_t payload_size{};
        };

        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const SimulationDescription>, EAssetCodecError>
        decodeDescription(
            std::span<const std::byte> bytes,
            std::size_t max_input_bytes,
            std::size_t max_decoded_bytes
        ) noexcept
        {
            if (bytes.size() > max_input_bytes ||
                bytes.size() < kSectionDataOffset)
            {
                return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
            }
            try
            {
                std::size_t cursor{};
                std::uint32_t magic{}, version{}, section_count{}, entry_bytes{};
                std::uint64_t directory_offset{}, total_size{};
                if (!readU32(bytes, cursor, magic) ||
                    !readU32(bytes, cursor, version) ||
                    !readU32(bytes, cursor, section_count) ||
                    !readU32(bytes, cursor, entry_bytes) ||
                    !readU64(bytes, cursor, directory_offset) ||
                    !readU64(bytes, cursor, total_size) ||
                    magic != SimulationAssetPrimaryMagic ||
                    version != kWireVersion || section_count != kSectionCount ||
                    entry_bytes != kDirectoryEntryBytes ||
                    directory_offset != kDirectoryOffset ||
                    total_size != bytes.size())
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }

                std::array<SectionView, kSectionCount> sections;
                std::uint64_t expected_offset{kSectionDataOffset};
                for (std::size_t index{}; index < sections.size(); ++index)
                {
                    std::uint32_t kind{}, record_bytes{};
                    std::uint64_t offset{}, size{};
                    if (!readU32(bytes, cursor, kind) ||
                        !readU32(bytes, cursor, record_bytes) ||
                        !readU64(bytes, cursor, offset) ||
                        !readU64(bytes, cursor, size) ||
                        kind != index + 1U ||
                        record_bytes != kRecordBytes[index] ||
                        offset != expected_offset || size > bytes.size() - offset ||
                        (record_bytes != 0U && size % record_bytes != 0U))
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    sections[index] = {
                        record_bytes,
                        bytes.subspan(
                            static_cast<std::size_t>(offset),
                            static_cast<std::size_t>(size))};
                    expected_offset += size;
                }
                if (expected_offset != total_size)
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);

                std::size_t estimated{sizeof(SimulationDescription)};
                for (const auto& section : sections)
                {
                    if (estimated > max_decoded_bytes ||
                        section.bytes.size() >
                            max_decoded_bytes - estimated)
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    estimated += section.bytes.size();
                }

                std::vector<std::string_view> strings;
                cursor = 0U;
                std::uint32_t string_count{};
                if (!readU32(sections[0].bytes, cursor, string_count) ||
                    string_count > sections[0].bytes.size())
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                strings.reserve(string_count);
                for (std::uint32_t index{}; index < string_count; ++index)
                {
                    std::uint32_t size{};
                    if (!readU32(sections[0].bytes, cursor, size) || size == 0U ||
                        size > sections[0].bytes.size() - cursor)
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    const std::string_view value{
                        reinterpret_cast<const char*>(
                            sections[0].bytes.data() + cursor),
                        size};
                    if (!strings.empty() && strings.back() >= value)
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    strings.push_back(value);
                    cursor += size;
                }
                if (cursor != sections[0].bytes.size())
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                bool invalid_string_ordinal{};
                const auto stringAt = [&](std::uint32_t ordinal) noexcept -> std::string_view
                {
                    if (ordinal == kNoString)
                        return {};
                    if (ordinal >= strings.size())
                    {
                        invalid_string_ordinal = true;
                        return {};
                    }
                    return strings[ordinal];
                };
                const auto countOf = [&](std::size_t section) noexcept
                {
                    return sections[section].record_bytes == 0U
                        ? std::size_t{}
                        : sections[section].bytes.size() /
                            sections[section].record_bytes;
                };

                const auto capability_count = countOf(4);
                const auto hook_count = countOf(5);
                const auto signature_count = countOf(6);
                const auto event_count = countOf(7);

                std::vector<TypeWire> types;
                types.reserve(countOf(2));
                cursor = 0U;
                for (std::size_t index{}; index < countOf(2); ++index)
                {
                    TypeWire type;
                    if (!readU64(sections[2].bytes, cursor, type.hash) ||
                        !readU32(sections[2].bytes, cursor, type.name) ||
                        !readU32(sections[2].bytes, cursor, type.version) ||
                        !readU32(
                            sections[2].bytes,
                            cursor,
                            type.configuration_name
                        ) ||
                        !readU32(
                            sections[2].bytes,
                            cursor,
                            type.configuration_version
                        ) ||
                        !readU64(
                            sections[2].bytes,
                            cursor,
                            type.configuration_hash
                        ) ||
                        !readU32(
                            sections[2].bytes,
                            cursor,
                            type.capability_first
                        ) ||
                        !readU32(
                            sections[2].bytes,
                            cursor,
                            type.capability_count
                        ) ||
                        !readU32(sections[2].bytes, cursor, type.hook_first) ||
                        !readU32(sections[2].bytes, cursor, type.hook_count) ||
                        !readU32(sections[2].bytes, cursor, type.event_first) ||
                        !readU32(sections[2].bytes, cursor, type.event_count) ||
                        type.version == 0U ||
                        !rangeValid(
                            type.capability_first,
                            type.capability_count,
                            capability_count
                        ) ||
                        !rangeValid(type.hook_first, type.hook_count, hook_count) ||
                        !rangeValid(
                            type.event_first,
                            type.event_count,
                            event_count
                        ))
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    const auto name = stringAt(type.name);
                    const auto configuration = stringAt(type.configuration_name);
                    const SystemTypeId identity{type.hash, std::string(name)};
                    if (!identity.valid() ||
                        configuration.empty() !=
                            (type.configuration_version == 0U) ||
                        (configuration.empty() ? 0U :
                            lux::cxx::Fnv1a64::hash(configuration)) !=
                            type.configuration_hash ||
                        (!types.empty() && !SystemTypeIdLess{}(
                            SystemTypeId{
                                types.back().hash,
                                std::string(stringAt(types.back().name))},
                            identity)))
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    types.push_back(type);
                }

                std::vector<InstanceWire> instances;
                instances.reserve(countOf(3));
                cursor = 0U;
                for (std::size_t index{}; index < countOf(3); ++index)
                {
                    InstanceWire instance;
                    if (!readU64(sections[3].bytes, cursor, instance.id.value) ||
                        !readU32(sections[3].bytes, cursor, instance.name) ||
                        !readU32(sections[3].bytes, cursor, instance.type) ||
                        !readU64(
                            sections[3].bytes,
                            cursor,
                            instance.payload_offset
                        ) ||
                        !readU64(
                            sections[3].bytes,
                            cursor,
                            instance.payload_size
                        ) ||
                        !instance.id.valid() || instance.type >= types.size() ||
                        stringAt(instance.name).empty() ||
                        (!instances.empty() &&
                            instances.back().id >= instance.id))
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    instances.push_back(instance);
                }

                SimulationDescriptionBuilder builder;
                std::uint64_t expected_payload_offset{};
                cursor = 0U;
                for (std::size_t index{}; index < countOf(1); ++index)
                {
                    std::uint64_t schema_hash{}, payload_offset{}, payload_size{};
                    std::uint32_t schema_name{}, schema_version{};
                    if (!readU64(sections[1].bytes, cursor, schema_hash) ||
                        !readU32(sections[1].bytes, cursor, schema_name) ||
                        !readU32(sections[1].bytes, cursor, schema_version) ||
                        !readU64(sections[1].bytes, cursor, payload_offset) ||
                        !readU64(sections[1].bytes, cursor, payload_size) ||
                        payload_offset != expected_payload_offset ||
                        payload_size > sections[9].bytes.size() - payload_offset)
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    const auto name = stringAt(schema_name);
                    if (simulationDataSchemaId(name).hash != schema_hash ||
                        !builder.addData(
                            SimulationDataSchemaId{
                                schema_hash,
                                std::string(name)},
                            schema_version,
                            sections[9].bytes.subspan(
                                static_cast<std::size_t>(payload_offset),
                                static_cast<std::size_t>(payload_size))))
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    expected_payload_offset += payload_size;
                }

                std::vector<std::vector<std::string_view>> capabilities(
                    types.size());
                std::vector<std::vector<HookPointSpec>> hooks(types.size());
                std::vector<std::vector<std::vector<lux::semantic::Type>>>
                    hook_parameters(types.size());
                std::vector<std::vector<EventPointSpec>> events(types.size());

                for (std::size_t type_index{};
                     type_index < types.size(); ++type_index)
                {
                    const auto& type = types[type_index];
                    auto& type_capabilities = capabilities[type_index];
                    type_capabilities.reserve(type.capability_count);
                    for (std::uint32_t item{};
                         item < type.capability_count; ++item)
                    {
                        std::size_t offset = static_cast<std::size_t>(
                            type.capability_first + item) * kRecordBytes[4];
                        std::uint32_t name{};
                        if (!readU32(sections[4].bytes, offset, name))
                            return lux::cxx::unexpected(
                                EAssetCodecError::CODEC_FAILURE);
                        type_capabilities.push_back(stringAt(name));
                    }

                    auto& type_hooks = hooks[type_index];
                    auto& type_parameters = hook_parameters[type_index];
                    type_hooks.reserve(type.hook_count);
                    type_parameters.resize(type.hook_count);
                    for (std::uint32_t item{}; item < type.hook_count; ++item)
                    {
                        std::size_t offset = static_cast<std::size_t>(
                            type.hook_first + item) * kRecordBytes[5];
                        HookPointId id;
                        std::uint32_t name{}, parameter_first{}, parameter_count{},
                            reserved{};
                        if (!readU64(sections[5].bytes, offset, id.value) ||
                            !readU32(sections[5].bytes, offset, name) ||
                            !readU32(
                                sections[5].bytes,
                                offset,
                                parameter_first
                            ) ||
                            !readU32(
                                sections[5].bytes,
                                offset,
                                parameter_count
                            ) ||
                            !readU32(sections[5].bytes, offset, reserved) ||
                            !id.valid() || reserved != 0U ||
                            !rangeValid(
                                parameter_first,
                                parameter_count,
                                signature_count
                            ))
                        {
                            return lux::cxx::unexpected(
                                EAssetCodecError::CODEC_FAILURE);
                        }
                        auto& parameters = type_parameters[item];
                        parameters.reserve(parameter_count);
                        for (std::uint32_t semantic{};
                             semantic < parameter_count; ++semantic)
                        {
                            std::size_t semantic_offset =
                                static_cast<std::size_t>(
                                    parameter_first + semantic) *
                                kRecordBytes[6];
                            lux::semantic::Type value;
                            std::uint32_t type_name{}, pass{};
                            if (!readU64(
                                    sections[6].bytes,
                                    semantic_offset,
                                    value.type_id
                                ) ||
                                !readU32(
                                    sections[6].bytes,
                                    semantic_offset,
                                    type_name
                                ) ||
                                !readU32(
                                    sections[6].bytes,
                                    semantic_offset,
                                    pass
                                ) ||
                                pass > static_cast<std::uint32_t>(
                                    lux::semantic::EValuePass::CONST_REF))
                            {
                                return lux::cxx::unexpected(
                                    EAssetCodecError::CODEC_FAILURE);
                            }
                            value.canonical_name = stringAt(type_name);
                            value.pass = static_cast<lux::semantic::EValuePass>(
                                pass);
                            if (!value.valid())
                                return lux::cxx::unexpected(
                                    EAssetCodecError::CODEC_FAILURE);
                            parameters.push_back(value);
                        }
                        type_hooks.push_back({
                            id,
                            stringAt(name),
                            {parameters, {}}});
                    }

                    auto& type_events = events[type_index];
                    type_events.reserve(type.event_count);
                    for (std::uint32_t item{}; item < type.event_count; ++item)
                    {
                        std::size_t offset = static_cast<std::size_t>(
                            type.event_first + item) * kRecordBytes[7];
                        EventPointId id;
                        std::uint32_t name{}, dispatch{}, route{}, payload_name{},
                            payload_version{}, reserved{};
                        lux::semantic::TypeId payload_type{};
                        if (!readU64(sections[7].bytes, offset, id.value) ||
                            !readU32(sections[7].bytes, offset, name) ||
                            !readU32(sections[7].bytes, offset, dispatch) ||
                            !readU32(sections[7].bytes, offset, route) ||
                            !readU32(
                                sections[7].bytes,
                                offset,
                                payload_name
                            ) ||
                            !readU32(
                                sections[7].bytes,
                                offset,
                                payload_version
                            ) ||
                            !readU64(
                                sections[7].bytes,
                                offset,
                                payload_type
                            ) ||
                            !readU32(sections[7].bytes, offset, reserved) ||
                            !id.valid() || reserved != 0U ||
                            dispatch < type.hook_first ||
                            dispatch >= type.hook_first + type.hook_count ||
                            route > static_cast<std::uint32_t>(
                                EEventRoute::ENTITY_TARGETED))
                        {
                            return lux::cxx::unexpected(
                                EAssetCodecError::CODEC_FAILURE);
                        }
                        const auto payload_schema = stringAt(payload_name);
                        if (payload_schema.empty() || payload_version == 0U ||
                            lux::semantic::typeId(payload_schema) != payload_type)
                        {
                            return lux::cxx::unexpected(
                                EAssetCodecError::CODEC_FAILURE);
                        }
                        type_events.push_back({
                            id,
                            stringAt(name),
                            type_hooks[dispatch - type.hook_first].id,
                            static_cast<EEventRoute>(route),
                            payload_type,
                            payload_schema,
                            payload_version});
                    }
                }

                for (const auto& instance : instances)
                {
                    const auto& type = types[instance.type];
                    if (instance.payload_offset != expected_payload_offset ||
                        instance.payload_size >
                            sections[9].bytes.size() - instance.payload_offset)
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    const SystemDescription system{
                        stringAt(type.name),
                        type.version,
                        stringAt(type.configuration_name),
                        type.configuration_version,
                        capabilities[instance.type],
                        hooks[instance.type],
                        events[instance.type]};
                    if (!builder.addSystem(
                            instance.id,
                            stringAt(instance.name),
                            system,
                            sections[9].bytes.subspan(
                                static_cast<std::size_t>(
                                    instance.payload_offset),
                                static_cast<std::size_t>(instance.payload_size))))
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    expected_payload_offset += instance.payload_size;
                }
                if (expected_payload_offset != sections[9].bytes.size())
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);

                cursor = 0U;
                std::pair<std::uint32_t, std::uint32_t> previous_dependency{};
                for (std::size_t index{}; index < countOf(8); ++index)
                {
                    std::uint32_t before{}, after{};
                    if (!readU32(sections[8].bytes, cursor, before) ||
                        !readU32(sections[8].bytes, cursor, after) ||
                        before >= instances.size() || after >= instances.size() ||
                        (index != 0U && previous_dependency >=
                            std::pair{before, after}) ||
                        !builder.addDependency(
                            instances[before].id,
                            instances[after].id))
                    {
                        return lux::cxx::unexpected(
                            EAssetCodecError::CODEC_FAILURE);
                    }
                    previous_dependency = {before, after};
                }

                if (invalid_string_ordinal)
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);

                auto built = std::move(builder).build();
                if (!built || built->retainedBytes() >
                    max_decoded_bytes)
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                auto description = std::make_shared<SimulationDescription>(
                    std::move(*built));
                return description;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EAssetCodecError::OUT_OF_MEMORY);
            }
            catch (...)
            {
                return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
            }
        }
    } // namespace detail

    SimulationAsset::SimulationAsset(
        lux::asset::AssetInfo info,
        std::shared_ptr<const SimulationDescription> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const SimulationAsset>, lux::asset::AssetDecodeFailure>
    SimulationAsset::create(
        lux::asset::AssetInfo info,
        std::shared_ptr<const SimulationDescription> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data)
        {
            return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                lux::asset::EAssetDecodeError::INVALID_PAYLOAD,
                0U
            });
        }
        info.type = asset_type;
        try
        {
            std::sort(
                auxiliary.begin(),
                auxiliary.end(),
                [](const auto& left, const auto& right) noexcept { return left.tag < right.tag; }
            );
            for (std::size_t index = 0U; index < auxiliary.size(); ++index)
            {
                const bool invalid = auxiliary[index].tag == 0U || auxiliary[index].bytes.empty();
                const bool duplicate = index != 0U && auxiliary[index - 1U].tag == auxiliary[index].tag;
                if (invalid || duplicate)
                {
                    return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                        lux::asset::EAssetDecodeError::INVALID_PAYLOAD,
                        index
                    });
                }
            }
            return std::shared_ptr<const SimulationAsset>(
                new SimulationAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                lux::asset::EAssetDecodeError::ALLOCATION_FAILURE,
                0U
            });
        }
    }
} // namespace lux::simulation

namespace lux::asset
{
    lux::cxx::expected<std::shared_ptr<const lux::simulation::SimulationAsset>, AssetDecodeFailure>
    TAssetSerDeser<lux::simulation::SimulationAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> cooked_image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(cooked_image), limits);
        if (!image)
            return lux::cxx::unexpected(image.error());
        if (image->magic() != lux::simulation::SimulationAsset::primary_magic)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_MAGIC, 0U});
        if (image->metadata().legacy_type_tag != lux::simulation::SimulationAsset::legacy_type_tag)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_TYPE, 0U});
        if (!image->information().empty())
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_LAYOUT, 0U});
        auto description = lux::simulation::detail::decodeDescription(
            image->data().view(),
            image->data().size(),
            limits.max_decoded_bytes
        );
        if (!description)
        {
            const auto code = description.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetDecodeError::ALLOCATION_FAILURE
                : EAssetDecodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetDecodeFailure{code, 0U});
        }
        try
        {
            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(),
                image->auxiliaryPayloads().end()
            );
            return lux::simulation::SimulationAsset::create(
                AssetInfo{
                    image->metadata().id,
                    lux::simulation::SimulationAsset::asset_type,
                    image->metadata().date,
                    image->metadata().display_name,
                    image->metadata().source_path,
                    image->metadata().source_mtime
                },
                std::move(*description),
                std::move(auxiliary)
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::ALLOCATION_FAILURE, 0U});
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<lux::simulation::SimulationAsset>::encode(
        const lux::simulation::SimulationAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        auto payload = lux::simulation::detail::encodeDescription(asset.data(), limits.max_encoded_bytes);
        if (!payload)
        {
            const auto code = payload.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetEncodeError::ALLOCATION_FAILURE
                : EAssetEncodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetEncodeFailure{code, 0U});
        }
        return detail::encodeCookedAssetImage(
            detail::CookedAssetWriteRequest{
                lux::simulation::SimulationAsset::primary_magic,
                lux::simulation::SimulationAsset::legacy_type_tag,
                asset.info(),
                {},
                *payload,
                asset.auxiliaryPayloads()
            },
            limits
        );
    }
} // namespace lux::asset
