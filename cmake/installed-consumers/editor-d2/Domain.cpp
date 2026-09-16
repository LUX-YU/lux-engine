#include <consumer/Component.ecs_schema.hpp>
#include <consumer/Domain.hpp>

namespace consumer
{
namespace
{
bool reject_decode{};
lux::simulation::ecs::DecodeComponentValueFn original_decode{};
} // namespace

void rejectNextDecode()
{
    reject_decode = true;
}

std::span<const lux::simulation::ecs::ComponentSchema> schemas()
{
    static const auto schemas = []
    {
        const auto generated = lux::simulation::ecs::generated::ConsumerComponentSchemas();
        std::vector<lux::simulation::ecs::ComponentSchema> values(generated.begin(), generated.end());
        original_decode = values.front().decode_value;
        values.front().decode_value =
            +[](std::uint32_t version, std::span<const std::byte> bytes,
                lux::simulation::ecs::ComponentEntityResolver resolver, std::shared_ptr<const void> code)
            -> lux::cxx::expected<lux::simulation::ecs::DecodedComponent, lux::simulation::ecs::ComponentDecodeFailure>
        {
            if (std::exchange(reject_decode, false))
            {
                return lux::cxx::unexpected(lux::simulation::ecs::ComponentDecodeFailure{
                    lux::simulation::ecs::EComponentDecodeError::UNSUPPORTED_VERSION, 23});
            }
            return original_decode(version, bytes, resolver, std::move(code));
        };
        return values;
    }();
    return schemas;
}
} // namespace consumer
