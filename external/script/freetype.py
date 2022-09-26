from platform import platform
import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

freetype_description = itools.PackageResouceDescription()
freetype_description.set_name("freetype")
freetype_description.set_available_path_in_resource("freetype-2.12.1")
freetype_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external",
        "libraries",
        "ft2121.zip"
    )
)

source_code_desc = freetype_description.to_source_code()
installer = itools.CMakeProjectInstaller(source_code_desc)
installer.enable()
