#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/detail/SimulationDescriptionFailureInjection.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace
{
    using namespace lux::simulation;

    [[nodiscard]] SimulationDescription buildDescription(bool reverse)
    {
        SimulationDescriptionBuilder builder;
        const auto gameplay = simulationDataSchemaId("test.gameplay");
        const auto spatial = simulationDataSchemaId("test.spatial3d");
        const std::array gameplay_payload{std::byte{1U}, std::byte{2U}};
        const std::array spatial_payload{std::byte{3U}};
        if (reverse)
        {
            assert(builder.addData(spatial, 7U, spatial_payload));
            assert(builder.addData(gameplay, 2U, gameplay_payload));
        }
        else
        {
            assert(builder.addData(gameplay, 2U, gameplay_payload));
            assert(builder.addData(spatial, 7U, spatial_payload));
        }
        auto result = std::move(builder).build();
        assert(result);
        return std::move(*result);
    }

    void assertEquivalent(
        const SimulationDescription& left,
        const SimulationDescription& right
    )
    {
        assert(left.dataCount() == right.dataCount());
        assert(left.payloadBytes() == right.payloadBytes());
        for (std::size_t index{}; index < left.dataCount(); ++index)
        {
            const auto left_data = left.dataAt(index);
            const auto right_data = right.dataAt(index);
            assert(left_data.schema() == right_data.schema());
            assert(left_data.version() == right_data.version());
            assert(left_data.payload().size() == right_data.payload().size());
            for (std::size_t byte{}; byte < left_data.payload().size(); ++byte)
                assert(left_data.payload()[byte] == right_data.payload()[byte]);
        }
    }
}

int main()
{
    using namespace lux::simulation;

    SimulationDescriptionBuilder validation;
    assert(!validation.addData({}, 1U, {}));
    const auto gameplay = simulationDataSchemaId("test.gameplay");
    assert(!validation.addData(gameplay, 0U, {}));
    assert(validation.addData(gameplay, 1U, {}));
    assert(!validation.addData(gameplay, 1U, {}));
    assert(!validation.eraseData(simulationDataSchemaId("test.missing")));

    const std::array replacement{std::byte{9U}, std::byte{8U}};
    assert(validation.setData(gameplay, 4U, replacement));
    auto validated = std::move(validation).build();
    assert(validated);
    assert(validated->dataCount() == 1U);
    assert(validated->findData(gameplay));
    assert(validated->findData(gameplay).version() == 4U);
    assert(validated->findData(gameplay).payload().size() == 2U);
    assert(validated->retainedBytes() >= validated->payloadBytes());
    assert(!validated->findData(simulationDataSchemaId("test.missing")));
    assert(!validated->dataAt(1U));

    auto canonical_a = buildDescription(false);
    auto canonical_b = buildDescription(true);
    assertEquivalent(canonical_a, canonical_b);

    SimulationDescriptionBuilder recoverable;
    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::MUTATION_ALLOCATION
    );
    const auto mutation_failure = recoverable.addData(gameplay, 1U, {});
    assert(!mutation_failure);
    assert(
        mutation_failure.error().code ==
        ESimulationDescriptionError::ALLOCATION_FAILURE
    );
    assert(recoverable.addData(gameplay, 1U, {}));

    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::BUILD_ALLOCATION
    );
    const auto build_failure = std::move(recoverable).build();
    assert(!build_failure);
    assert(
        build_failure.error().code ==
        ESimulationDescriptionError::ALLOCATION_FAILURE
    );
    detail::failNextSimulationDescriptionOperationForTest(
        detail::ESimulationDescriptionFailurePoint::BUILD_SIZE_OVERFLOW
    );
    const auto overflow_failure = std::move(recoverable).build();
    assert(!overflow_failure);
    assert(
        overflow_failure.error().code ==
        ESimulationDescriptionError::SIZE_OVERFLOW
    );
    auto recovered = std::move(recoverable).build();
    assert(recovered);
    assert(recovered->dataCount() == 1U);

    SimulationDescriptionBuilder erase;
    assert(erase.addData(gameplay, 1U, {}));
    assert(erase.eraseData(gameplay));
    auto empty = std::move(erase).build();
    assert(empty);
    assert(empty->empty());

    return 0;
}
