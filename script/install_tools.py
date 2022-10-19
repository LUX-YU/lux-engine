import os
import subprocess
from zipfile import ZipFile
import multiprocessing
from abc import abstractclassmethod

project_root_path = os.path.join(os.path.dirname(__file__), os.pardir)
build_path        = os.path.join(project_root_path, "prebuild")

source_dir        = os.path.join(build_path, "source")
install_path      = os.path.join(build_path, "install")
config_path       = os.path.join(build_path, "config")

class SourceCodeDescription:
    def __init__(self, name, source_code_dir) -> None:
        self.__name = name
        self.__source_code_dir = source_code_dir

    def source_code_path(self):
        return self.__source_code_dir

    def name(self):
        return self.__name
    
class ResourceDescription:
    def __init__(self) -> None:
        self.available_path = ""

    @abstractclassmethod
    def to_source_code(self) -> SourceCodeDescription:
        ...

    def set_name(self, name : str):
        self.name = name

    def set_available_path_in_resource(self, path : str):
        self.available_path = path

class GitResourceDescription(ResourceDescription):
    def __init__(self) -> None:
        super().__init__()

    def __git_download(self, clone_path) -> bool:
        clone_cmd = [
            "git", 
            "clone",
            "--depth=1",
            "-b", self.branch,  # specific branch
            self.uri,           # uri
            clone_path          # clone path
        ]
        print("Execute command:", clone_cmd)
        return subprocess.run(clone_cmd).returncode == 0

    def __git_checkout(self, path, version):
        last_path = os.getcwd()
        os.chdir(path)
        if os.getcwd() != path:
            return False
        checkout_cmd = [
            "git",
            "checkout",
            version
        ]
        print("Execute command:", checkout_cmd)
        if subprocess.run(checkout_cmd).returncode == 0:
            os.chdir(last_path)
            return True

    def set_git_repo_uri(self, uri : str):
        self.uri = uri

    def set_branch(self, branch : str):
        self.branch = branch

    def to_source_code(self) -> SourceCodeDescription:
        global source_dir
        clone_path = os.path.join(source_dir, self.name)
        if not os.path.exists(clone_path):
            if not self.__git_download(clone_path):
                return None
        else:
            self.__git_checkout(clone_path, self.branch)
            
        return SourceCodeDescription(self.name, os.path.join(clone_path, self.available_path))

class PackageResouceDescription(ResourceDescription):
    def __init__(self) -> None:
        super().__init__()

    def __unpack_package(self, pkg_unpack_dir):

        with ZipFile(self.package_path) as _pack_file:
            _pack_file.extractall(pkg_unpack_dir)

    def set_package_path(self, path : str):
        self.package_path = path

    def to_source_code(self) -> SourceCodeDescription:
        global source_dir
        pkg_unpack_dir = os.path.join(source_dir, self.name)

        self.__unpack_package(pkg_unpack_dir)

        return SourceCodeDescription(self.name, os.path.join(pkg_unpack_dir, self.available_path))


class Installer:
    def __init__(self, desc : SourceCodeDescription) -> None:
        self.desc = desc

    def description(self):
        return self.desc

    def enable(self) -> bool:
        cmds = [
            "config_command",
            "build_command",
            "install_command"
        ]

        for cmd in cmds:
            if not hasattr(self, cmd):
                print("Don't has attribute", cmd)
                return False
            _cmd = getattr(self, cmd)
            if _cmd == "":
                continue
            print("Execute command:", _cmd)
            if subprocess.run(_cmd).returncode != 0:
                return False
        
        return True

class CustomerInstaller(Installer):
    def __init__(self, desc : SourceCodeDescription) -> None:
        super(CMakeProjectInstaller, self).__init__(desc)

    def set_config_command(self, cmd : list):
        self.config_command(cmd)

    def set_build_command(self, cmd : list):
        self.build_command = cmd

    def set_install_command(self, cmd : list):
        self.install_command = cmd

class CMakeProjectInstaller(Installer):
    def __init__(self, desc : SourceCodeDescription) -> None:
        super(CMakeProjectInstaller, self).__init__(desc)
        self.build_type = "RELEASE"
        self.parallel_number = multiprocessing.cpu_count()
        self.extend_cfg_options = []
        self.build_command = ""
    
    def set_build_type(self, type: str):
        self.build_type = type

    def set_parallel(self, number : int):
        self.parallel_number = number

    def use_ccache(self, enable : bool):
        self.use_ccache = enable

    def add_config_option(self, cmd : str):
        self.extend_cfg_options.append(cmd)

    def __gen_config_cmd(self):
        global config_path
        global install_path
        _build_path = "-B{}".format(os.path.join(config_path, self.desc.name()))
        _build_type = "-DCMAKE_BUILD_TYPE={}".format(self.build_type)
        _install_path = "-DCMAKE_INSTALL_PREFIX={}".format(install_path)

        self.config_command = ["cmake"]

        for option in self.extend_cfg_options:
            self.config_command.append(option)

        self.config_command.append(_build_path)
        self.config_command.append(self.desc.source_code_path())
        self.config_command.append(_build_type)
        self.config_command.append(_install_path)

    def __gen_install_cmd(self):
        global config_path
        self.install_command = [
            "cmake",
            "--build",      os.path.join(config_path, self.desc.name()),
            "--parallel",   str(self.parallel_number),
            "--target",     "install",
            "--config",     self.build_type
        ]

    def enable(self) -> bool:
        self.__gen_config_cmd()
        self.__gen_install_cmd()
        return super().enable()
