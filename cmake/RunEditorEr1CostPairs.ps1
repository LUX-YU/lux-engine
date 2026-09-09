param(
    [Parameter(Mandatory = $true)][string]$QualificationRoot,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$SeedPak,
    [Parameter(Mandatory = $true)][string]$CxxPrefix,
    [Parameter(Mandatory = $true)][string]$VcpkgRoot
)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Cost evidence requires a new directory' }
$qualification = Get-Content -LiteralPath "$QualificationRoot/qualification.json" -Raw | ConvertFrom-Json
if ($qualification.configuration -ne 'RelWithDebInfo' -or $qualification.diagnostics) {
    throw 'Costs require qualified normal RelWithDebInfo artifacts'
}
$source = $qualification.source
if ((& git -C $source rev-parse HEAD).Trim() -ne $qualification.source_commit -or
    (& git -C $source status --porcelain)) { throw 'Qualified source identity changed' }
$build = $qualification.build
$rules = Get-Content -LiteralPath "$build/build.ninja" -Raw
if ($rules -match 'LUX_SV1_DIAGNOSTICS=1|EditorAllocationDiagnostics\.cpp\.obj:|ClientAllocationDiagnostics\.cpp\.obj:|fsanitize=address') {
    throw 'Diagnostic instrumentation is forbidden in performance artifacts'
}
foreach ($artifact in $qualification.artifacts) {
    $file = Join-Path "$build/bin" $artifact.name
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $artifact.sha256) {
        throw "Artifact changed after qualification: $file"
    }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$cleanPath = ($env:PATH -split ';' | Where-Object {
    $_ -and $_ -notmatch '(?i)lux-er1|lux-sv1|install[/\\]RelWithDebInfo|build[/\\]RelWithDebInfo'
}) -join ';'
$env:PATH = "$build/bin;$CxxPrefix/bin;$VcpkgRoot/installed/x64-windows/bin;$cleanPath"
$pairs = @()
for ($pair = 1; $pair -le 5; ++$pair) {
    # Alternate order to avoid assigning every later run to the same implementation.
    $order = if ($pair % 2) { @('legacy', 'scene') } else { @('scene', 'legacy') }
    foreach ($stack in $order) {
        $sample = "$OutputDirectory/$pair-$stack.json"
        $executable = if ($stack -eq 'legacy') { 'editor_workbench_cost.exe' } else { 'editor_scene_cost.exe' }
        & "$build/bin/$executable" $SeedPak $sample *> "$OutputDirectory/$pair-$stack.log"
        if ($LASTEXITCODE -ne 0) { throw "Cost process $pair/$stack failed; sample not qualified" }
    }
    $old = Get-Content -LiteralPath "$OutputDirectory/$pair-legacy.json" -Raw | ConvertFrom-Json
    $new = Get-Content -LiteralPath "$OutputDirectory/$pair-scene.json" -Raw | ConvertFrom-Json
    if ($old.wait_boundary -ne 'per-frame-recorded' -or $new.wait_boundary -ne 'per-frame-recorded' -or
        $old.work_polls -ne 500 -or $new.work_polls -ne 500) { throw "Pair $pair has invalid measurement boundaries" }
    foreach ($field in @('configuration', 'warmup', 'iterations', 'view_count', 'cadence_seconds',
                         'completed_scene_frames', 'verification_frames', 'wait_boundary', 'close_completed', 'checksum')) {
        if ($old.$field -ne $new.$field) { throw "Pair $pair has unequal $field" }
    }
    if (($old.window -join ',') -ne ($new.window -join ',') -or
        ($old.extent -join ',') -ne ($new.extent -join ',') -or $new.completed_scene_frames -ne 500) {
        throw "Pair $pair does not match dimensions and actual completion amount"
    }
    $pairs += [pscustomobject]@{
        pair = $pair; checksum = $new.checksum; frames = $new.completed_scene_frames
        legacy_work_wall = $old.work_wall_seconds; candidate_work_wall = $new.work_wall_seconds
        legacy_wait = $old.explicit_wait_seconds; candidate_wait = $new.explicit_wait_seconds
        legacy_owner_cpu = $old.owner_cpu_seconds; candidate_owner_cpu = $new.owner_cpu_seconds
        legacy_process_cpu = $old.process_cpu_seconds; candidate_process_cpu = $new.process_cpu_seconds
        legacy_work_cycles = $old.owner_work_cycles; candidate_work_cycles = $new.owner_work_cycles
        legacy_wait_cycles = $old.owner_wait_cycles; candidate_wait_cycles = $new.owner_wait_cycles
        legacy_work_polls = $old.work_polls; candidate_work_polls = $new.work_polls
        legacy_wait_polls = $old.wait_polls; candidate_wait_polls = $new.wait_polls
        legacy_close_wall = $old.close_wall_seconds; candidate_close_wall = $new.close_wall_seconds
    }
    $pairs | Export-Csv -LiteralPath "$OutputDirectory/pairs.csv" -NoTypeInformation
    Write-Output "Pair $pair PASS: 500 completed scene frames, matched actual target pixels"
}
[ordered]@{
    source_commit = $qualification.source_commit
    seed_sha256 = (Get-FileHash -LiteralPath $SeedPak -Algorithm SHA256).Hash
    qualification_manifest_sha256 = (Get-FileHash -LiteralPath "$QualificationRoot/qualification.json" -Algorithm SHA256).Hash
    artifacts = $qualification.artifacts
    pairs = $pairs
    scope = 'Static production pipelines driven by non-installed consumers; one hidden window, one View, three resources; full Application event-loop cost is not measured'
    accounting = 'Work wall includes internal blocking and preemption; explicit waits include pacing, progress polling and final readback with eight separately counted verification frames; owner work/wait cycles use QueryThreadCycleTime without conversion to time. CPU seconds use OS thread/process time, not GPU duration or allocation attribution'
} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$OutputDirectory/identity.json" -Encoding utf8
