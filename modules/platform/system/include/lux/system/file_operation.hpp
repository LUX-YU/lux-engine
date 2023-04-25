#pragma once
#include "file_system.hpp"
#include <string>
#include <fstream>
#include <sstream>

namespace lux::system
{
    // function for debug, don't use this function against big file
    static inline std::string loadAllFrom(const filesystem::path& path)
    {
        std::ifstream t(path);
        std::stringstream ss;
        if(!t.eof())
        {
            ss << t.rdbuf();
        }
        t.close();
        return ss.str();
    }
}
