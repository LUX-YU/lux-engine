#include <lux/engine/editor/material/MaterialCompilation.hpp>
#include <lux/engine/editor/flowforge/FlowCompilationService.hpp>
#include <utility>

#if defined(OWNER_material)
using Operation = lux::editor::material::MaterialCompileOperation;
#elif defined(OWNER_flow)
using Operation = lux::editor::flowforge::FlowCompileOperation;
#else
#error Select the operation under test
#endif

void rejected(Operation& destination, Operation& source)
{
#if defined(ACTION_copy_construct)
    Operation duplicate(source);
#elif defined(ACTION_copy_assign)
    destination = source;
#elif defined(ACTION_move_construct)
    Operation duplicate(std::move(source));
#elif defined(ACTION_move_assign)
    destination = std::move(source);
#else
#error Select the special member under test
#endif
}
