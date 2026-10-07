import subprocess
import sys

scenarios = (
    "apply-in-draw", "destroy-in-draw", "apply-in-event", "destroy-in-event",
    "apply-in-update", "recursive-apply", "destroy-in-apply", "apply-in-signal", "modal-wrong-thread",
)
for scenario in scenarios:
    result = subprocess.run([sys.argv[1], scenario], capture_output=True, text=True, timeout=15)
    assert "UI contract probe entered" in result.stdout, (scenario, result.returncode, result.stderr)
    if "destroy" in scenario or scenario == "modal-wrong-thread":
        assert result.returncode not in (0, 1), (scenario, result.returncode, result.stdout, result.stderr)
    else:
        assert result.returncode == 0, (scenario, result.returncode, result.stdout, result.stderr)
    print(f"{scenario}: contract rejection confirmed ({result.returncode})", flush=True)
