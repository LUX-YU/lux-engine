<#
.SYNOPSIS
  Build the bring-up APK — WITHOUT Gradle.

.DESCRIPTION
  A NativeActivity app with android:hasCode="false" contains no Java and no
  dex, so the APK is just: a compiled manifest, and lib/<abi>/*.so. That is
  reachable with the three tools already in build-tools (aapt2 / zipalign /
  apksigner), which is why bring-up does not stand up a second build system.

  What would change this: per-game package names, plugins contributing manifest
  fragments, or permissions that vary with content — any of those turn the
  manifest into a COMPOSED artifact, and composition is what Gradle is for.
  None of them exist yet, so the manifest is a static file and this is a
  packaging step, not a code generator.

  Native libraries are NOT built here. `cmake --build --preset android-arm64`
  stays the single source of truth for compilation (it owns the vcpkg overlay
  triplet, the NDK chainload, the shared reflection outputs); this script only
  collects what that build produced.

.PARAMETER Install
  adb install -r the signed APK onto the connected device.

.PARAMETER Run
  Launch the activity after installing, and stream its logcat.
#>
[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$Run,
    # 'smoke' = the bring-up gauntlet; 'game' = the NativeActivity→SceneRuntime
    # front end (libluxgame.so) booting the paks packaged under assets/.
    [ValidateSet('smoke','game')]
    [string]$Target      = 'smoke',
    # game target: the cooked paks to place under APK assets/ (game_main
    # extracts them to internalDataPath on first run). GamePak is required
    # for -Target game; EnginePak (builtins) is optional but a scene using
    # builtin meshes/materials will be hollow without it.
    [string]$GamePak     = "",
    [string]$EnginePak   = "",
    # Must be produced by the shared Resource deployment writer. The current
    # Android adapter accepts the canonical packaged names game.luxpak and
    # base.luxpak; extension asset staging is not implemented yet.
    [string]$RuntimeManifest = "",
    [string]$Sdk         = "D:\Development\Android\SDK",
    [string]$BuildTools  = "36.0.0",
    [string]$Platform    = "android-36.1",
    [string]$Abi         = "arm64-v8a",
    [int]   $MinSdk      = 33,   # matches ANDROID_PLATFORM: API 29's libvulkan
                                 # does not export the Vulkan 1.2/1.3 entry points
    [int]   $TargetSdk   = 36
)

$ErrorActionPreference = 'Stop'
$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir   = Join-Path $here 'build'

$aapt2     = Join-Path $Sdk "build-tools\$BuildTools\aapt2.exe"
$zipalign  = Join-Path $Sdk "build-tools\$BuildTools\zipalign.exe"
$apksigner = Join-Path $Sdk "build-tools\$BuildTools\apksigner.bat"
$androidJar= Join-Path $Sdk "platforms\$Platform\android.jar"
$adb       = Join-Path $Sdk "platform-tools\adb.exe"

foreach ($t in @($aapt2, $zipalign, $androidJar)) {
    if (-not (Test-Path $t)) { throw "missing required tool/file: $t" }
}

# Where `cmake --build --preset android-arm64` drops its artifacts.
$soRoots = @(
    "E:\SyncForder\CodeRepos\build\Android\lux-engine\bin",
    "E:\SyncForder\CodeRepos\build\Android\lux-cxx\reflection"
)

if ($Target -eq 'game' -and
    (-not $GamePak -or -not (Test-Path $GamePak) -or
     -not $RuntimeManifest -or -not (Test-Path $RuntimeManifest))) {
    throw "-Target game requires -GamePak and -RuntimeManifest"
}
if ($Target -eq 'game') {
    $runtimeText = Get-Content -LiteralPath $RuntimeManifest -Raw
    if ($runtimeText -notmatch '(?m)^\s*game_pak\s*=\s*"game\.luxpak"\s*$') {
        throw 'Android RuntimeManifest must name game_pak = "game.luxpak"'
    }
    if ($runtimeText -match '(?m)^\s*\[\[extensions\]\]') {
        throw "Android extension asset staging is not implemented; RuntimeManifest must not contain extensions"
    }
    if ($EnginePak -and
        $runtimeText -notmatch '(?m)^\s*base_pak\s*=\s*"base\.luxpak"\s*$') {
        throw 'Android RuntimeManifest must name base_pak = "base.luxpak" when the legacy -EnginePak input is supplied'
    }
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$unsigned = Join-Path $outDir "$Target-unsigned.apk"
$aligned  = Join-Path $outDir "$Target-aligned.apk"
$signed   = Join-Path $outDir "$Target.apk"
foreach ($f in @($unsigned, $aligned, $signed)) {
    if (Test-Path $f) { Remove-Item $f -Force }
}

$manifest = if ($Target -eq 'game') { 'AndroidManifestGame.xml' } else { 'AndroidManifest.xml' }

# ── 1. Compile + link the manifest into a bare APK ────────────────────────────
Write-Host "[1/4] aapt2 link ($Target)"
& $aapt2 link `
    -o $unsigned `
    --manifest (Join-Path $here $manifest) `
    -I $androidJar `
    --min-sdk-version $MinSdk `
    --target-sdk-version $TargetSdk
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed ($LASTEXITCODE)" }

# ── 2. Add the configured native runtime closure under lib/<abi>/ ─────────────
# The build directory is not a product boundary: it can contain stale .so files
# from an older profile. CMake writes a target-graph-derived inventory for each
# NativeActivity entrypoint; package exactly that list plus the entrypoint.
Write-Host "[2/4] adding native libraries"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')
try {
    $inventoryStem = if ($Target -eq 'game') { 'luxgame' } else { 'luxsmoke' }
    $entrypoint = "lib$inventoryStem.so"
    $inventoryPath = Join-Path $soRoots[0] "$inventoryStem.runtime-files"
    if (-not (Test-Path -LiteralPath $inventoryPath)) {
        throw "missing Android runtime inventory: $inventoryPath"
    }
    $required = @($entrypoint)
    $required += Get-Content -LiteralPath $inventoryPath |
        Where-Object { $_ -and -not $_.StartsWith('#') }
    $required = @($required | Sort-Object -Unique)

    $forbidden = 'editor|authoring|toolchain|assimp|shaderc|spirv[-_]?cross|mlir|llvm|node.?editor|nativefiledialog|flowforge|shadergen|material.?graph'
    $forbiddenHits = @($required | Where-Object { $_ -match $forbidden })
    if ($forbiddenHits.Count -ne 0) {
        throw "Android Player inventory contains forbidden product files: $($forbiddenHits -join ', ')"
    }

    $candidates = @{}
    foreach ($root in $soRoots) {
        if (-not (Test-Path $root)) { Write-Warning "  (skipped, not found) $root"; continue }
        foreach ($so in Get-ChildItem -Path $root -Filter '*.so' -File) {
            if (-not $candidates.ContainsKey($so.Name)) {
                $candidates[$so.Name] = $so.FullName
            }
        }
    }
    foreach ($name in $required) {
        if (-not $candidates.ContainsKey($name)) {
            throw "Android runtime inventory entry is missing: $name"
        }
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $candidates[$name], "lib/$Abi/$name",
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    Write-Host "      $($required.Count) inventoried libraries"

    # game target: the cooked paks ride under assets/ — game_main extracts
    # them to internalDataPath on first run, then mounts them through the
    # SAME PakAssetProvider the desktop player uses.
    if ($Target -eq 'game') {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, (Resolve-Path $RuntimeManifest), "assets/game.luxruntime.toml",
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        Write-Host "      assets/game.luxruntime.toml <- $RuntimeManifest"
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, (Resolve-Path $GamePak), "assets/game.luxpak",
            [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
        Write-Host "      assets/game.luxpak <- $GamePak"
        if ($EnginePak -and (Test-Path $EnginePak)) {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zip, (Resolve-Path $EnginePak), "assets/base.luxpak",
                [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
            Write-Host "      assets/base.luxpak <- $EnginePak"
        } else {
            Write-Warning "no -EnginePak: builtin meshes/materials will not resolve"
        }
    }
} finally { $zip.Dispose() }

# ── 3. Align ─────────────────────────────────────────────────────────────────
Write-Host "[3/4] zipalign"
& $zipalign -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed ($LASTEXITCODE)" }

# ── 4. Sign with the standard Android debug key ──────────────────────────────
# ~/.android/debug.keystore with alias/password 'androiddebugkey'/'android' is
# the platform-wide convention every Android SDK install creates; it grants
# nothing beyond installing a debuggable build on a developer device.
Write-Host "[4/4] apksigner"
$ks = Join-Path $env:USERPROFILE '.android\debug.keystore'
if (-not (Test-Path $ks)) {
    throw "debug keystore not found at $ks — create one with keytool, or point this script at your own."
}
& $apksigner sign --ks $ks --ks-pass pass:android --ks-key-alias androiddebugkey `
    --key-pass pass:android --out $signed $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner failed ($LASTEXITCODE)" }

Write-Host "APK: $signed"

if ($Install -or $Run) {
    Write-Host "installing"
    & $adb install -r $signed
    if ($LASTEXITCODE -ne 0) { throw "adb install failed ($LASTEXITCODE)" }
}

if ($Run) {
    $pkg = if ($Target -eq 'game') { 'com.lux.engine.game' } else { 'com.lux.engine.smoke' }
    $tags = if ($Target -eq 'game') { @('luxgame:V','luxstdio:V','AndroidRuntime:E','DEBUG:V') }
            else                    { @('luxsmoke:V','AndroidRuntime:E','DEBUG:V') }
    & $adb logcat -c
    & $adb shell am start -n "$pkg/android.app.NativeActivity" | Out-Null
    Write-Host "--- logcat ($Target) — Ctrl-C to stop ---"
    & $adb logcat -s @tags
}
