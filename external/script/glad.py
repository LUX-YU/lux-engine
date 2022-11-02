import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

glad_description = itools.PackageResouceDescription()
glad_description.set_name("glad")
glad_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external",
        "libraries", 
        "glad.zip"
    )
)
source_code_desc = glad_description.to_source_code()

cmake_script_path = os.path.join(itools.project_root_path, "external", "script", "cmake", "glad.cmake")
glad_cmake_path  = os.path.join(source_code_desc.source_code_path(), "CMakeLists.txt")
# copy script to glad project directory
print("copy {} to {}".format(cmake_script_path, glad_cmake_path))
with open(cmake_script_path, "rb") as cf, open(glad_cmake_path, "wb") as pf:
    pf.write(cf.read())

installer = itools.CMakeProjectInstaller(source_code_desc)
installer.enable()

os.remove(glad_cmake_path)