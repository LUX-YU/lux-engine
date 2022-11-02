import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

glfw3_description = itools.PackageResouceDescription()
glfw3_description.set_name("glfw3")
glfw3_description.set_available_path_in_resource("glfw-3.3.8")
glfw3_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external",
        "libraries", 
        "glfw-3.3.8.zip"
    )
)
source_code_desc = glfw3_description.to_source_code()

installer = itools.CMakeProjectInstaller(source_code_desc)
installer.add_config_option("-DBUILD_SHARED_LIBS=ON")

installer.enable()
