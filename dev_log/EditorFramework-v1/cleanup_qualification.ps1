$ErrorActionPreference = 'Stop'
$canonical = 'E:/SyncForder/CodeRepos/lux-engine'
$targets = @(
    'E:/SyncForder/CodeRepos/qualification/framework-v1-34a8a25f',
    'E:/SyncForder/CodeRepos/qualification/framework-v1-1d40785b'
)
foreach ($requested in $targets) {
    $resolved = (Resolve-Path -LiteralPath $requested).Path
    $expected = [IO.Path]::GetFullPath($requested)
    if ($resolved -ne $expected -or -not $resolved.StartsWith('E:\SyncForder\CodeRepos\qualification\framework-v1-')) {
        throw "Unexpected cleanup target: $resolved"
    }
    $status = git -C $resolved status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0 -or $status) { throw "Qualification checkout is not clean: $resolved" }
    $commit = git -C $resolved rev-parse HEAD
    git -C $canonical merge-base --is-ancestor $commit HEAD
    if ($LASTEXITCODE -ne 0) { throw "Unpreserved qualification commit: $commit" }
    Write-Output "Verified temporary checkout: $resolved, HEAD=$commit"
    Remove-Item -LiteralPath $resolved -Recurse -Force
    Write-Output "Removed temporary checkout: $resolved"
}
