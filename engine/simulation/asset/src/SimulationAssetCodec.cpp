#include <lux/engine/simulation/SimulationAssetCodec.hpp>

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::simulation
{
    using lux::asset::DecodedAsset;
    using lux::asset::EAssetCodecError;

    namespace
    {
        constexpr std::uint32_t kWireVersion = 4U;
        constexpr std::uint32_t kSectionCount = 12U;
        constexpr std::uint32_t kDirectoryEntryBytes = 24U;
        constexpr std::uint64_t kHeaderBytes = 32U;
        constexpr std::uint64_t kDirectoryOffset = kHeaderBytes;
        constexpr std::uint64_t kSectionDataOffset =
            kDirectoryOffset + kSectionCount * kDirectoryEntryBytes;
        constexpr std::uint32_t kNoString =
            std::numeric_limits<std::uint32_t>::max();

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
            GLOBAL_SCRIPT_MOUNTS,
            SCRIPT_BINDINGS,
            PAYLOAD,
        };

        enum class EBindingTargetWire : std::uint32_t
        {
            HOOK,
            EVENT,
            LIFECYCLE,
        };

        constexpr std::array<std::uint32_t, kSectionCount> kRecordBytes{
            0U,
            32U,
            56U,
            24U,
            4U,
            24U,
            16U,
            32U,
            8U,
            32U,
            40U,
            0U,
        };

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

        [[nodiscard]] bool validCount(std::size_t value) noexcept
        {
            return value <= std::numeric_limits<std::uint32_t>::max();
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

            [[nodiscard]] std::uint32_t ordinal(std::string_view value) const
            {
                if (value.empty())
                    return kNoString;
                const auto found = std::lower_bound(
                    strings_.begin(), strings_.end(), value);
                if (found == strings_.end() || *found != value)
                    throw std::logic_error("LXSD string was not collected");
                return static_cast<std::uint32_t>(
                    std::distance(strings_.begin(), found));
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
                const auto found = std::find_if(
                    types.begin(), types.end(),
                    [&](const auto& value) noexcept
                    {
                        return value.type == system.type();
                    });
                if (found == types.end())
                    types.push_back({system.type(), system});
            }
            std::sort(
                types.begin(), types.end(),
                [](const auto& left, const auto& right) noexcept
                {
                    return SystemTypeIdLess{}(left.type, right.type);
                });

            for (const auto& source : types)
            {
                const auto system = source.system;
                strings.add(source.type.name);
                strings.add(system.configurationSchemaName());
                for (std::size_t index{}; index < system.capabilityCount(); ++index)
                    strings.add(system.capabilityAt(index));
                for (std::size_t hook{}; hook < system.hookPointCount(); ++hook)
                {
                    const auto point = system.hookPointAt(hook);
                    strings.add(point.name());
                    for (std::size_t type{}; type < point.parameterCount(); ++type)
                        strings.add(point.parameterAt(type).canonical_name);
                    for (std::size_t type{}; type < point.returnCount(); ++type)
                        strings.add(point.returnAt(type).canonical_name);
                }
                for (std::size_t event{}; event < system.eventCount(); ++event)
                {
                    strings.add(system.eventAt(event).name());
                    strings.add(system.eventAt(event).payloadSchemaName());
                }
            }
            for (std::size_t mount{};
                 mount < description.globalScriptMountCount(); ++mount)
            {
                const auto value = description.globalScriptMountAt(mount);
                for (std::size_t binding{};
                     binding < value.bindingCount(); ++binding)
                {
                    const auto* item = value.bindingAt(binding);
                    std::visit(
                        [&](const auto& target)
                        {
                            using Target = std::remove_cvref_t<decltype(target)>;
                            if constexpr (
                                std::is_same_v<Target, SystemHookBindingTarget>)
                            {
                                strings.add(target.system_type.name);
                                strings.add(target.system_instance);
                                strings.add(target.hook);
                            }
                            else if constexpr (
                                std::is_same_v<Target, SystemEventBindingTarget>)
                            {
                                strings.add(target.system_type.name);
                                strings.add(target.system_instance);
                                strings.add(target.event);
                            }
                        },
                        item->target
                    );
                }
            }
            strings.finish();
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<std::byte>, EAssetCodecError>
        encodeDescription(
            const void* payload,
            const lux::asset::AssetEncodeContext& context
        ) noexcept
        {
            if (payload == nullptr)
                return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
            try
            {
                const auto& description =
                    *static_cast<const SimulationDescription*>(payload);
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
                auto& signature_types = sections[6];
                auto& events = sections[7];
                auto& dependencies = sections[8];
                auto& mounts = sections[9];
                auto& bindings = sections[10];
                auto& binary_payload = sections[11];

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

                for (const auto& type_source : types)
                {
                    const auto system = type_source.system;
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
                            signature_types.values.size() / kRecordBytes[6]);
                        for (std::size_t item{}; item < hook.parameterCount(); ++item)
                        {
                            const auto type = hook.parameterAt(item);
                            signature_types.u64(type.type_id);
                            signature_types.u32(strings.ordinal(type.canonical_name));
                            signature_types.u32(static_cast<std::uint32_t>(type.pass));
                        }
                        const auto return_first = static_cast<std::uint32_t>(
                            signature_types.values.size() / kRecordBytes[6]);
                        for (std::size_t item{}; item < hook.returnCount(); ++item)
                        {
                            const auto type = hook.returnAt(item);
                            signature_types.u64(type.type_id);
                            signature_types.u32(strings.ordinal(type.canonical_name));
                            signature_types.u32(static_cast<std::uint32_t>(type.pass));
                        }
                        hooks.u32(strings.ordinal(hook.name()));
                        hooks.u32(static_cast<std::uint32_t>(hook.cardinality()));
                        hooks.u32(parameter_first);
                        hooks.u32(static_cast<std::uint32_t>(hook.parameterCount()));
                        hooks.u32(return_first);
                        hooks.u32(static_cast<std::uint32_t>(hook.returnCount()));
                    }

                    const auto event_first = static_cast<std::uint32_t>(
                        events.values.size() / kRecordBytes[7]);
                    for (std::size_t index{}; index < system.eventCount(); ++index)
                    {
                        const auto event = system.eventAt(index);
                        const auto dispatch = system.findHookPoint(
                            event.dispatchHook().name());
                        std::uint32_t local_hook{};
                        for (; local_hook < system.hookPointCount(); ++local_hook)
                        {
                            if (system.hookPointAt(local_hook).name() == dispatch.name())
                                break;
                        }
                        events.u32(strings.ordinal(event.name()));
                        events.u32(hook_first + local_hook);
                        events.u32(static_cast<std::uint32_t>(event.target()));
                        events.u32(strings.ordinal(event.payloadSchemaName()));
                        events.u32(event.payloadSchemaVersion());
                        events.u32(0U);
                        events.u64(event.payloadSchemaHash());
                    }

                    system_types.u64(type_source.type.hash);
                    system_types.u32(strings.ordinal(type_source.type.name));
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
                        types.begin(), types.end(), system.type(),
                        [](const auto& value, const SystemTypeId& id) noexcept
                        {
                            return SystemTypeIdLess{}(value.type, id);
                        });
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
                    const auto before = description.findSystem(
                        dependency.before().instanceName());
                    const auto after = description.findSystem(
                        dependency.after().instanceName());
                    std::uint32_t before_ordinal{}, after_ordinal{};
                    for (std::size_t system{};
                         system < description.systemCount(); ++system)
                    {
                        if (description.systemAt(system).instanceName() ==
                            before.instanceName())
                            before_ordinal = static_cast<std::uint32_t>(system);
                        if (description.systemAt(system).instanceName() ==
                            after.instanceName())
                            after_ordinal = static_cast<std::uint32_t>(system);
                    }
                    dependencies.u32(before_ordinal);
                    dependencies.u32(after_ordinal);
                }

                for (std::size_t mount{};
                     mount < description.globalScriptMountCount(); ++mount)
                {
                    const auto value = description.globalScriptMountAt(mount);
                    mounts.u64(value.id().value);
                    for (const auto byte : value.script().bytes())
                        mounts.u8(static_cast<std::uint8_t>(byte));
                    mounts.u32(static_cast<std::uint32_t>(
                        bindings.values.size() / kRecordBytes[10]));
                    mounts.u32(static_cast<std::uint32_t>(value.bindingCount()));
                    for (std::size_t binding{};
                         binding < value.bindingCount(); ++binding)
                    {
                        const auto& item = *value.bindingAt(binding);
                        bindings.u64(item.function);
                        std::visit(
                            [&](const auto& target)
                            {
                                using Target = std::remove_cvref_t<decltype(target)>;
                                if constexpr (
                                    std::is_same_v<Target, SystemHookBindingTarget>)
                                {
                                    bindings.u32(static_cast<std::uint32_t>(
                                        EBindingTargetWire::HOOK));
                                    bindings.u64(target.system_type.hash);
                                    bindings.u32(strings.ordinal(
                                        target.system_type.name));
                                    bindings.u32(strings.ordinal(
                                        target.system_instance));
                                    bindings.u32(strings.ordinal(target.hook));
                                    bindings.u32(0U);
                                    bindings.u32(0U);
                                }
                                else if constexpr (
                                    std::is_same_v<Target, SystemEventBindingTarget>)
                                {
                                    bindings.u32(static_cast<std::uint32_t>(
                                        EBindingTargetWire::EVENT));
                                    bindings.u64(target.system_type.hash);
                                    bindings.u32(strings.ordinal(
                                        target.system_type.name));
                                    bindings.u32(strings.ordinal(
                                        target.system_instance));
                                    bindings.u32(strings.ordinal(target.event));
                                    bindings.u32(0U);
                                    bindings.u32(0U);
                                }
                                else
                                {
                                    bindings.u32(static_cast<std::uint32_t>(
                                        EBindingTargetWire::LIFECYCLE));
                                    bindings.u64(0U);
                                    bindings.u32(kNoString);
                                    bindings.u32(kNoString);
                                    bindings.u32(kNoString);
                                    bindings.u32(static_cast<std::uint32_t>(
                                        target.point));
                                    bindings.u32(0U);
                                }
                            },
                            item.target
                        );
                    }
                }

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
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
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
                    output.values.size() > context.limits.max_encoded_bytes)
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

        [[nodiscard]] bool rangeValid(
            std::uint32_t first,
            std::uint32_t count,
            std::size_t total
        ) noexcept
        {
            return first <= total && count <= total - first;
        }

        [[nodiscard]] lux::cxx::expected<DecodedAsset, EAssetCodecError>
        decodeDescription(
            std::span<const std::byte> bytes,
            const lux::asset::AssetDecodeContext& context
        ) noexcept
        {
            if (bytes.size() > context.limits.max_input_bytes ||
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
                    directory_offset != kDirectoryOffset || total_size != bytes.size())
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
                        kind != index + 1U || record_bytes != kRecordBytes[index] ||
                        offset != expected_offset || size > bytes.size() - offset ||
                        (record_bytes != 0U && size % record_bytes != 0U))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
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

                std::size_t estimated = sizeof(SimulationDescription);
                for (const auto& section : sections)
                {
                    if (section.bytes.size() >
                        context.limits.max_decoded_bytes -
                        std::min(estimated, context.limits.max_decoded_bytes))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    estimated += section.bytes.size();
                }
                if (estimated > context.limits.max_decoded_bytes)
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);

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
                    if (!readU32(sections[0].bytes, cursor, size) ||
                        size == 0U || size > sections[0].bytes.size() - cursor)
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    const std::string_view value{
                        reinterpret_cast<const char*>(
                            sections[0].bytes.data() + cursor), size};
                    if (!strings.empty() && strings.back() >= value)
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    strings.push_back(value);
                    cursor += size;
                }
                if (cursor != sections[0].bytes.size())
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                const auto stringAt = [&](std::uint32_t ordinal) -> std::string_view
                {
                    if (ordinal == kNoString)
                        return {};
                    if (ordinal >= strings.size())
                        throw std::out_of_range("LXSD string ordinal");
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
                const auto instance_count = countOf(3);
                const auto binding_count = countOf(10);

                std::vector<TypeWire> types;
                types.reserve(countOf(2));
                cursor = 0U;
                for (std::size_t index{}; index < countOf(2); ++index)
                {
                    TypeWire type;
                    if (!readU64(sections[2].bytes, cursor, type.hash) ||
                        !readU32(sections[2].bytes, cursor, type.name) ||
                        !readU32(sections[2].bytes, cursor, type.version) ||
                        !readU32(sections[2].bytes, cursor, type.configuration_name) ||
                        !readU32(sections[2].bytes, cursor, type.configuration_version) ||
                        !readU64(sections[2].bytes, cursor, type.configuration_hash) ||
                        !readU32(sections[2].bytes, cursor, type.capability_first) ||
                        !readU32(sections[2].bytes, cursor, type.capability_count) ||
                        !readU32(sections[2].bytes, cursor, type.hook_first) ||
                        !readU32(sections[2].bytes, cursor, type.hook_count) ||
                        !readU32(sections[2].bytes, cursor, type.event_first) ||
                        !readU32(sections[2].bytes, cursor, type.event_count) ||
                        !rangeValid(type.capability_first, type.capability_count,
                            capability_count) ||
                        !rangeValid(type.hook_first, type.hook_count, hook_count) ||
                        !rangeValid(type.event_first, type.event_count, event_count) ||
                        type.version == 0U)
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    const auto name = stringAt(type.name);
                    const auto configuration = stringAt(type.configuration_name);
                    if (name.empty() || systemTypeId(name).hash != type.hash ||
                        configuration.empty() !=
                            (type.configuration_version == 0U) ||
                        (configuration.empty() ? 0U :
                            lux::cxx::Fnv1a64::hash(configuration)) !=
                            type.configuration_hash ||
                        (!types.empty() && SystemTypeIdLess{}(
                            SystemTypeId{type.hash, std::string(name)},
                            SystemTypeId{types.back().hash,
                                std::string(stringAt(types.back().name))})))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    types.push_back(type);
                }

                struct InstanceWire final
                {
                    std::uint32_t name{};
                    std::uint32_t type{};
                    std::uint64_t payload_offset{};
                    std::uint64_t payload_size{};
                };
                std::vector<InstanceWire> instances;
                instances.reserve(instance_count);
                cursor = 0U;
                for (std::size_t index{}; index < instance_count; ++index)
                {
                    InstanceWire instance;
                    if (!readU32(sections[3].bytes, cursor, instance.name) ||
                        !readU32(sections[3].bytes, cursor, instance.type) ||
                        !readU64(sections[3].bytes, cursor, instance.payload_offset) ||
                        !readU64(sections[3].bytes, cursor, instance.payload_size) ||
                        instance.type >= types.size() || stringAt(instance.name).empty() ||
                        (!instances.empty() &&
                            stringAt(instances.back().name) >= stringAt(instance.name)))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
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
                        payload_size > sections[11].bytes.size() - payload_offset)
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    const auto name = stringAt(schema_name);
                    if (simulationDataSchemaId(name).hash != schema_hash ||
                        !builder.addData(
                            SimulationDataSchemaId{schema_hash, std::string(name)},
                            schema_version,
                            sections[11].bytes.subspan(
                                static_cast<std::size_t>(payload_offset),
                                static_cast<std::size_t>(payload_size))))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    expected_payload_offset += payload_size;
                }

                std::vector<std::vector<std::string_view>> capability_views(types.size());
                std::vector<std::vector<SystemHookPoint>> hook_views(types.size());
                std::vector<std::vector<std::vector<lux::script::ScriptSemanticType>>>
                    parameter_views(types.size()), return_views(types.size());
                std::vector<std::vector<SystemEventDescription>> event_views(types.size());

                for (std::size_t type_index{}; type_index < types.size(); ++type_index)
                {
                    const auto& type = types[type_index];
                    auto& type_capabilities = capability_views[type_index];
                    type_capabilities.reserve(type.capability_count);
                    for (std::uint32_t item{}; item < type.capability_count; ++item)
                    {
                        std::size_t offset = static_cast<std::size_t>(
                            type.capability_first + item) * kRecordBytes[4];
                        std::uint32_t name{};
                        if (!readU32(sections[4].bytes, offset, name))
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        type_capabilities.push_back(stringAt(name));
                    }

                    auto& type_hooks = hook_views[type_index];
                    auto& type_parameters = parameter_views[type_index];
                    auto& type_returns = return_views[type_index];
                    type_hooks.reserve(type.hook_count);
                    type_parameters.resize(type.hook_count);
                    type_returns.resize(type.hook_count);
                    for (std::uint32_t item{}; item < type.hook_count; ++item)
                    {
                        std::size_t offset = static_cast<std::size_t>(
                            type.hook_first + item) * kRecordBytes[5];
                        std::uint32_t name{}, cardinality{}, parameter_first{},
                            parameter_count{}, return_first{}, return_count{};
                        if (!readU32(sections[5].bytes, offset, name) ||
                            !readU32(sections[5].bytes, offset, cardinality) ||
                            !readU32(sections[5].bytes, offset, parameter_first) ||
                            !readU32(sections[5].bytes, offset, parameter_count) ||
                            !readU32(sections[5].bytes, offset, return_first) ||
                            !readU32(sections[5].bytes, offset, return_count) ||
                            cardinality > static_cast<std::uint32_t>(
                                ESystemHookCardinality::MULTI) ||
                            !rangeValid(parameter_first, parameter_count,
                                signature_count) ||
                            !rangeValid(return_first, return_count, signature_count))
                        {
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        }
                        const auto readSemanticRange = [&](
                            std::uint32_t first,
                            std::uint32_t count,
                            std::vector<lux::script::ScriptSemanticType>& output)
                        {
                            output.reserve(count);
                            for (std::uint32_t semantic{}; semantic < count; ++semantic)
                            {
                                std::size_t semantic_offset =
                                    static_cast<std::size_t>(first + semantic) *
                                    kRecordBytes[6];
                                std::uint64_t id{};
                                std::uint32_t type_name{}, pass{};
                                if (!readU64(sections[6].bytes, semantic_offset, id) ||
                                    !readU32(sections[6].bytes, semantic_offset,
                                        type_name) ||
                                    !readU32(sections[6].bytes, semantic_offset, pass) ||
                                    pass > static_cast<std::uint32_t>(
                                        lux::script::EScriptPassMode::CONST_REF))
                                {
                                    return false;
                                }
                                const auto canonical_name = stringAt(type_name);
                                if (id != lux::script::scriptSemanticTypeId(
                                        canonical_name))
                                    return false;
                                output.push_back({
                                    id, canonical_name,
                                    static_cast<lux::script::EScriptPassMode>(pass)});
                            }
                            return true;
                        };
                        if (!readSemanticRange(parameter_first, parameter_count,
                                type_parameters[item]) ||
                            !readSemanticRange(return_first, return_count,
                                type_returns[item]))
                        {
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        }
                        type_hooks.push_back({
                            stringAt(name),
                            static_cast<ESystemHookCardinality>(cardinality),
                            {type_parameters[item], type_returns[item]}});
                    }

                    auto& type_events = event_views[type_index];
                    type_events.reserve(type.event_count);
                    for (std::uint32_t item{}; item < type.event_count; ++item)
                    {
                        std::size_t offset = static_cast<std::size_t>(
                            type.event_first + item) * kRecordBytes[7];
                        std::uint32_t name{}, dispatch{}, target{}, payload_name{},
                            payload_version{}, reserved{};
                        std::uint64_t payload_hash{};
                        if (!readU32(sections[7].bytes, offset, name) ||
                            !readU32(sections[7].bytes, offset, dispatch) ||
                            !readU32(sections[7].bytes, offset, target) ||
                            !readU32(sections[7].bytes, offset, payload_name) ||
                            !readU32(sections[7].bytes, offset, payload_version) ||
                            !readU32(sections[7].bytes, offset, reserved) ||
                            !readU64(sections[7].bytes, offset, payload_hash) ||
                            dispatch < type.hook_first ||
                            dispatch >= type.hook_first + type.hook_count ||
                            target > static_cast<std::uint32_t>(
                                ESystemEventTarget::ENTITY_TARGETED) || reserved != 0U)
                        {
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        }
                        const auto payload_schema = stringAt(payload_name);
                        if ((payload_schema.empty() ? 0U :
                                lux::cxx::Fnv1a64::hash(payload_schema)) !=
                                payload_hash ||
                            payload_schema.empty() != (payload_version == 0U))
                        {
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        }
                        type_events.push_back({
                            stringAt(name),
                            type_hooks[dispatch - type.hook_first].name,
                            static_cast<ESystemEventTarget>(target),
                            payload_schema,
                            payload_version,
                            payload_schema.empty()
                                ? lux::cxx::typeToken<void>()
                                : lux::cxx::TypeToken{}});
                    }
                }

                for (const auto& instance : instances)
                {
                    const auto& type = types[instance.type];
                    if (instance.payload_offset != expected_payload_offset ||
                        instance.payload_size >
                            sections[11].bytes.size() - instance.payload_offset)
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    const SystemDescription system{
                        stringAt(type.name),
                        type.version,
                        stringAt(type.configuration_name),
                        type.configuration_version,
                        capability_views[instance.type],
                        hook_views[instance.type],
                        event_views[instance.type]};
                    if (!builder.addSystem(
                            stringAt(instance.name), system,
                            sections[11].bytes.subspan(
                                static_cast<std::size_t>(instance.payload_offset),
                                static_cast<std::size_t>(instance.payload_size))))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    expected_payload_offset += instance.payload_size;
                }
                if (expected_payload_offset != sections[11].bytes.size())
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
                            stringAt(instances[before].name),
                            stringAt(instances[after].name)))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    previous_dependency = {before, after};
                }

                cursor = 0U;
                for (std::size_t index{}; index < countOf(9); ++index)
                {
                    std::uint64_t mount_id{};
                    if (!readU64(sections[9].bytes, cursor, mount_id) ||
                        mount_id == 0U)
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    std::array<std::uint8_t, 16U> id{};
                    for (auto& byte : id)
                    {
                        if (!readU8(sections[9].bytes, cursor, byte))
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    std::uint32_t binding_first{}, mount_binding_count{};
                    if (!readU32(sections[9].bytes, cursor, binding_first) ||
                        !readU32(sections[9].bytes, cursor, mount_binding_count) ||
                        !rangeValid(binding_first, mount_binding_count,
                            binding_count))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    ScriptMountDescription mount;
                    mount.id = ScriptMountId{mount_id};
                    mount.script = lux::asset::AssetId{id};
                    mount.bindings.reserve(mount_binding_count);
                    for (std::uint32_t item{}; item < mount_binding_count; ++item)
                    {
                        std::size_t offset = static_cast<std::size_t>(
                            binding_first + item) * kRecordBytes[10];
                        ScriptBindingDescription binding;
                        std::uint32_t kind{}, system_type{}, system_instance{}, member{},
                            lifecycle{}, reserved{};
                        std::uint64_t system_type_hash{};
                        if (!readU64(sections[10].bytes, offset, binding.function) ||
                            !readU32(sections[10].bytes, offset, kind) ||
                            !readU64(sections[10].bytes, offset, system_type_hash) ||
                            !readU32(sections[10].bytes, offset, system_type) ||
                            !readU32(sections[10].bytes, offset, system_instance) ||
                            !readU32(sections[10].bytes, offset, member) ||
                            !readU32(sections[10].bytes, offset, lifecycle) ||
                            !readU32(sections[10].bytes, offset, reserved) ||
                            kind > static_cast<std::uint32_t>(
                                EBindingTargetWire::LIFECYCLE) ||
                            reserved != 0U)
                        {
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        }
                        const auto target_kind = static_cast<EBindingTargetWire>(kind);
                        if (target_kind == EBindingTargetWire::HOOK ||
                            target_kind == EBindingTargetWire::EVENT)
                        {
                            const auto type_name = stringAt(system_type);
                            const auto instance_name = stringAt(system_instance);
                            const auto member_name = stringAt(member);
                            if (system_type_hash == 0U || type_name.empty() ||
                                member_name.empty() || lifecycle != 0U ||
                                systemTypeId(type_name).hash != system_type_hash)
                            {
                                return lux::cxx::unexpected(
                                    EAssetCodecError::CODEC_FAILURE);
                            }
                            if (target_kind == EBindingTargetWire::HOOK)
                            {
                                binding.target = SystemHookBindingTarget{
                                    SystemTypeId{system_type_hash,
                                        std::string(type_name)},
                                    std::string(instance_name),
                                    std::string(member_name)};
                            }
                            else
                            {
                                binding.target = SystemEventBindingTarget{
                                    SystemTypeId{system_type_hash,
                                        std::string(type_name)},
                                    std::string(instance_name),
                                    std::string(member_name)};
                            }
                        }
                        else
                        {
                            if (system_type_hash != 0U || system_type != kNoString ||
                                system_instance != kNoString || member != kNoString ||
                                lifecycle > static_cast<std::uint32_t>(
                                    EBehaviorLifecyclePoint::STOP))
                            {
                                return lux::cxx::unexpected(
                                    EAssetCodecError::CODEC_FAILURE);
                            }
                            binding.target = BehaviorLifecycleBindingTarget{
                                static_cast<EBehaviorLifecyclePoint>(lifecycle)};
                        }
                        mount.bindings.push_back(std::move(binding));
                    }
                    if (!builder.addGlobalScriptMount(mount))
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }

                auto built = std::move(builder).build();
                if (!built || built->retainedBytes() >
                    context.limits.max_decoded_bytes)
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                auto description = std::make_shared<SimulationDescription>(
                    std::move(*built));
                const auto retained = description->retainedBytes();
                return DecodedAsset{std::move(description), retained};
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
            &decodeDescription,
            &encodeDescription,
            std::move(code_lifetime)};
    }
}
