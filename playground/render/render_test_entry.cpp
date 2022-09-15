#include <iostream>
#include <lux-engine/platform/cxx/SubProgram.hpp>

int main(int argc, char* argv[])
{
    using namespace ::lux::engine::platform;

    return SubProgramRegister::invokeSubProgram("basic_lighting", argc, argv);
}
