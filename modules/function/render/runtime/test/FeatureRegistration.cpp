#include "ErasedRegistrationFixture.hpp"
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <cstdlib>

using namespace lux::render;
static void check(bool value) { if (!value) std::abort(); }
struct First final
{
    static constexpr std::string_view name = "test.registration.first";
    using Request = std::uint32_t;
    using Response = Ack;
};
struct Second final
{
    static constexpr std::string_view name = "test.registration.second";
    using Request = std::uint32_t;
    using Response = Ack;
};
static RenderResult<void> describe(const WireView*, std::string_view, std::vector<ResourceClaim>&) noexcept
{
    return {};
}
static RenderResult<void> handler(void*, CommandInvocation&) noexcept { return {}; }

int main()
{
    const FeatureDescriptor first{FeatureTypeIdView("test.registration.a"), {1, 0}, nullptr};
    const FeatureDescriptor second{FeatureTypeIdView("test.registration.b"), {1, 0}, nullptr};
    const FeatureGraphOps graph{describe, nullptr};
    // Assembly is independent of Commands; repeated instances remain legal.
    const FeatureInstanceView instances[]{{&first, &graph, nullptr, "one"}, {&first, &graph, nullptr, "two"}};
    check(bool(assembleFeatures(instances, {})));
    for (unsigned fault = 0; fault < 7; ++fault)
    {
        auto created = RenderServer::create({"test.feature.registration", false});
        check(bool(created));
        auto server = std::move(*created);
        auto original = commandDescriptor<First>();
        const std::array initial{test_registration::erased(&original, handler)};
        check(bool(server->registerFeature<First>({&first, &graph, nullptr, initial})));
        auto candidate = commandDescriptor<First>();
        if (fault == 1) ++candidate.version.minor;
        if (fault == 2) candidate.completion = ECompletionContract::RESOURCE_GPU_READY;
        if (fault == 3) candidate.response = candidate.request;
        if (fault >= 4) candidate = commandDescriptor<Second>();
        if (fault == 5) candidate.version = {};
        const std::array next{test_registration::erased(&candidate, handler)};
        if (fault < 4) check(!server->registerFeature<First>({&second, &graph, nullptr, next}));
        else if (fault == 4) check(bool(server->registerFeature<Second>({&second, &graph, nullptr, next})));
        else if (fault == 5) check(!server->registerFeature<Second>({&second, &graph, nullptr, next}));
        else
        {
            const std::array repeated{next[0], next[0]};
            check(!server->registerFeature<Second, Second>({&second, &graph, nullptr, repeated}));
        }
    }
}
