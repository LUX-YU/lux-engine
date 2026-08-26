#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/detail/SimulationDescriptionFailureInjection.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace lux::simulation
{
    struct SimulationDescriptionBuilder::Impl final
    {
        struct PendingData final
        {
            SimulationDataSchemaId schema;
            std::uint32_t version{};
            std::vector<std::byte> payload;
        };

        std::vector<PendingData> data;
    };

    namespace
    {
        [[nodiscard]] SimulationDescriptionFailure failure(
            ESimulationDescriptionError code,
            SimulationDataSchemaId schema = {}
        ) noexcept
        {
            return {code, std::move(schema)};
        }

        template <class Range>
        [[nodiscard]] auto findData(
            Range& range,
            const SimulationDataSchemaId& schema
        ) noexcept
        {
            return std::find_if(
                range.begin(),
                range.end(),
                [&](const auto& candidate) noexcept
                {
                    return candidate.schema == schema;
                }
            );
        }

        [[nodiscard]] bool failMutationForTest() noexcept
        {
            return detail::consumeSimulationDescriptionFailureForTest(
                detail::ESimulationDescriptionFailurePoint::MUTATION_ALLOCATION
            );
        }
    }

    SimulationDescriptionBuilder::SimulationDescriptionBuilder()
        : impl_(std::make_unique<Impl>())
    {
    }

    SimulationDescriptionBuilder::~SimulationDescriptionBuilder() = default;
    SimulationDescriptionBuilder::SimulationDescriptionBuilder(
        SimulationDescriptionBuilder&&
    ) noexcept = default;
    SimulationDescriptionBuilder& SimulationDescriptionBuilder::operator=(
        SimulationDescriptionBuilder&&
    ) noexcept = default;

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::addData(
        SimulationDataSchemaId schema,
        std::uint32_t version,
        std::span<const std::byte> payload
    ) noexcept
    {
        if (!schema.valid())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_ID,
                std::move(schema)
            ));
        if (version == 0U)
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_VERSION,
                std::move(schema)
            ));
        if (findData(impl_->data, schema) != impl_->data.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DUPLICATE_DATA,
                std::move(schema)
            ));
        if (failMutationForTest())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        try
        {
            impl_->data.push_back({
                std::move(schema),
                version,
                std::vector<std::byte>(payload.begin(), payload.end())
            });
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        }
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::setData(
        SimulationDataSchemaId schema,
        std::uint32_t version,
        std::span<const std::byte> payload
    ) noexcept
    {
        if (!schema.valid())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_ID,
                std::move(schema)
            ));
        if (version == 0U)
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::INVALID_SCHEMA_VERSION,
                std::move(schema)
            ));
        if (failMutationForTest())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        auto data = findData(impl_->data, schema);
        try
        {
            std::vector<std::byte> replacement(payload.begin(), payload.end());
            if (data == impl_->data.end())
            {
                impl_->data.push_back({
                    std::move(schema),
                    version,
                    std::move(replacement)
                });
            }
            else
            {
                data->version = version;
                data->payload = std::move(replacement);
            }
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE,
                std::move(schema)
            ));
        }
    }

    lux::cxx::expected<void, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::eraseData(
        const SimulationDataSchemaId& schema
    ) noexcept
    {
        auto data = findData(impl_->data, schema);
        if (data == impl_->data.end())
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::DATA_NOT_FOUND,
                schema
            ));
        impl_->data.erase(data);
        return {};
    }

    void SimulationDescriptionBuilder::clear() noexcept
    {
        impl_->data.clear();
    }

    lux::cxx::expected<SimulationDescription, SimulationDescriptionFailure>
    SimulationDescriptionBuilder::build() && noexcept
    {
        if (detail::consumeSimulationDescriptionFailureForTest(
                detail::ESimulationDescriptionFailurePoint::BUILD_ALLOCATION
            ))
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
        if (detail::consumeSimulationDescriptionFailureForTest(
                detail::ESimulationDescriptionFailurePoint::BUILD_SIZE_OVERFLOW
            ))
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::SIZE_OVERFLOW
            ));
        }
        try
        {
            std::sort(
                impl_->data.begin(),
                impl_->data.end(),
                [](const Impl::PendingData& left,
                   const Impl::PendingData& right) noexcept
                {
                    return SimulationDataSchemaIdLess{}(
                        left.schema,
                        right.schema
                    );
                }
            );

            std::size_t total_payload{};
            for (std::size_t index{}; index < impl_->data.size(); ++index)
            {
                const auto& current = impl_->data[index];
                if (index != 0U)
                {
                    const auto& previous = impl_->data[index - 1U];
                    if (previous.schema.hash == current.schema.hash &&
                        previous.schema.name != current.schema.name)
                    {
                        return lux::cxx::unexpected(failure(
                            ESimulationDescriptionError::SCHEMA_HASH_COLLISION,
                            current.schema
                        ));
                    }
                }
                if (current.payload.size() >
                    std::numeric_limits<std::size_t>::max() - total_payload)
                {
                    return lux::cxx::unexpected(failure(
                        ESimulationDescriptionError::SIZE_OVERFLOW,
                        current.schema
                    ));
                }
                total_payload += current.payload.size();
            }

            SimulationDescription result;
            result.schemas_.reserve(impl_->data.size());
            result.data_.reserve(impl_->data.size());
            result.payload_.reserve(total_payload);
            for (auto& source : impl_->data)
            {
                SimulationDescription::DataRecord record;
                record.schema_ordinal = result.schemas_.size();
                record.version = source.version;
                record.payload_offset = result.payload_.size();
                record.payload_size = source.payload.size();
                result.schemas_.push_back(std::move(source.schema));
                result.data_.push_back(record);
                result.payload_.insert(
                    result.payload_.end(),
                    source.payload.begin(),
                    source.payload.end()
                );
            }
            impl_->data.clear();
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(
                ESimulationDescriptionError::ALLOCATION_FAILURE
            ));
        }
    }
}
