#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace lux::script
{
    using lux::asset::EAssetCodecError;

    namespace detail
    {
        constexpr std::uint32_t kWireVersion = 8U;

        class Writer final
        {
          public:
            void u8(std::uint8_t value)
            {
                bytes_.push_back(static_cast<std::byte>(value));
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

            void string(std::string_view value)
            {
                if (value.size() > std::numeric_limits<std::uint32_t>::max())
                {
                    valid_ = false;
                    return;
                }
                u32(static_cast<std::uint32_t>(value.size()));
                const auto* first = reinterpret_cast<const std::byte*>(value.data());
                bytes_.insert(bytes_.end(), first, first + value.size());
            }

            void raw(std::span<const std::byte> value)
            {
                bytes_.insert(bytes_.end(), value.begin(), value.end());
            }

            [[nodiscard]] std::vector<std::byte> finish() &&
            {
                return std::move(bytes_);
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return valid_;
            }

          private:
            std::vector<std::byte> bytes_;
            bool valid_{true};
        };

        class Reader final
        {
          public:
            explicit Reader(std::span<const std::byte> bytes) noexcept
                : bytes_(bytes)
            {
            }

            [[nodiscard]] bool u8(std::uint8_t& value) noexcept
            {
                if (remaining() < 1U)
                    return false;
                value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
                return true;
            }

            [[nodiscard]] bool u32(std::uint32_t& value) noexcept
            {
                value = 0U;
                for (std::uint32_t shift{}; shift < 32U; shift += 8U)
                {
                    std::uint8_t byte{};
                    if (!u8(byte))
                        return false;
                    value |= static_cast<std::uint32_t>(byte) << shift;
                }
                return true;
            }

            [[nodiscard]] bool u64(std::uint64_t& value) noexcept
            {
                value = 0U;
                for (std::uint32_t shift{}; shift < 64U; shift += 8U)
                {
                    std::uint8_t byte{};
                    if (!u8(byte))
                        return false;
                    value |= static_cast<std::uint64_t>(byte) << shift;
                }
                return true;
            }

            [[nodiscard]] bool string(
                std::string& value,
                std::size_t& decoded,
                std::size_t limit
            )
            {
                std::uint32_t size{};
                if (!u32(size) || size > remaining() || size > limit - decoded)
                    return false;
                value.assign(
                    reinterpret_cast<const char*>(bytes_.data() + offset_),
                    size
                );
                offset_ += size;
                decoded += size;
                return true;
            }

            [[nodiscard]] bool take(
                std::size_t size,
                std::span<const std::byte>& value
            ) noexcept
            {
                if (size > remaining())
                    return false;
                value = bytes_.subspan(offset_, size);
                offset_ += size;
                return true;
            }

            [[nodiscard]] std::size_t remaining() const noexcept
            {
                return bytes_.size() - offset_;
            }

          private:
            std::span<const std::byte> bytes_;
            std::size_t offset_{};
        };

        void writeType(Writer& writer, const lux::rdesc::ScriptValueType& type)
        {
            writer.string(type.canonical_name);
            writer.u64(type.type_id);
            writer.u8(static_cast<std::uint8_t>(type.pass));
            writer.u8(type.abi_kind);
            writer.u32(type.size);
            writer.u32(type.alignment);
        }

        [[nodiscard]] bool readType(
            Reader& reader,
            lux::rdesc::ScriptValueType& type,
            std::size_t& decoded,
            std::size_t limit
        )
        {
            std::uint8_t pass{};
            return reader.string(type.canonical_name, decoded, limit) &&
                reader.u64(type.type_id) && reader.u8(pass) &&
                reader.u8(type.abi_kind) && reader.u32(type.size) &&
                reader.u32(type.alignment) &&
                pass <= static_cast<std::uint8_t>(
                    lux::semantic::EValuePass::CONST_REF
                ) &&
                ((type.pass = static_cast<lux::semantic::EValuePass>(pass)), true);
        }

        template <class Type>
        [[nodiscard]] bool reserveRecords(
            std::vector<Type>& records,
            std::uint32_t count,
            std::size_t& decoded,
            std::size_t limit,
            std::size_t remaining
        )
        {
            if (count > remaining ||
                count > (limit - decoded) / sizeof(Type))
            {
                return false;
            }
            decoded += static_cast<std::size_t>(count) * sizeof(Type);
            records.reserve(count);
            return true;
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<std::byte>,
            EAssetCodecError>
        encodeScriptArtifact(
            const ScriptArtifact& artifact,
            std::size_t max_encoded_bytes
        ) noexcept
        {
            const auto& description = artifact.description();
            try
            {
                Writer writer;
                writer.u32(ScriptArtifactPrimaryMagic);
                writer.u32(kWireVersion);
                writer.u32(lux::rdesc::Script::kSchemaVersion);
                writer.u32(static_cast<std::uint32_t>(description.kind()));
                writer.string(description.module_name);
                writer.u64(description.lifecycle.begin_play);
                writer.u64(description.lifecycle.end_play);

                if (description.exports.size() >
                    std::numeric_limits<std::uint32_t>::max())
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                writer.u32(static_cast<std::uint32_t>(
                    description.exports.size()));
                for (const auto& function : description.exports)
                {
                    writer.string(function.name);
                    writer.u64(function.symbol_id);
                    writer.u32(static_cast<std::uint32_t>(function.args.size()));
                    for (const auto& argument : function.args)
                        writeType(writer, argument);
                    writer.u32(static_cast<std::uint32_t>(function.returns.size()));
                    for (const auto& result : function.returns)
                        writeType(writer, result);
                }

                writer.u32(static_cast<std::uint32_t>(
                    description.dependencies.size()));
                for (const auto& dependency : description.dependencies)
                {
                    writer.string(dependency.kind);
                    writer.string(dependency.id);
                }
                writer.u32(static_cast<std::uint32_t>(description.api_requirements.size()));
                for (const auto& requirement : description.api_requirements)
                {
                    writer.string(requirement.contract.name());
                    writer.u64(requirement.contract.hash());
                    writer.u64(requirement.expected_schema_hash);
                }
                writer.u32(static_cast<std::uint32_t>(description.event_requirements.size()));
                for (const auto& requirement : description.event_requirements)
                {
                    writer.string(requirement.system_name);
                    writer.string(requirement.event_name);
                    writer.u64(requirement.system_id);
                    writer.u64(requirement.event_id);
                    writer.u32(static_cast<std::uint32_t>(requirement.route));
                    writer.string(requirement.payload.canonical_name);
                    writer.u64(requirement.payload.type_id);
                    writer.u32(requirement.payload.abi_kind);
                    writer.u32(requirement.payload.size);
                    writer.u32(requirement.payload.alignment);
                    writer.u64(requirement.payload_schema_hash);
                    writer.u32(requirement.payload_schema_version);
                }
                const auto& provenance = description.provenance;
                writer.string(provenance.compiler_id);
                writer.string(provenance.compiler_version);
                writer.string(provenance.source_id);
                writer.string(provenance.source_hash);
                writer.string(provenance.built_at);

                if (const auto* lua = std::get_if<lux::rdesc::LuaSourceScript>(
                        &description.body))
                {
                    writer.string(lua->entry);
                    writer.u32(static_cast<std::uint32_t>(lua->suspension_capable_exports.size()));
                    for (const auto symbol : lua->suspension_capable_exports)
                        writer.u64(symbol);
                }
                else if (const auto* native =
                    std::get_if<lux::rdesc::NativeModuleScript>(
                        &description.body))
                {
                    writer.u32(native->abi_version);
                    writer.u64(native->state_layout_hash);
                    writer.u32(native->state_size);
                    writer.u32(native->state_align);
                    writer.u32(static_cast<std::uint32_t>(
                        native->state_defaults.size()));
                    writer.raw(native->state_defaults);
                }
                else if (const auto* cpp_static =
                    std::get_if<lux::rdesc::CppStaticScript>(
                        &description.body))
                {
                    writer.string(cpp_static->descriptor);
                }

                writer.u64(artifact.payload().size());
                writer.raw(artifact.payload());
                if (!writer.valid())
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);

                auto result = std::move(writer).finish();
                if (result.size() > max_encoded_bytes)
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                return result;
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

        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const ScriptArtifact>, EAssetCodecError>
        decodeScriptArtifact(
            std::span<const std::byte> bytes,
            std::size_t max_input_bytes,
            std::size_t max_decoded_bytes
        ) noexcept
        {
            if (bytes.size() > max_input_bytes)
                return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
            try
            {
                Reader reader(bytes);
                std::uint32_t magic{}, wire{}, schema{}, kind{};
                if (!reader.u32(magic) || !reader.u32(wire) ||
                    !reader.u32(schema) || !reader.u32(kind) ||
                    magic != ScriptArtifactPrimaryMagic || wire != kWireVersion ||
                    schema != lux::rdesc::Script::kSchemaVersion ||
                    (kind != static_cast<std::uint32_t>(
                         lux::rdesc::Script::Kind::LUA_SOURCE) &&
                     kind != static_cast<std::uint32_t>(
                         lux::rdesc::Script::Kind::NATIVE_MODULE) &&
                     kind != static_cast<std::uint32_t>(
                         lux::rdesc::Script::Kind::CPP_STATIC)))
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }

                lux::rdesc::Script description;
                std::size_t decoded = sizeof(ScriptArtifact);
                const auto limit = max_decoded_bytes;
                if (decoded > limit ||
                    !reader.string(description.module_name, decoded, limit) ||
                    !reader.u64(description.lifecycle.begin_play) ||
                    !reader.u64(description.lifecycle.end_play))
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }

                std::uint32_t count{};
                if (!reader.u32(count) || !reserveRecords(
                        description.exports, count, decoded, limit,
                        reader.remaining()))
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                for (std::uint32_t index{}; index < count; ++index)
                {
                    lux::rdesc::ScriptFunction function;
                    if (!reader.string(function.name, decoded, limit) ||
                        !reader.u64(function.symbol_id))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    std::uint32_t type_count{};
                    if (!reader.u32(type_count) || !reserveRecords(
                            function.args, type_count, decoded, limit,
                            reader.remaining()))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    for (std::uint32_t type{}; type < type_count; ++type)
                    {
                        lux::rdesc::ScriptValueType value;
                        if (!readType(reader, value, decoded, limit))
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        function.args.push_back(std::move(value));
                    }
                    if (!reader.u32(type_count) || !reserveRecords(
                            function.returns, type_count, decoded, limit,
                            reader.remaining()))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    for (std::uint32_t type{}; type < type_count; ++type)
                    {
                        lux::rdesc::ScriptValueType value;
                        if (!readType(reader, value, decoded, limit))
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        function.returns.push_back(std::move(value));
                    }
                    description.exports.push_back(std::move(function));
                }

                if (!reader.u32(count) || !reserveRecords(
                        description.dependencies, count, decoded, limit,
                        reader.remaining()))
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                for (std::uint32_t index{}; index < count; ++index)
                {
                    lux::rdesc::ScriptDependency dependency;
                    if (!reader.string(dependency.kind, decoded, limit) ||
                        !reader.string(dependency.id, decoded, limit))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    description.dependencies.push_back(std::move(dependency));
                }
                if (!reader.u32(count) || !reserveRecords(
                        description.api_requirements, count, decoded, limit,
                        reader.remaining()))
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                for (std::uint32_t index{}; index < count; ++index)
                {
                    std::string name;
                    std::uint64_t hash{};
                    std::uint64_t schema{};
                    if (!reader.string(name, decoded, limit) || !reader.u64(hash) || !reader.u64(schema) ||
                        name.empty() || lux::cxx::Fnv1a64::hash(name) != hash)
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    description.api_requirements.push_back({lux::script::ScriptApiContractId{name}, schema});
                }
                if (!reader.u32(count) || !reserveRecords(
                        description.event_requirements,
                        count,
                        decoded,
                        limit,
                        reader.remaining()
                    ))
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                for (std::uint32_t index{}; index < count; ++index)
                {
                    lux::script::ScriptEventSourceDescription requirement;
                    std::uint32_t route{};
                    std::uint32_t abi_kind{};
                    if (!reader.string(requirement.system_name, decoded, limit) ||
                        !reader.string(requirement.event_name, decoded, limit) ||
                        !reader.u64(requirement.system_id) || !reader.u64(requirement.event_id) ||
                        !reader.u32(route) || !reader.string(requirement.payload.canonical_name, decoded, limit) ||
                        !reader.u64(requirement.payload.type_id) || !reader.u32(abi_kind) ||
                        !reader.u32(requirement.payload.size) || !reader.u32(requirement.payload.alignment) ||
                        !reader.u64(requirement.payload_schema_hash) ||
                        !reader.u32(requirement.payload_schema_version) ||
                        route > static_cast<std::uint32_t>(lux::script::EScriptEventRoute::ENTITY_TARGETED) ||
                        abi_kind > std::numeric_limits<std::uint8_t>::max())
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    requirement.route = static_cast<lux::script::EScriptEventRoute>(route);
                    requirement.payload.abi_kind = static_cast<std::uint8_t>(abi_kind);
                    description.event_requirements.push_back(std::move(requirement));
                }
                auto& provenance = description.provenance;
                if (!reader.string(provenance.compiler_id, decoded, limit) ||
                    !reader.string(provenance.compiler_version, decoded, limit) ||
                    !reader.string(provenance.source_id, decoded, limit) ||
                    !reader.string(provenance.source_hash, decoded, limit) ||
                    !reader.string(provenance.built_at, decoded, limit))
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }

                const auto script_kind = static_cast<lux::rdesc::Script::Kind>(kind);
                if (script_kind == lux::rdesc::Script::Kind::LUA_SOURCE)
                {
                    lux::rdesc::LuaSourceScript lua;
                    if (!reader.string(lua.entry, decoded, limit) || !reader.u32(count) ||
                        !reserveRecords(
                            lua.suspension_capable_exports,
                            count,
                            decoded,
                            limit,
                            reader.remaining()
                        ))
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    for (std::uint32_t index{}; index < count; ++index)
                    {
                        lux::script::ScriptSymbolId symbol{};
                        if (!reader.u64(symbol))
                            return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                        lua.suspension_capable_exports.push_back(symbol);
                    }
                    description.body = std::move(lua);
                }
                else if (script_kind == lux::rdesc::Script::Kind::NATIVE_MODULE)
                {
                    lux::rdesc::NativeModuleScript native;
                    std::uint32_t defaults_size{};
                    if (!reader.u32(native.abi_version) ||
                        !reader.u64(native.state_layout_hash) ||
                        !reader.u32(native.state_size) ||
                        !reader.u32(native.state_align) ||
                        !reader.u32(defaults_size) ||
                        defaults_size > reader.remaining() ||
                        defaults_size > limit - decoded)
                    {
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    }
                    std::span<const std::byte> defaults;
                    if (!reader.take(defaults_size, defaults))
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    native.state_defaults.assign(defaults.begin(), defaults.end());
                    decoded += defaults_size;
                    description.body = std::move(native);
                }
                else if (script_kind == lux::rdesc::Script::Kind::CPP_STATIC)
                {
                    lux::rdesc::CppStaticScript cpp_static;
                    if (!reader.string(cpp_static.descriptor, decoded, limit))
                        return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                    description.body = std::move(cpp_static);
                }
                else
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }

                std::uint64_t payload_size{};
                if (!reader.u64(payload_size) ||
                    payload_size > reader.remaining() ||
                    payload_size > limit - decoded)
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                std::span<const std::byte> payload;
                if (!reader.take(static_cast<std::size_t>(payload_size), payload) ||
                    reader.remaining() != 0U)
                {
                    return lux::cxx::unexpected(EAssetCodecError::CODEC_FAILURE);
                }
                std::vector<std::byte> artifact_payload(payload.begin(), payload.end());
                decoded += static_cast<std::size_t>(payload_size);
                auto artifact = ScriptArtifact::create(std::move(description), std::move(artifact_payload));
                if (!artifact)
                {
                    const auto error = artifact.error() == EScriptArtifactError::ALLOCATION_FAILURE
                        ? EAssetCodecError::OUT_OF_MEMORY
                        : EAssetCodecError::CODEC_FAILURE;
                    return lux::cxx::unexpected(error);
                }
                return std::make_shared<const ScriptArtifact>(std::move(*artifact));
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

    lux::cxx::expected<ScriptArtifact, EScriptArtifactError>
    ScriptArtifact::create(lux::rdesc::Script description, std::vector<std::byte> payload) noexcept
    {
        if (!lux::rdesc::detail::validScriptBody(description))
            return lux::cxx::unexpected(EScriptArtifactError::INVALID_DESCRIPTION);

        ScriptArtifact artifact{std::move(description), std::move(payload)};
        try
        {
            artifact.export_index_.reserve(artifact.description_.exports.size());
            for (std::size_t index{}; index < artifact.description_.exports.size(); ++index)
            {
                const auto& function = artifact.description_.exports[index];
                if (!lux::rdesc::detail::validScriptFunction(function))
                    return lux::cxx::unexpected(EScriptArtifactError::INVALID_DESCRIPTION);
                const auto symbol = function.symbol_id;
                if (!artifact.export_index_.emplace(symbol, index).second)
                    return lux::cxx::unexpected(EScriptArtifactError::INVALID_DESCRIPTION);
            }
            std::unordered_set<std::uint64_t> requirements;
            requirements.reserve(artifact.description_.api_requirements.size());
            for (const auto& requirement : artifact.description_.api_requirements)
            {
                if (!requirement.contract.isValid() || requirement.expected_schema_hash == 0U ||
                    !requirements.insert(requirement.contract.hash()).second)
                {
                    return lux::cxx::unexpected(EScriptArtifactError::INVALID_DESCRIPTION);
                }
            }
            const auto& event_requirements = artifact.description_.event_requirements;
            if (!std::ranges::is_sorted(event_requirements, ScriptEventSourceLess{}))
                return lux::cxx::unexpected(EScriptArtifactError::INVALID_DESCRIPTION);
            for (std::size_t index{}; index < event_requirements.size(); ++index)
            {
                const auto& requirement = event_requirements[index];
                if (!requirement.valid())
                    return lux::cxx::unexpected(EScriptArtifactError::INVALID_DESCRIPTION);
                for (std::size_t previous{}; previous < index; ++previous)
                {
                    const auto& candidate = event_requirements[previous];
                    const bool duplicate_identity = candidate.system_id == requirement.system_id &&
                        candidate.event_id == requirement.event_id;
                    const bool duplicate_name = candidate.system_name == requirement.system_name &&
                        candidate.event_name == requirement.event_name;
                    if (duplicate_identity || duplicate_name)
                        return lux::cxx::unexpected(EScriptArtifactError::INVALID_DESCRIPTION);
                }
            }
            return artifact;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptArtifactError::ALLOCATION_FAILURE);
        }
    }

    const lux::rdesc::ScriptFunction* ScriptArtifact::findExport(ScriptSymbolId symbol) const noexcept
    {
        const auto found = export_index_.find(symbol);
        return found == export_index_.end() ? nullptr : &description_.exports[found->second];
    }

    ScriptArtifactAsset::ScriptArtifactAsset(
        lux::asset::AssetInfo info,
        std::shared_ptr<const ScriptArtifact> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const ScriptArtifactAsset>, lux::asset::AssetDecodeFailure>
    ScriptArtifactAsset::create(
        lux::asset::AssetInfo info,
        std::shared_ptr<const ScriptArtifact> data,
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
            return std::shared_ptr<const ScriptArtifactAsset>(new ScriptArtifactAsset(
                std::move(info), std::move(data), std::move(auxiliary)
            ));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                lux::asset::EAssetDecodeError::ALLOCATION_FAILURE,
                0U
            });
        }
    }
} // namespace lux::script

namespace lux::asset
{
    lux::cxx::expected<std::shared_ptr<const lux::script::ScriptArtifactAsset>, AssetDecodeFailure>
    TAssetSerDeser<lux::script::ScriptArtifactAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> bytes,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(bytes), limits);
        if (!image) return lux::cxx::unexpected(image.error());
        if (image->magic() != lux::script::ScriptArtifactAsset::primary_magic)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_MAGIC, 0U});
        if (image->metadata().legacy_type_tag != lux::script::ScriptArtifactAsset::legacy_type_tag)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_TYPE, 0U});
        if (!image->information().empty())
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_LAYOUT, 0U});
        auto artifact = lux::script::detail::decodeScriptArtifact(
            image->data().view(),
            image->data().size(),
            limits.max_decoded_bytes
        );
        if (!artifact)
        {
            const auto code = artifact.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetDecodeError::ALLOCATION_FAILURE
                : EAssetDecodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetDecodeFailure{code, 0U});
        }
        try
        {
            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(), image->auxiliaryPayloads().end()
            );
            return lux::script::ScriptArtifactAsset::create(
                AssetInfo{
                    image->metadata().id,
                    lux::script::ScriptArtifactAsset::asset_type,
                    image->metadata().date,
                    image->metadata().display_name,
                    image->metadata().source_path,
                    image->metadata().source_mtime
                },
                std::move(*artifact),
                std::move(auxiliary)
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::ALLOCATION_FAILURE, 0U});
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<lux::script::ScriptArtifactAsset>::encode(
        const lux::script::ScriptArtifactAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        auto payload = lux::script::detail::encodeScriptArtifact(asset.data(), limits.max_encoded_bytes);
        if (!payload)
        {
            const auto code = payload.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetEncodeError::ALLOCATION_FAILURE
                : EAssetEncodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetEncodeFailure{code, 0U});
        }
        return detail::encodeCookedAssetImage(
            detail::CookedAssetWriteRequest{
                lux::script::ScriptArtifactAsset::primary_magic,
                lux::script::ScriptArtifactAsset::legacy_type_tag,
                asset.info(),
                {},
                *payload,
                asset.auxiliaryPayloads()
            },
            limits
        );
    }
} // namespace lux::asset
