[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build-windows',
    [ValidateSet('Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$BackupDirectory = ''
)
$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot
if (![System.IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $projectDirectory $BuildDirectory }
if (!$BackupDirectory) { $BackupDirectory = Join-Path $projectDirectory 'backups' }
$BackupDirectory = [System.IO.Path]::GetFullPath($BackupDirectory)
$bundleSource = (Resolve-Path -LiteralPath (Join-Path $BuildDirectory "PocketBalance_artefacts\$Configuration\VST3\Pocket Balance.vst3")).Path
$vstDirectory = [System.IO.Path]::GetFullPath((Join-Path $env:CommonProgramFiles 'VST3'))
$installedBundle = [System.IO.Path]::GetFullPath((Join-Path $vstDirectory 'Pocket Balance.vst3'))

function Assert-FlStudioClosed {
    $flProcesses = @(Get-Process -ErrorAction Stop | Where-Object { $_.ProcessName -match '^(FL(32|64)?(_scaled)?|FL Studio.*)$' })
    if ($flProcesses.Count -gt 0) { throw 'FL Studio is running. Save your project and close FL Studio, then run this script again. This script never closes applications.' }
}
function Get-BundleManifest([string]$Folder) {
    $resolvedFolder = (Resolve-Path -LiteralPath $Folder).Path.TrimEnd('\')
    $rootItem = Get-Item -LiteralPath $resolvedFolder -Force
    if (!$rootItem.PSIsContainer -or ($rootItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) { throw "Bundle must be a real directory: $resolvedFolder" }
    $items = @(Get-ChildItem -LiteralPath $resolvedFolder -Recurse -Force)
    if (@($items | Where-Object { $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint }).Count -gt 0) {
        throw "Refusing bundle containing links or junctions: $resolvedFolder"
    }
    $manifest = @{}
    foreach ($file in $items | Where-Object { !$_.PSIsContainer }) {
        $relative = $file.FullName.Substring($resolvedFolder.Length + 1)
        $manifest[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    if ($manifest.Count -eq 0) { throw "Bundle is empty: $resolvedFolder" }
    return $manifest
}
function Assert-ManifestEqual($Expected, $Actual) {
    if ($Expected.Count -ne $Actual.Count) { throw 'Bundle file count verification failed.' }
    foreach ($name in $Expected.Keys) {
        if (!$Actual.ContainsKey($name) -or $Expected[$name] -ne $Actual[$name]) { throw "Bundle hash verification failed: $name" }
    }
}

Assert-FlStudioClosed
if ($bundleSource -eq $installedBundle) { throw 'Build source and installation must be different folders.' }
if (!(Test-Path -LiteralPath (Join-Path $bundleSource 'Contents\x86_64-win\Pocket Balance.vst3') -PathType Leaf)) { throw 'Build does not contain the Windows x64 VST3 binary.' }
if ($BackupDirectory.StartsWith($vstDirectory.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase) -or $BackupDirectory -eq $vstDirectory) {
    throw 'Backups must be outside the VST3 scan folder.'
}
$sourceManifest = Get-BundleManifest $bundleSource
[void][System.IO.Directory]::CreateDirectory($vstDirectory)
$identifier = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$stagingBundle = [System.IO.Path]::GetFullPath((Join-Path $vstDirectory "Pocket-Balance-staging-$identifier"))
$backupBundle = [System.IO.Path]::GetFullPath((Join-Path $BackupDirectory "Pocket-Balance-previous-$identifier.vst3"))
if ((Test-Path -LiteralPath $stagingBundle) -or (Test-Path -LiteralPath $backupBundle)) { throw 'Fresh deployment path already exists.' }
Copy-Item -LiteralPath $bundleSource -Destination $stagingBundle -Recurse
Assert-ManifestEqual $sourceManifest (Get-BundleManifest $stagingBundle)
Assert-FlStudioClosed

# Both moves use previously resolved, exact paths. No recursive deletion is used.
$movedPrevious = $false
$installedNew = $false
try {
    if (Test-Path -LiteralPath $installedBundle) {
        $previousManifest = Get-BundleManifest $installedBundle
        [void][System.IO.Directory]::CreateDirectory($BackupDirectory)
        Move-Item -LiteralPath $installedBundle -Destination $backupBundle
        $movedPrevious = $true
        Assert-ManifestEqual $previousManifest (Get-BundleManifest $backupBundle)
    }
    Move-Item -LiteralPath $stagingBundle -Destination $installedBundle
    $installedNew = $true
    Assert-ManifestEqual $sourceManifest (Get-BundleManifest $installedBundle)
} catch {
    if ($installedNew -and !(Test-Path -LiteralPath $stagingBundle)) {
        Move-Item -LiteralPath $installedBundle -Destination $stagingBundle
    }
    if ($movedPrevious -and !(Test-Path -LiteralPath $installedBundle)) {
        Move-Item -LiteralPath $backupBundle -Destination $installedBundle
    }
    throw
}
Write-Output "Installed and hash-verified: $installedBundle"
if ($movedPrevious) { Write-Output "Previous complete bundle preserved: $backupBundle" }
Write-Output 'Reopen FL Studio and scan plugins if needed. This script does not verify playback.'
