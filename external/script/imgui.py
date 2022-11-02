import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

imgui_description = itools.PackageResouceDescription()
imgui_description.set_name("imgui")
imgui_description.set_available_path_in_resource("imgui-1.88")
imgui_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external",
        "libraries", 
        "imgui-1.88.zip"
    )
)
source_code_desc = imgui_description.to_source_code()

cmake_script_path = os.path.join(itools.project_root_path, "external", "script", "cmake", "imgui.cmake")
imgui_cmake_path  = os.path.join(source_code_desc.source_code_path(), "CMakeLists.txt")
# copy script to imgui project directory
print("copy {} to {}".format(cmake_script_path, imgui_cmake_path))
with open(cmake_script_path, "rb") as cf, open(imgui_cmake_path, "wb") as pf:
    pf.write(cf.read())

installer = itools.CMakeProjectInstaller(source_code_desc)
installer.enable()

os.remove(imgui_cmake_path)