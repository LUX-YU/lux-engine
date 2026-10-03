#include <a_fields.inspector.generated.hpp>
#include <b_fields.inspector.generated.hpp>
#include <cassert>
int main()
{
    const auto a = lux::editor::scene::generated::a_fieldsBindings();
    const auto b = lux::editor::scene::generated::b_fieldsBindings();
    const auto run = lux::editor::scene::run_generated::a_fieldsBindings();
    assert(a.size() == 1 && b.size() == 1 && run.size() == 1);
    assert(a.front().create && b.front().create && run.front().create);
}
