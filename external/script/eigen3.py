from platform import platform
import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

eigen3_description = itools.PackageResouceDescription()
eigen3_description.set_name("eigen3")
eigen3_description.set_available_path_in_resource("eigen-3.4.0")
eigen3_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external",
        "libraries", 
        "eigen-3.4.0.zip"
    )
)
source_code_desc = eigen3_description.to_source_code()

installer = itools.CMakeProjectInstaller(source_code_desc)
installer.add_config_option("-DEIGEN_TEST_CXX11=ON")
if sys.platform == "win32":
    installer.add_config_option("-DEIGEN_TEST_CUSTOM_CXX_FLAGS=\"/std:cxx11\"")

installer.enable()
