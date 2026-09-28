"""Use the actual native test compile command; negative cases must fail for deleted EditScope constructors."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    entries = json.loads((args.build / "compile_commands.json").read_text())
    entry = next(x for x in entries if x["file"].replace("\\", "/").endswith("editor/sessions/test/sessions.cpp"))
    output = args.build / "sessions-scope-compile"
    output.mkdir(exist_ok=True)
    prefix = '#include <lux/engine/editor/sessions/SessionState.hpp>\nusing lux::editor::sessions::EditScope;\n'
    cases = {
        "valid": "void inspect(EditScope& scope) { (void)scope; }",
        "copy": "void copy(EditScope& scope) { auto value = scope; }",
        "move": "void move(EditScope& scope) { auto value = static_cast<EditScope&&>(scope); }",
    }
    for name, source in cases.items():
        path = output / f"{name}.cpp"
        path.write_text(prefix + source)
        command = entry["command"]
        original = entry["file"].replace("/", "\\")
        assert original in command or entry["file"] in command, command
        command = command.replace(original, str(path).replace("\\", "/"))
        command = command.replace(entry["file"], str(path).replace("\\", "/"))
        command = re.sub(r'/Fo(?:"[^"]+"|\S+)', f'/Fo"{(output / (name + ".obj")).as_posix()}"', command)
        result = subprocess.run(command, cwd=entry["directory"], capture_output=True, text=True)
        log = result.stdout + result.stderr
        (output / f"{name}.log").write_text(log, encoding="utf-8")
        if name == "valid":
            assert result.returncode == 0, log
        else:
            assert result.returncode != 0 and "C2280" in log and "EditScope::EditScope" in log, log
        print(f"PASS {name}: compiler exit {result.returncode}")


if __name__ == "__main__":
    main()
