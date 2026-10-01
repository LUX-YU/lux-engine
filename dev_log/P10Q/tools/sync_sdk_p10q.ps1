# Run only after all old-SDK measurements have ended. Never remove another project's prefix content.
$ErrorActionPreference = 'Stop'
if (Get-Process -Name bq1,measure,hierarchy_alloc,candidates,records,canvas_ids -ErrorAction SilentlyContinue) {
    throw 'Measurements are still using an installed SDK.'
}
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$implementation = (git -C $sourceRoot rev-parse HEAD).Trim()
$clusterRoot = Split-Path $sourceRoot
$buildRoot = Join-Path $clusterRoot ('build/RelWithDebInfo/p10q-'+$implementation.Substring(0,12))
$prefix = [IO.Path]::GetFullPath((Join-Path $clusterRoot 'install/RelWithDebInfo'))
$output = Join-Path $PSScriptRoot ('P10Q-final/'+$implementation+'/sdk-sync')
New-Item -ItemType Directory -Path $output -Force | Out-Null
$obsolete = @(
 'include/lux/engine/editor/io/SaveExecution.hpp',
 'include/lux/engine/editor/io/ProjectArtifactStore.hpp',
 'include/lux/engine/editor/project/ProjectCatalogAccess.hpp',
 'include/lux/engine/editor/tasks/TaskQueryPort.hpp',
 'include/lux/engine/editor/material/MaterialSessionAccess.hpp',
 'include/lux/engine/editor/flowforge/FlowSessionAccess.hpp',
 'include/lux/engine/editor/scene/SceneSessionAccess.hpp',
 'include/lux/engine/editor/scene/SceneElement.hpp',
 'include/lux/engine/editor/scene/CameraNavigation.hpp',
 'include/lux/engine/editor/scene/ViewportPresentation.hpp',
 'lib/project_io.lib'
)
foreach($name in @('app','project','scene','material','flowforge')) {
    $obsolete += 'bin/lux_engine_editor_'+$name+'.dll'
    $obsolete += 'bin/lux_engine_editor_'+$name+'.pdb'
}
$package = [IO.Path]::GetFullPath((Join-Path $prefix 'share/lux-engine-editor-project-io'))
if (Test-Path -LiteralPath $package) {
    foreach($file in Get-ChildItem -LiteralPath $package -File -Recurse) {
        $obsolete += [IO.Path]::GetRelativePath($prefix,$file.FullName)
    }
}
$removals = @()
foreach($relative in $obsolete) {
    $target = [IO.Path]::GetFullPath((Join-Path $prefix $relative))
    if (!$target.StartsWith($prefix+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Target escapes the intended development prefix: '+$target
    }
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $removals += @{path=$relative; sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash}
        Remove-Item -LiteralPath $target
    }
}
# Remove only now-empty directories of this one obsolete package, without recursive deletion.
if (Test-Path -LiteralPath $package) {
    $directories = @(Get-ChildItem -LiteralPath $package -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending)
    foreach($directory in $directories) {
        if (@(Get-ChildItem -LiteralPath $directory.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $directory.FullName }
    }
    if (@(Get-ChildItem -LiteralPath $package -Force).Count -eq 0) { Remove-Item -LiteralPath $package }
}
$removals | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $output 'explicit-removals.json')
cmake --install $buildRoot --prefix $prefix *> (Join-Path $output 'install.log')
$result = $LASTEXITCODE
@{implementation_sha=$implementation; prefix=$prefix; exit_code=$result; modules_public_headers_changed=$false} |
 ConvertTo-Json | Set-Content (Join-Path $output 'result.json')
if ($result -ne 0) { throw 'Development SDK install failed; log preserved.' }
foreach($relative in $obsolete) {
    if (Test-Path -LiteralPath (Join-Path $prefix $relative)) { throw 'Obsolete SDK interface remains: '+$relative }
}
Write-Output 'Development SDK synchronized; only enumerated obsolete files removed. No modules public-header changes.'
