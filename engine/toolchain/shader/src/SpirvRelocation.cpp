#include <lux/engine/toolchain/shader/SpirvRelocation.hpp>

#include <algorithm>
#include <spirv-tools/libspirv.h>

namespace lux::toolchain
{
    namespace
    {
        cxx::expected<void, std::string> validateBinary(std::span<const std::uint32_t> words) noexcept
        {
            const auto context = spvContextCreate(SPV_ENV_VULKAN_1_3);
            if (!context)
            {
                std::terminate();
            }
            spv_diagnostic diagnostic{};
            const auto result = spvValidateBinary(context, words.data(), words.size(), &diagnostic);
            const auto message = diagnostic && diagnostic->error ? std::string(diagnostic->error) : std::string{};
            spvDiagnosticDestroy(diagnostic);
            spvContextDestroy(context);
            if (result != SPV_SUCCESS)
            {
                return cxx::unexpected("spirv-val: " + message);
            }
            return {};
        }

        struct Pair
        {
            std::size_t set_word{}, binding_word{};
            bool variable{};
        };
    } // namespace

    cxx::expected<RelocatedShader, std::string> relocatePassSpirv(
        std::span<const std::uint32_t> original,
        const rdesc::PassShaderContract& schema,
        std::span<const PassDescriptorLocation> final_locations,
        std::uint32_t expected_stage
    ) noexcept
    {
        const bool invalid_header = original.size() < 5 || original[0] != 0x07230203 || original[1] < 0x00010000 ||
                                    original[1] > 0x00010600 || original[3] == 0 || original[3] > original.size() ||
                                    original[4] != 0;
        if (invalid_header)
        {
            return cxx::unexpected(std::string("Unsupported SPIR-V header/version/bound"));
        }
        std::vector<Pair> pairs(original[3]);
        std::uint32_t stage = 0, entries = 0;
        for (std::size_t cursor = 5; cursor < original.size();)
        {
            const auto length = original[cursor] >> 16;
            const auto opcode = original[cursor] & 0xffff;
            if (length == 0 || length > original.size() - cursor)
            {
                return cxx::unexpected(std::string("Invalid SPIR-V instruction length"));
            }
            // Decoration groups, ID/string decorations and Aliased variables are outside the cooked contract.
            if (opcode == 73 || opcode == 74 || opcode == 75 || opcode == 332 || opcode == 5632 || opcode == 5633)
            {
                return cxx::unexpected(std::string("Unsupported grouped/ID/string decoration"));
            }
            if (opcode == 15)
            {
                if (length < 4)
                {
                    return cxx::unexpected(std::string("Invalid entry point"));
                }
                ++entries;
                stage = original[cursor + 1] == 0
                            ? 1u
                            : (original[cursor + 1] == 4 ? 2u : (original[cursor + 1] == 5 ? 4u : 0u));
            }
            if (opcode == 59)
            {
                if (length < 4 || original[cursor + 2] >= pairs.size())
                {
                    return cxx::unexpected(std::string("Invalid variable identity"));
                }
                pairs[original[cursor + 2]].variable = true;
            }
            if (opcode == 71)
            {
                if (length < 3 || original[cursor + 1] >= pairs.size())
                {
                    return cxx::unexpected(std::string("Invalid decoration target"));
                }
                const auto decoration = original[cursor + 2];
                if (decoration == 20)
                {
                    return cxx::unexpected(std::string("Unsupported Aliased decoration"));
                }
                if (decoration == 33 || decoration == 34)
                {
                    if (length != 4)
                    {
                        return cxx::unexpected(std::string("Invalid descriptor decoration length"));
                    }
                    auto& pair = pairs[original[cursor + 1]];
                    auto& word = decoration == 33 ? pair.binding_word : pair.set_word;
                    if (word != 0)
                    {
                        return cxx::unexpected(std::string("Duplicate descriptor decoration"));
                    }
                    word = cursor + 3;
                }
            }
            cursor += length;
        }
        if (entries != 1 || stage == 0 || stage != expected_stage)
        {
            return cxx::unexpected(std::string("Unexpected or multiple Shader entry points/stages"));
        }
        auto original_valid = validateBinary(original);
        if (!original_valid)
        {
            return cxx::unexpected(original_valid.error());
        }
        auto original_contract = validatePassSpirv(original, schema);
        if (!original_contract)
        {
            return cxx::unexpected(original_contract.error());
        }
        RelocatedShader result{
            {original.begin(), original.end()},
            {original.begin(), original.end()},
            {final_locations.begin(), final_locations.end()},
            stage
        };
        std::size_t patched = 0;
        for (const auto& pair : pairs)
        {
            if (pair.set_word == 0 && pair.binding_word == 0)
            {
                continue;
            }
            if (!pair.variable || pair.set_word == 0 || pair.binding_word == 0)
            {
                return cxx::unexpected(std::string("Missing descriptor pair or unknown variable"));
            }
            const auto old_set = original[pair.set_word], old_binding = original[pair.binding_word];
            std::uint32_t provisional = 0;
            const PassDescriptorLocation* location = nullptr;
            for (std::uint32_t field = 0; field < schema.resources.size(); ++field)
            {
                if (!rdesc::isShaderDescriptorRole(schema.resources[field].role))
                {
                    continue;
                }
                if (old_set == 0 && old_binding == provisional && (schema.resources[field].stages & stage) != 0)
                {
                    const auto found = std::find_if(
                        final_locations.begin(),
                        final_locations.end(),
                        [&](const auto& item) { return item.field_index == field; }
                    );
                    if (found != final_locations.end())
                    {
                        location = &*found;
                    }
                }
                ++provisional;
            }
            if (!location)
            {
                return cxx::unexpected(std::string("Missing final field placement"));
            }
            result.words[pair.set_word] = location->set;
            result.words[pair.binding_word] = location->binding;
            ++patched;
        }
        const auto expected = std::count_if(
            schema.resources.begin(),
            schema.resources.end(),
            [&](const auto& field) { return rdesc::isShaderDescriptorRole(field.role) && (field.stages & stage) != 0; }
        );
        if (patched != expected)
        {
            return cxx::unexpected(std::string("Descriptor coverage mismatch"));
        }
        auto final_valid = validateBinary(result.words);
        if (!final_valid)
        {
            return cxx::unexpected(final_valid.error());
        }
        auto reconciled = validatePassSpirv(result.words, schema, final_locations);
        if (!reconciled)
        {
            return cxx::unexpected(reconciled.error());
        }
        return result;
    }
} // namespace lux::toolchain
