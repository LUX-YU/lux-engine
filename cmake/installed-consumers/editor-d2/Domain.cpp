#include <consumer/Component.ecs_schema.hpp>
#include <consumer/Domain.hpp>

namespace consumer
{
namespace
{
bool reject_decode{};
lux::simulation::ecs::DecodeEmplaceComponentFn original_decode{};
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
        original_decode = values.front().decode_emplace;
        values.front().decode_emplace =
            +[](lux::simulation::ecs::Registry &registry, const lux::simulation::ecs::WorldEntityMap &identities,
                lux::simulation::ecs::Entity entity, std::uint32_t version, std::span<const std::byte> bytes) noexcept
            -> lux::cxx::expected<void, lux::simulation::ecs::ComponentDecodeFailure>
        {
            if (std::exchange(reject_decode, false))
            {
                return lux::cxx::unexpected(lux::simulation::ecs::ComponentDecodeFailure{
                    lux::simulation::ecs::EComponentDecodeError::UNSUPPORTED_VERSION, 23});
            }
            return original_decode(registry, identities, entity, version, bytes);
        };
        return values;
    }();
    return schemas;
}
} // namespace consumer
