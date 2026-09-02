#include <lux/engine/process/TaskScope.hpp>

#include <stdexec/execution.hpp>

int main()
{
    lux::process::TaskScope scope;
    const auto started = scope.start(stdexec::just(42));
    return started ? 0 : 1;
}
