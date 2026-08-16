<#
.SYNOPSIS
  Render-graph compile acceptance gate — confirms, on a REAL project, that the
  render graph actually compiles and binds correctly.

.DESCRIPTION
  Why this is needed and `ctest -L gpu` isn't enough:
    In practice, the graph that `scene_cycle_stress_test` compiles comes out
    with `Pass count = 0`, and `canvas2d_stress_test` only reaches 1. Neither
    test exercises the real path of MULTI-PIPELINE AGGREGATION -> GRAPH
    COMPILE -> RECORD-TIME DESCRIPTOR BINDING. On 2026-07-19 we had a concrete
    case of this: the descriptor-layout allocator at graph-compile time made
    every real scene's graph FAIL TO COMPILE ENTIRELY (compiled count = 0),
    while those three GPU tests still passed 100%. What they prove is
    "repeated assembly/teardown doesn't crash", not "the real graph is
    correct".

  Verdict criteria (short-circuited in order of severity):
    process crash > graph compile failure > validation error > layout-plan
    warning > zero successful compiles.
    The last one is INSUFFICIENT COVERAGE, not a pass — if not a single graph
    ever compiled, "zero errors" is an empty verification.

  Note: whether the scene view is active DOES NOT AFFECT this criterion.
  Graph compilation happens at assembly time; even if not a single frame got
  drawn, "can this graph compile" is still a meaningful question. (On
  2026-07-18 we once used this reasoning to wrongly write off the whole
  editor acceptance check as invalid, which let the regression above slip
  through.)

.PARAMETER BuildDir
  Root of the build tree; defaults to RelWithDebInfo (the same tree the
  editor's launch.json uses).

.PARAMETER Project
  Path to the .luxproject file.

.PARAMETER Seconds
  How long to keep the editor alive. 45 seconds is enough to load the project
  and compile the graphs for every scene.

.EXAMPLE
  pwsh modules/function/render/cmake/verify_graph_compile.ps1
#>
param(
    [string]$BuildDir = "E:\SyncForder\CodeRepos\build\RelWithDebInfo\lux-engine",
    [string]$Project  = "E:\SyncForder\CodeRepos\lux-engine-test\demo\demo.luxproject",
    [string]$InstallBin = "E:\SyncForder\CodeRepos\install\RelWithDebInfo\bin",
    [string]$VcpkgBin = "D:\Development\vcpkg\installed\x64-windows\bin",
    [int]$Seconds = 45
)

$ErrorActionPreference = 'Stop'

$editor = Join-Path $BuildDir "bin\lux_editor.exe"
if (-not (Test-Path $editor)) { Write-Error "找不到编辑器:$editor"; exit 3 }
if (-not (Test-Path $Project)) { Write-Error "找不到项目:$Project"; exit 3 }

$log = Join-Path ([System.IO.Path]::GetTempPath()) "lux_graph_verify_err.txt"
$out = Join-Path ([System.IO.Path]::GetTempPath()) "lux_graph_verify_out.txt"

$env:PATH = "$InstallBin;$VcpkgBin;$env:PATH"
$env:LUX_DUMP_RG = "1"          # makes every successful compile print its Plan section (see RGDebugPrint)
# 高亮条件链的"运行"分支覆盖:第 300 帧自动选中场景第一个 mesh 实体。
# 无此覆盖时链恒跳过,选中路径的回归对门禁不可见(2026-07 高亮整链静默
# 失效正是藏在这个盲区里——审查文档 5.5 教训三)。
$env:LUX_EDITOR_AUTO_SELECT_FRAME = "300"

# cwd must be bin — matching the .vscode/launch.json "Debug lux_editor with
# test project" config; the editor resolves its docking-layout ini relative to cwd.
$p = Start-Process -FilePath $editor `
    -ArgumentList '--project', $Project, '--vk-validation' `
    -WorkingDirectory (Join-Path $BuildDir "bin") -PassThru `
    -RedirectStandardError $log -RedirectStandardOutput $out

Start-Sleep -Seconds $Seconds
$exited   = $p.HasExited
$exitCode = if ($exited) { $p.ExitCode } else { 0 }
if (-not $exited) { Stop-Process -Id $p.Id -Force }

$lines = Get-Content $log -ErrorAction SilentlyContinue

# WARNING: when the pattern contains '|', do NOT add -SimpleMatch — that would
# treat the whole string as a literal, so it would never match and would
# silently produce a false CLEAN result. Already hit this bug once; don't
# reintroduce it.
$failed   = $lines | Select-String -Pattern "Graph compile failed"
$compiled = $lines | Select-String -Pattern "graph \(re\)compiled"
$verr     = $lines | Select-String -Pattern "Validation Error|VUID-"
$planWarn = $lines | Select-String -Pattern "布局计划告警"
$passCounts = $lines | Select-String -Pattern "^Pass count\s+:\s+(\d+)" |
              ForEach-Object { [int]$_.Matches[0].Groups[1].Value }
$maxPass = if ($passCounts) { ($passCounts | Measure-Object -Maximum).Maximum } else { 0 }

""
"process           : $(if ($exited) { "exited code=$exitCode" } else { "alive ${Seconds}s" })"
"graphs compiled   : $($compiled.Count)"
"compile failures  : $($failed.Count)"
"validation errors : $($verr.Count)"
"plan warnings     : $($planWarn.Count)"
"max pass count    : $maxPass"

if ($failed)   { ""; "=== compile failures (distinct) ==="; $failed.Line   | Sort-Object -Unique | Select-Object -First 5 }
if ($planWarn) { ""; "=== plan warnings (distinct) ===";    $planWarn.Line | Sort-Object -Unique | Select-Object -First 5 }
if ($verr)     { ""; "=== validation (distinct, first 3) ==="; $verr.Line | Sort-Object -Unique | Select-Object -First 3 }

""
if ($exited -and $exitCode -ne 0) { "VERDICT: FAIL (editor crashed, code=$exitCode)"; exit 1 }
elseif ($failed.Count -gt 0)      { "VERDICT: FAIL ($($failed.Count) graph compile failures)"; exit 1 }
elseif ($verr.Count -gt 0)        { "VERDICT: FAIL ($($verr.Count) validation errors)"; exit 1 }
elseif ($planWarn.Count -gt 0)    { "VERDICT: FAIL ($($planWarn.Count) layout plan warnings)"; exit 1 }
elseif ($compiled.Count -eq 0)    { "VERDICT: INCONCLUSIVE (no graph compiled — nothing was exercised)"; exit 2 }
else { "VERDICT: PASS ($($compiled.Count) graphs compiled, max pass count $maxPass)"; exit 0 }
