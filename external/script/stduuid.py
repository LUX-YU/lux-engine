from platform import platform
import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

stduuid_description = itools.PackageResouceDescription()
stduuid_description.set_name("stduuid")
stduuid_description.set_available_path_in_resource("stduuid-1.2.2")
stduuid_description.set_package_path(
    os.path.join(
        itools.project_root_path, 
        "external",
        "libraries",
        "stduuid-1.2.2.zip"
    )
)

source_code_desc = stduuid_description.to_source_code()
installer = itools.CMakeProjectInstaller(source_code_desc)
installer.add_config_option("-DUUID_BUILD_TESTS=OFF")
installer.add_config_option("-DUUID_SYSTEM_GENERATOR=ON")
installer.add_config_option("-DUUID_TIME_GENERATOR=ON")
installer.enable()
