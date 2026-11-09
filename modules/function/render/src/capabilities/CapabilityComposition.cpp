#include <lux/engine/render/capabilities/CapabilityComposition.hpp>
#include <lux/engine/render/extensions/features/FeatureGraphOps.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

namespace lux::render
{
    namespace
    {
        bool validDescription(const CapabilityCompositionDescription& value) noexcept
        {
            if (value.contributions.empty())
            {
                return value.outputs.empty();
            }
            if (value.outputs.empty())
            {
                return false;
            }
            std::set<std::string_view> keys;
            for (const auto& selected : value.contributions)
            {
                const bool invalid_name =
                    selected.key.empty() || selected.capability.empty() || selected.implementation.empty();
                const bool invalid_version = selected.configuration.version == 0;
                if (invalid_name || invalid_version || !keys.insert(selected.key).second)
                {
                    return false;
                }
            }
            const auto valid_reference = [&](const CapabilityOutput& reference) noexcept
            { return keys.contains(reference.contribution) && !reference.name.empty(); };
            const bool invalid_output = !std::ranges::all_of(value.outputs, valid_reference);
            return !invalid_output;
        }
    } // namespace

} // namespace lux::render

namespace lux::serialization
{
    namespace
    {
        auto compositionFields(auto& value) noexcept
            requires std::same_as<std::remove_cvref_t<decltype(value)>, render::CapabilityOutput>
        {
            return std::tie(value.contribution, value.name);
        }

        auto compositionFields(auto& value) noexcept
            requires std::same_as<std::remove_cvref_t<decltype(value)>, render::CapabilityContributionDescription>
        {
            return std::tie(
                value.key,
                value.capability,
                value.implementation,
                value.configuration.schema,
                value.configuration.version,
                value.configuration.bytes
            );
        }

        auto compositionFields(auto& value) noexcept
            requires std::same_as<std::remove_cvref_t<decltype(value)>, render::CapabilityCompositionDescription>
        {
            return std::tie(value.contributions, value.outputs);
        }

        template <class Value> struct CompositionValueCodec
        {
            static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

            template <class Writer>
            static SerializationResult
            write(Writer& writer, const Value& value, const SerializationContext& context) noexcept
            {
                return serialization::write(writer, compositionFields(value), context);
            }

            template <class Reader>
            static SerializationResult read(Reader& reader, Value& value, const SerializationContext& context) noexcept
            {
                Value candidate;
                auto fields = compositionFields(candidate);
                auto result = serialization::read(reader, fields, context);
                if (result)
                {
                    value = std::move(candidate);
                }
                return result;
            }
        };
    } // namespace

    template <> struct TSerializer<render::CapabilityOutput> : CompositionValueCodec<render::CapabilityOutput>
    {
    };

    template <>
    struct TSerializer<render::CapabilityContributionDescription>
        : CompositionValueCodec<render::CapabilityContributionDescription>
    {
    };

    template <>
    struct TSerializer<render::CapabilityCompositionDescription>
        : CompositionValueCodec<render::CapabilityCompositionDescription>
    {
    };
} // namespace lux::serialization

namespace lux::render
{
    bool validCapabilityComposition(const CapabilityCompositionDescription& value) noexcept
    {
        return !value.contributions.empty() && validDescription(value);
    }

    RenderResult<std::vector<std::byte>>
    encodeCapabilityComposition(const CapabilityCompositionDescription& value) noexcept
    {
        if (!validDescription(value))
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        std::vector<std::byte> result;
        serialization::BinaryWriter writer(result);
        const auto fields = std::tie(value);
        auto encoded =
            serialization::write(writer, std::tuple{std::uint32_t{4}}, serialization::kPortableMetadataBudget);
        if (encoded)
        {
            encoded = serialization::write(writer, fields, serialization::kPortableMetadataBudget);
        }
        if (!encoded)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    RenderResult<CapabilityCompositionDescription>
    decodeCapabilityComposition(std::span<const std::byte> bytes) noexcept
    {
        serialization::BinaryReader reader(bytes);
        std::uint32_t version = 0;
        CapabilityCompositionDescription result;
        auto fields = std::tie(version, result);
        const auto decoded = serialization::read(reader, fields, serialization::kPortableMetadataBudget);
        const bool invalid_encoding = !decoded || version != 4 || reader.remaining() != 0;
        if (invalid_encoding || !validDescription(result))
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }
} // namespace lux::render
