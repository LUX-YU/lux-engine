#include <lux/engine/simulation/systems/ScriptSystemDescriptionCodec.hpp>

#include <array>
#include <limits>
#include <new>
#include <type_traits>

namespace lux::simulation::script
{
    namespace
    {
        constexpr std::uint32_t kWireVersion{1U};
        constexpr std::uint32_t kSectionCount{2U};
        constexpr std::uint32_t kDirectoryEntryBytes{24U};
        constexpr std::uint64_t kDirectoryOffset{32U};
        constexpr std::uint64_t kDataOffset{
            kDirectoryOffset + kSectionCount * kDirectoryEntryBytes};
        constexpr std::array<std::uint32_t, kSectionCount> kRecordBytes{
            56U,
            32U};

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
    }

    SimulationDataSchemaId scriptSystemDataSchemaId()
    {
        return simulationDataSchemaId(ScriptSystemDataCanonicalName);
    }

    lux::cxx::expected<std::vector<std::byte>, EScriptSystemDescriptionError>
    encodeScriptSystemDescription(
        const ScriptSystemDescription& description,
        ScriptSystemCodecLimits limits
    ) noexcept
    {
        try
        {
            Bytes mounts;
            Bytes bindings;
            for (const auto& mount : description.mounts())
            {
                if (!mount.id.valid() || mount.asset.isNull() ||
                    mount.bindings.size() >
                        std::numeric_limits<std::uint32_t>::max())
                {
                    return lux::cxx::unexpected(
                        EScriptSystemDescriptionError::INVALID_MOUNT);
                }
                mounts.u64(mount.id.value);
                mounts.raw(mount.asset.bytes());
                const bool entity =
                    std::holds_alternative<EntityScriptMount>(mount.scope);
                mounts.u32(entity ? 1U : 0U);
                mounts.u32(mount.enabled ? 1U : 0U);
                if (entity)
                {
                    mounts.raw(std::get<EntityScriptMount>(mount.scope)
                        .object.value.as_bytes());
                }
                else
                {
                    for (std::size_t index{}; index < 16U; ++index)
                        mounts.u8(0U);
                }
                mounts.u32(static_cast<std::uint32_t>(
                    bindings.values.size() / kRecordBytes[1]));
                mounts.u32(static_cast<std::uint32_t>(mount.bindings.size()));
                for (const auto& binding : mount.bindings)
                {
                    bindings.u64(binding.symbol);
                    std::visit(
                        [&](const auto& target)
                        {
                            using Target =
                                std::remove_cvref_t<decltype(target)>;
                            if constexpr (std::is_same_v<
                                Target,
                                HookScriptTarget>)
                            {
                                bindings.u32(0U);
                                bindings.u32(0U);
                                bindings.u64(target.system.value);
                                bindings.u64(target.hook.value);
                            }
                            else
                            {
                                bindings.u32(1U);
                                bindings.u32(0U);
                                bindings.u64(target.system.value);
                                bindings.u64(target.event.value);
                            }
                        },
                        binding.target
                    );
                }
            }

            Bytes output;
            output.u32(ScriptSystemWireMagic);
            output.u32(kWireVersion);
            output.u32(kSectionCount);
            output.u32(kDirectoryEntryBytes);
            output.u64(kDirectoryOffset);
            const std::uint64_t total_size =
                kDataOffset + mounts.values.size() + bindings.values.size();
            output.u64(total_size);
            output.u32(1U);
            output.u32(kRecordBytes[0]);
            output.u64(kDataOffset);
            output.u64(mounts.values.size());
            output.u32(2U);
            output.u32(kRecordBytes[1]);
            output.u64(kDataOffset + mounts.values.size());
            output.u64(bindings.values.size());
            output.raw(mounts.values);
            output.raw(bindings.values);
            if (output.values.size() != total_size ||
                output.values.size() > limits.max_encoded_bytes)
            {
                return lux::cxx::unexpected(
                    EScriptSystemDescriptionError::OUTPUT_BUDGET_EXCEEDED);
            }
            return std::move(output.values);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<ScriptSystemDescription, EScriptSystemDescriptionError>
    decodeScriptSystemDescription(
        std::span<const std::byte> bytes,
        const SimulationDescription& simulation,
        ScriptSystemCodecLimits limits
    ) noexcept
    {
        if (bytes.size() > limits.max_input_bytes || bytes.size() < kDataOffset)
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::INPUT_BUDGET_EXCEEDED);
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
                magic != ScriptSystemWireMagic || version != kWireVersion ||
                section_count != kSectionCount ||
                entry_bytes != kDirectoryEntryBytes ||
                directory_offset != kDirectoryOffset || total_size != bytes.size())
            {
                return lux::cxx::unexpected(
                    EScriptSystemDescriptionError::CORRUPT_WIRE);
            }
            std::array<std::span<const std::byte>, kSectionCount> sections;
            std::uint64_t expected_offset{kDataOffset};
            for (std::size_t index{}; index < sections.size(); ++index)
            {
                std::uint32_t kind{}, record_bytes{};
                std::uint64_t offset{}, size{};
                if (!readU32(bytes, cursor, kind) ||
                    !readU32(bytes, cursor, record_bytes) ||
                    !readU64(bytes, cursor, offset) ||
                    !readU64(bytes, cursor, size) || kind != index + 1U ||
                    record_bytes != kRecordBytes[index] ||
                    offset != expected_offset || size > bytes.size() - offset ||
                    size % record_bytes != 0U)
                {
                    return lux::cxx::unexpected(
                        EScriptSystemDescriptionError::CORRUPT_WIRE);
                }
                sections[index] = bytes.subspan(
                    static_cast<std::size_t>(offset),
                    static_cast<std::size_t>(size)
                );
                expected_offset += size;
            }
            if (expected_offset != total_size ||
                sizeof(ScriptSystemDescription) + sections[0].size() +
                    sections[1].size() > limits.max_decoded_bytes)
            {
                return lux::cxx::unexpected(
                    EScriptSystemDescriptionError::DECODED_BUDGET_EXCEEDED);
            }

            const auto binding_count =
                sections[1].size() / kRecordBytes[1];
            ScriptSystemDescriptionBuilder builder;
            cursor = 0U;
            std::size_t expected_binding_first{};
            for (std::size_t index{};
                 index < sections[0].size() / kRecordBytes[0]; ++index)
            {
                ScriptMountDescription mount;
                if (!readU64(sections[0], cursor, mount.id.value))
                    return lux::cxx::unexpected(
                        EScriptSystemDescriptionError::CORRUPT_WIRE);
                std::array<std::uint8_t, 16U> asset{};
                for (auto& byte : asset)
                {
                    if (!readU8(sections[0], cursor, byte))
                        return lux::cxx::unexpected(
                            EScriptSystemDescriptionError::CORRUPT_WIRE);
                }
                mount.asset = lux::asset::AssetId{asset};
                std::uint32_t scope{}, enabled{};
                if (!readU32(sections[0], cursor, scope) ||
                    !readU32(sections[0], cursor, enabled) || scope > 1U ||
                    enabled > 1U)
                {
                    return lux::cxx::unexpected(
                        EScriptSystemDescriptionError::CORRUPT_WIRE);
                }
                std::array<std::uint8_t, 16U> object{};
                for (auto& byte : object)
                {
                    if (!readU8(sections[0], cursor, byte))
                        return lux::cxx::unexpected(
                            EScriptSystemDescriptionError::CORRUPT_WIRE);
                }
                if (scope == 0U)
                {
                    if (std::any_of(
                            object.begin(),
                            object.end(),
                            [](auto byte) noexcept { return byte != 0U; }))
                    {
                        return lux::cxx::unexpected(
                            EScriptSystemDescriptionError::CORRUPT_WIRE);
                    }
                    mount.scope = SimulationScriptMount{};
                }
                else
                {
                    mount.scope = EntityScriptMount{
                        lux::domain::WorldObjectId{uuids::uuid{object}}};
                }
                mount.enabled = enabled != 0U;
                std::uint32_t binding_first{}, mount_binding_count{};
                if (!readU32(
                        sections[0],
                        cursor,
                        binding_first
                    ) ||
                    !readU32(
                        sections[0],
                        cursor,
                        mount_binding_count
                    ) ||
                    !rangeValid(
                        binding_first,
                        mount_binding_count,
                        binding_count
                    ) || binding_first != expected_binding_first)
                {
                    return lux::cxx::unexpected(
                        EScriptSystemDescriptionError::CORRUPT_WIRE);
                }
                mount.bindings.reserve(mount_binding_count);
                for (std::uint32_t item{};
                     item < mount_binding_count; ++item)
                {
                    std::size_t offset = static_cast<std::size_t>(
                        binding_first + item) * kRecordBytes[1];
                    ScriptBindingDescription binding;
                    std::uint32_t kind{}, reserved{};
                    std::uint64_t system{}, endpoint{};
                    if (!readU64(sections[1], offset, binding.symbol) ||
                        !readU32(sections[1], offset, kind) ||
                        !readU32(sections[1], offset, reserved) ||
                        !readU64(sections[1], offset, system) ||
                        !readU64(sections[1], offset, endpoint) ||
                        kind > 1U || reserved != 0U)
                    {
                        return lux::cxx::unexpected(
                            EScriptSystemDescriptionError::CORRUPT_WIRE);
                    }
                    if (kind == 0U)
                    {
                        binding.target = HookScriptTarget{
                            SystemInstanceId{system},
                            HookPointId{endpoint}};
                    }
                    else
                    {
                        binding.target = EventScriptTarget{
                            SystemInstanceId{system},
                            EventPointId{endpoint}};
                    }
                    mount.bindings.push_back(std::move(binding));
                }
                expected_binding_first += mount_binding_count;
                const auto added = builder.addMount(std::move(mount));
                if (!added)
                    return lux::cxx::unexpected(added.error());
            }
            if (expected_binding_first != binding_count)
            {
                return lux::cxx::unexpected(
                    EScriptSystemDescriptionError::CORRUPT_WIRE);
            }
            return std::move(builder).build(simulation);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::ALLOCATION_FAILURE);
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::CORRUPT_WIRE);
        }
    }

    lux::cxx::expected<void, EScriptSystemDescriptionError>
    addScriptSystemData(
        SimulationDescriptionBuilder& builder,
        const ScriptSystemDescription& description,
        ScriptSystemCodecLimits limits
    ) noexcept
    {
        auto encoded = encodeScriptSystemDescription(description, limits);
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        if (!builder.addData(
                scriptSystemDataSchemaId(),
                ScriptSystemDescription::kSchemaVersion,
                *encoded))
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::INVALID_MOUNT);
        }
        return {};
    }
}
