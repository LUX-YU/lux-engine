#include <lux/engine/render/core/Descriptors.hpp>
#include <lux/engine/render/core/Handles.hpp>
#include <lux/engine/render/core/Target.hpp>

#include <array>
#include <cstdio>
#include <type_traits>

namespace
{
    using namespace lux::render;

    constexpr auto kDataId = renderDataTypeId("test.value.v1");
    constexpr RenderDataDescriptor kData{kDataId, "test.value.v1", 1, 1, 16, 8};
    constexpr auto kCapability = sceneCapabilityId("test.capability.v1");
    constexpr auto kFeature = featureTypeId("test.feature.v1");

    static_assert(renderDataTypeId("hello").value() == 0xa430d84680aabd0bULL);
    static_assert(renderDataTypeId("\xc3\xa9").value() == lux::cxx::Fnv1a64::hash("\xc3\xa9"));
    static_assert(!renderDataTypeId("").isValid());
    static_assert(!FeatureTypeId{}.isValid());
    static_assert(!SceneCapabilityId{}.isValid());
    static_assert(!RenderTargetSemanticId{}.isValid());
    static_assert(!std::is_convertible_v<RenderDataTypeId, FeatureTypeId>);
    static_assert(!std::is_convertible_v<RenderSceneId, RenderViewHandle>);
    static_assert(std::is_trivially_copyable_v<RenderSceneId>);
    static_assert(std::is_trivially_copyable_v<RenderDataDescriptor>);
    static_assert(noexcept(validateDescriptor(kData)));
    static_assert(noexcept(validateCompatible(kData, kData)));
    static_assert(kSceneColorSemantic != kDepthSemantic);

    bool check(bool passed, const char* name)
    {
        if (!passed)
        {
            std::fprintf(stderr, "FAIL: %s\n", name);
        }
        return passed;
    }

    bool failsWith(const RenderResult<void>& result, lux::error::ErrorId id)
    {
        return !result && result.error().type == id;
    }
}

int main()
{
    bool passed = check(bool(validateDescriptor(kData)), "valid data");
    auto candidate = kData;
    candidate.canonical_name = "";
    passed &= check(failsWith(validateDescriptor(candidate), kInvalidIdentity), "empty canonical name");
    candidate = kData;
    candidate.id = RenderDataTypeId{42};
    passed &= check(failsWith(validateDescriptor(candidate), kInvalidIdentity), "forged identity");

    candidate = kData;
    candidate.wire_version = 0;
    passed &= check(failsWith(validateDescriptor(candidate), kInvalidVersion), "zero wire version");
    candidate = kData;
    candidate.layout_version = 0;
    passed &= check(failsWith(validateDescriptor(candidate), kInvalidVersion), "zero layout version");

    for (const auto alignment : {0u, 3u, 32u})
    {
        candidate = kData;
        candidate.alignment = alignment;
        const auto result = validateDescriptor(candidate);
        const bool has_expected_error = failsWith(result, kInvalidLayout) &&
            result.error().args == std::array<std::uint64_t, 3>{kDataId.value(), 16, alignment};
        passed &= check(has_expected_error, "alignment and structured error arguments");
    }
    candidate = kData;
    candidate.size = 0;
    passed &= check(failsWith(validateDescriptor(candidate), kInvalidLayout), "empty native value");
    candidate = kData;
    candidate.size = 17;
    passed &= check(failsWith(validateDescriptor(candidate), kInvalidLayout), "size is not alignment multiple");

    passed &= check(bool(validateCompatible(kData, kData)), "compatible definition");
    candidate = kData;
    candidate.wire_version = 2;
    passed &= check(failsWith(validateCompatible(kData, candidate), kDefinitionMismatch), "wire mismatch");
    candidate = kData;
    candidate.layout_version = 2;
    passed &= check(failsWith(validateCompatible(kData, candidate), kDefinitionMismatch), "layout version mismatch");
    candidate = kData;
    candidate.size = 32;
    passed &= check(failsWith(validateCompatible(kData, candidate), kDefinitionMismatch), "size mismatch");
    candidate = kData;
    candidate.alignment = 4;
    passed &= check(failsWith(validateCompatible(kData, candidate), kDefinitionMismatch), "alignment mismatch");
    candidate = kData;
    candidate.id = renderDataTypeId("test.other.v1");
    passed &= check(failsWith(validateCompatible(kData, candidate), kDefinitionMismatch), "different id");
    // Inject equal hashes to exercise collision classification, not a claim to
    // have found a real FNV collision. Registration still validates each record.
    candidate = kData;
    candidate.canonical_name = "test.other.v1";
    passed &= check(failsWith(validateCompatible(kData, candidate), kIdentityCollision), "collision classification");

    SceneCapabilityDescriptor capability{kCapability, "test.capability.v1", 1};
    passed &= check(bool(validateDescriptor(capability)), "valid capability");
    capability.contract_version = 0;
    passed &= check(failsWith(validateDescriptor(capability), kInvalidVersion), "capability version");
    capability.canonical_name = "other";
    passed &= check(failsWith(validateDescriptor(capability), kInvalidIdentity), "capability identity");

    std::array capabilities{kCapability, sceneCapabilityId("test.second.v1")};
    FeatureDescriptor feature{kFeature, "test.feature.v1", 1, capabilities, {}};
    passed &= check(bool(validateDescriptor(feature)), "valid static feature");
    capabilities[1] = kCapability;
    passed &= check(failsWith(validateDescriptor(feature), kInvalidCapability), "duplicate provider");
    feature.provides = {};
    feature.required_capabilities = capabilities;
    passed &= check(failsWith(validateDescriptor(feature), kInvalidCapability), "duplicate requirement");
    capabilities[1] = {};
    passed &= check(failsWith(validateDescriptor(feature), kInvalidCapability), "null capability");
    feature.required_capabilities = {};
    feature.contract_version = 0;
    passed &= check(failsWith(validateDescriptor(feature), kInvalidVersion), "feature version");
    feature.id = {};
    passed &= check(failsWith(validateDescriptor(feature), kInvalidIdentity), "feature identity");

    lux::cxx::SlotMap<int, RenderSceneTag> scenes;
    const RenderSceneId first = scenes.emplace(7);
    passed &= check(!RenderSceneId{}.isValid(), "null handle");
    passed &= check(scenes.erase(first), "owner removes scene");
    const RenderSceneId second = scenes.emplace(9);
    const bool has_generation_protection = first.index == second.index && first.gen != second.gen &&
        first != second && scenes.find(first) == nullptr;
    passed &= check(has_generation_protection, "reused slot rejects stale generation");
    const auto* value = scenes.find(second);
    passed &= check(value && *value == 9, "new generation resolves");

    const auto errors = renderCoreErrorDescriptors();
    constexpr std::array error_ids{
        kInvalidIdentity, kInvalidLayout, kInvalidVersion,
        kInvalidCapability, kIdentityCollision, kDefinitionMismatch
    };
    passed &= check(errors.size() == error_ids.size(), "static error catalog size");
    for (std::size_t index = 0; index < errors.size(); ++index)
    {
        const bool has_expected_descriptor = index < error_ids.size() &&
            lux::error::errorId(errors[index].name) == error_ids[index] && !errors[index].message.empty();
        passed &= check(has_expected_descriptor, "error id resolves to static descriptor");
    }
    return passed ? 0 : 1;
}
