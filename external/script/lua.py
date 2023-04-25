import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

lua_description = itools.PackageResouceDescription()
lua_description.set_name("lua")
lua_description.set_available_path_in_resource("lua53")
lua_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external", 
        "tools", 
        "lua-5.3.6_Sources.zip"
    )
)
source_code_desc = lua_description.to_source_code()

cmake_script_path = os.path.join(itools.project_root_path, "external", "script", "cmake", "lua.cmake")
lua_cmake_path  = os.path.join(source_code_desc.source_code_path(), "CMakeLists.txt")
# copy script to lua project directory
print("copy {} to {}".format(cmake_script_path, lua_cmake_path))
with open(cmake_script_path, "rb") as cf, open(lua_cmake_path, "wb") as pf:
    pf.write(cf.read())

installer = itools.CMakeProjectInstaller(source_code_desc)
installer.enable()

os.remove(lua_cmake_path)

