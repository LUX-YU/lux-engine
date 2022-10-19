import sys
import os

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

assimp_description = itools.GitResourceDescription()
assimp_description.set_name("assimp")
assimp_description.set_branch("v5.2.5")
assimp_description.set_git_repo_uri("https://github.com/assimp/assimp.git")
source_code_desc = assimp_description.to_source_code()

installer = itools.CMakeProjectInstaller(source_code_desc)
installer.enable()
