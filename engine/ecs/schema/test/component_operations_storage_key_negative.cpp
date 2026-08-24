#include <lux/engine/ecs/ComponentOperations.hpp>

int main()
{
    lux::ecs::ComponentOperations operations;
    const auto component_operations_storage_key_negative =
        operations.storage_key_;
    (void)component_operations_storage_key_negative;
}
