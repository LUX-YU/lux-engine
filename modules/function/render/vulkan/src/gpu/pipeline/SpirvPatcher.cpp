#include <lux/engine/render/gpu/pipeline/SpirvPatcher.hpp>

#include <algorithm>
#include <unordered_map>

namespace lux::render
{
namespace
{
    // All the constants this file needs from the SPIR-V binary format.
    // Spec §2.3 (physical layout) and §3.20 (Decoration).
    constexpr uint32_t kMagic            = 0x07230203u;
    constexpr uint32_t kMagicReversed    = 0x03022307u;   ///< a module with reversed byte order
    constexpr std::size_t kHeaderWords   = 5;             ///< magic/version/generator/bound/schema
    constexpr uint32_t kOpDecorate       = 71u;
    constexpr uint32_t kDecorationBinding       = 33u;
    constexpr uint32_t kDecorationDescriptorSet = 34u;

    /// OpDecorate word layout: [0]=opcode+wordcount, [1]=target id,
    /// [2]=decoration, [3]=literal (only present when the decoration takes a
    /// parameter).
    constexpr std::size_t kDecorateTargetWord     = 1;
    constexpr std::size_t kDecorateDecorationWord = 2;
    constexpr std::size_t kDecorateLiteralWord    = 3;
    constexpr uint32_t    kDecorateMinWordCount   = 4;   ///< when carrying one literal parameter

    struct Position
    {
        uint32_t set{0};
        uint32_t binding{0};
        bool     has_set{false};
        bool     has_binding{false};
    };
} // namespace

SpirvPatchResult patchSpirvDescriptorPositions(
    std::span<uint32_t> words,
    std::span<const SpirvRelocation> relocations) noexcept
{
    SpirvPatchResult r{};

    if (words.size() < kHeaderWords)
    {
        r.error = "SPIR-V too short (no header)";
        return r;
    }
    if (words[0] == kMagicReversed)
    {
        // A byte-swapped module would need a full byte-swap pass before it
        // can be parsed. A module produced by the engine itself is never
        // like this, so seeing one means the source is suspect — better to
        // error out than to silently byte-swap and let the caller use it
        // as-is.
        r.error = "SPIR-V is byte-swapped (foreign endianness)";
        return r;
    }
    if (words[0] != kMagic)
    {
        r.error = "not a SPIR-V module (bad magic)";
        return r;
    }
    // (The first pass runs even when relocations is empty — descriptor_count
    //  is produced unconditionally, and the caller uses it to cross-check
    //  against reflection.)

    // -- Pass 1: collect the current (set, binding) for every decorated ----
    // -- variable ------------------------------------------------------------
    // These must all be collected before anything is rewritten: a variable's
    // DescriptorSet and Binding decorations are two separate instructions,
    // and their order of appearance isn't guaranteed. A single read-and-patch
    // pass would end up matching relocations against a mix of stale and
    // already-patched positions.
    std::unordered_map<uint32_t, Position> positions;
    // Also record each instruction's own word index so pass 2 can write
    // directly without re-scanning.
    std::unordered_map<uint32_t, std::size_t> set_word;
    std::unordered_map<uint32_t, std::size_t> binding_word;

    for (std::size_t i = kHeaderWords; i < words.size();)
    {
        const uint32_t word_count = words[i] >> 16;
        const uint32_t opcode     = words[i] & 0xFFFFu;

        // word_count == 0 would make the scan loop in place forever — a
        // classic symptom of a corrupted module.
        if (word_count == 0 || i + word_count > words.size())
        {
            r.error = "malformed SPIR-V (bad instruction word count)";
            return r;
        }

        if (opcode == kOpDecorate && word_count >= kDecorateMinWordCount)
        {
            const uint32_t target     = words[i + kDecorateTargetWord];
            const uint32_t decoration = words[i + kDecorateDecorationWord];
            const std::size_t literal = i + kDecorateLiteralWord;

            if (decoration == kDecorationDescriptorSet)
            {
                positions[target].set     = words[literal];
                positions[target].has_set = true;
                set_word[target]          = literal;
            }
            else if (decoration == kDecorationBinding)
            {
                positions[target].binding     = words[literal];
                positions[target].has_binding = true;
                binding_word[target]          = literal;
            }
        }

        i += word_count;
    }

    // -- Pass 2: match against the pre-patch positions and rewrite --------
    for (const auto& [target, pos] : positions)
    {
        // A decoration with only a set or only a binding isn't a descriptor
        // resource (e.g. a push constant block only carries other
        // decorations) — skip it.
        if (!pos.has_set || !pos.has_binding)
            continue;
        ++r.descriptor_count;

        const auto it = std::find_if(
            relocations.begin(), relocations.end(),
            [&pos](const SpirvRelocation& reloc) {
                return reloc.from_set == pos.set && reloc.from_binding == pos.binding;
            });
        if (it == relocations.end())
            continue;

        const bool moved = (it->to_set != pos.set) || (it->to_binding != pos.binding);
        if (!moved)
            continue;   // identity relocation: don't write, don't count

        words[set_word[target]]     = it->to_set;
        words[binding_word[target]] = it->to_binding;
        ++r.relocated;
    }

    r.ok = true;
    return r;
}

SpirvPatchResult patchSpirvDescriptorPositions(
    std::span<const uint32_t> words,
    std::span<const SpirvRelocation> relocations,
    std::vector<uint32_t>& out)
{
    out.assign(words.begin(), words.end());
    auto r = patchSpirvDescriptorPositions(std::span<uint32_t>{out}, relocations);
    if (!r.ok)
        out.assign(words.begin(), words.end());   // on failure, guarantee out == input
    return r;
}

} // namespace lux::render
