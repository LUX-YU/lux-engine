import sys

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

llvm_description = itools.GitResourceDescription()
llvm_description.set_name("llvm")
llvm_description.set_available_path_in_resource("llvm")
llvm_description.set_branch("llvmorg-14.0.6")
llvm_description.set_git_repo_uri("https://github.com/llvm/llvm-project.git")
source_code_desc = llvm_description.to_source_code()

installer = itools.CMakeProjectInstaller(source_code_desc)
installer.add_config_option("-DLLVM_ENABLE_PROJECTS=clang")

installer.enable()
