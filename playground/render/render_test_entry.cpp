#include <iostream>
#include <lux-engine/platform/cxx/SubProgram.hpp>

int main(int argc, char* argv[])
{
    using namespace ::lux::engine::platform;

    return SubProgramRegister::invokeSubProgram("light_caster", argc, argv);
}
