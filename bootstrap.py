import os
import subprocess
import argparse

project_root_dir            = os.path.dirname(__file__)
external_install_script_dir = os.path.join(project_root_dir, "external", "script")
installer_class_dir         = os.path.join(project_root_dir, "script")

external_tools_list = {
    "ninja":    os.path.join(external_install_script_dir,   "ninja.py"),
    "llvm":     os.path.join(external_install_script_dir,   "llvm.py"),
}

external_library_list = {
    "glad":     os.path.join(external_install_script_dir,   "glad.py"),
    "glfw":     os.path.join(external_install_script_dir,   "glfw.py"),
    "imgui":    os.path.join(external_install_script_dir,   "imgui.py"),
    "eigen3":   os.path.join(external_install_script_dir,   "eigen3.py"),
    "stb":      os.path.join(external_install_script_dir,   "stb.py"),
    "freetype": os.path.join(external_install_script_dir,   "freetype.py"),
    "assimp":   os.path.join(external_install_script_dir,   "assimp.py"),
    # "boost":   os.path.join(external_install_script_dir,    "boost.py"),
    "stduuid":   os.path.join(external_install_script_dir,  "stduuid.py")
}

def execute_cmd_list(cmd_dict : dict, enable_list = []):
    python_executable = "python"
    global installer_class_dir

    for name in enable_list:
        script_path = cmd_dict[name]
        script_enable_cmd = [
            python_executable,
            script_path,
            installer_class_dir
        ]
        print("Executing command:", str(script_enable_cmd))
        ret = subprocess.run(script_enable_cmd)
        if ret.returncode != 0:
            print("Error Occurred when execute script:", script_path)
            exit(255)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-t", "--tools",    nargs='*')
    parser.add_argument("--tools-all")
    parser.add_argument("-l", "--libraries",nargs='*')
    parser.add_argument("--libraries-all") 
    parser.add_argument("-d", "--debug")

    args = parser.parse_args()
    input_tools = []
    input_libraries = []

    print(args.tools)

    if args.tools:
        input_tools += args.tools
    if args.libraries:
        input_libraries += args.libraries

    enable_tools = []
    enable_libraries = []

    if "all" in input_tools:
        if  len(input_tools) > 1:
            print("Warning: option `all` in tools set detected, but there are other option exist.")
        enable_tools = list(external_tools_list.keys())
    else:
        for tool in input_tools:
            if tool not in external_tools_list.keys() and tool != "all":
                print("tools:", tool, " is not allowed")
                exit(-1)
            else:
                enable_tools.append(tool)
    if "all" in input_libraries:
        if len(input_libraries) > 1:
            print("Warning: option `all` in libraries set detected, but there are other option exist.")
        enable_libraries = list(external_library_list.keys())
    else:
        for library in input_libraries:
            if library not in external_library_list.keys() and library != "all":
                print("libraries:", library, " is not allowed")
                exit(-1)
            else:
                enable_libraries.append(library)

    execute_cmd_list(external_tools_list, enable_tools)
    execute_cmd_list(external_library_list, enable_libraries)
