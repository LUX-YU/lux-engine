import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

ninja_description = itools.PackageResouceDescription()
ninja_description.set_name("ninja")
ninja_description.set_available_path_in_resource("ninja-1.11.0")
ninja_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external", 
        "tools", 
        "ninja-1.11.0.zip"
    )
)
source_code_desc = ninja_description.to_source_code()

installer = itools.CMakeProjectInstaller(source_code_desc)

installer.enable()
