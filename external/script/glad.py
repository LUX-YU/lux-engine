import sys
import os
from zipfile import ZipFile

tool_dir_path = sys.argv[1]
sys.path.append(tool_dir_path)

import install_tools as itools

pkg_unpack_dir = os.path.join(itools.source_dir, "glad")

glad_pkg_path = os.path.join(
    itools.project_root_path, 
    "external",
    "libraries", 
    "glad.zip"
)

with ZipFile(glad_pkg_path) as _pack_file:
    _pack_file.extractall(pkg_unpack_dir)