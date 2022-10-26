import sys
from sys import platform
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

boost_description = itools.GitResourceDescription()
boost_description.set_name("boost")
boost_description.set_branch("boost-1.80.0")
boost_description.set_git_repo_uri("https://github.com/boostorg/boost.git")
boost_description.init_submodule(True)
source_code_desc = boost_description.to_source_code()

installer = itools.CustomerInstaller(source_code_desc)
if platform == "win32":
    installer.set_config_command(["bootstrap.bat"])
    installer.set_build_command(["b2.exe", "--prefix={}".format(itools.install_path)])
elif platform == "linux":
    installer.set_config_command(["./bootstrap.sh"])
    installer.set_build_command(["./b2", "--prefix={}".format(itools.install_path)])

installer.enable()
