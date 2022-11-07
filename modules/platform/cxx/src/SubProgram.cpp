#include "lux-engine/platform/cxx/SubProgram.hpp"
#include <unordered_map>

namespace lux::engine::platform
{
    using FunctionMap = std::unordered_map<std::string, SubProgramFunc>;

    static inline FunctionMap& getMap()
    {
        static FunctionMap map;
        return map;
    }

#define func_map getMap()

    void SubProgramRegister::registProgram(const std::string& name, const SubProgramFunc& func)
    {
        if(!func_map.count(name))
        {
            func_map[name] = func;
        }
    }

    bool SubProgramRegister::hasSubProgram(const std::string& name)
    {
        return func_map.count(name);
    }

    std::vector<std::string> SubProgramRegister::listSubProgram()
    {
        std::vector<std::string> list;
        for(auto& [key, func] : func_map)
        {
            list.push_back(key);
        }
        return list;
    }

    int SubProgramRegister::invokeSubProgram(const std::string& name, int argc, char* argv[])
    {
        if(func_map.count(name)) return func_map[name](argc, argv);
        return -255;
    }
}
