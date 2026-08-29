#include <lux/engine/simulation/ecs/TransformSchema.hpp>

int main()
{
    const auto schemas = lux::simulation::ecs::transformComponentSchemas();
    for (const auto& schema : schemas)
    {
        if (schema.id.name == "lux.ecs.Transform3D")
            return schema.decode_emplace != nullptr ? 0 : 1;
    }
    return 2;
}
