[CmdletBinding()]
param(
    [string]$JuceSourceDir = '',
    [string]$BuildDirectory = 'build-windows',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 64)]
    [int]$Parallel = 4,
    [string]$CMakePath = ''
)
$ErrorActionPreference = 'Stop'
$projectDirectory = Split-Path -Parent $PSScriptRoot
if (!$CMakePath) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($cmakeCommand) { $CMakePath = $cmakeCommand.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($vsRoot) { $CMakePath = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
        }
    }
}
if (!$CMakePath -or !(Test-Path -LiteralPath $CMakePath)) {
    throw 'Install CMake 3.22+ and Visual Studio 2022 Build Tools with Desktop development with C++ and the Windows SDK, or supply -CMakePath.'
}
$ctestPath = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'
if (!(Test-Path -LiteralPath $ctestPath)) { throw "CTest was not found beside CMake: $ctestPath" }
if (![System.IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $projectDirectory $BuildDirectory }
$configureArguments = @('-S', $projectDirectory, '-B', $BuildDirectory, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DBUILD_TESTING=ON')
if ($JuceSourceDir) {
    $JuceSourceDir = (Resolve-Path -LiteralPath $JuceSourceDir).Path
    $configureArguments += "-DPOCKET_JUCE_SOURCE_DIR=$JuceSourceDir"
}
& $CMakePath @configureArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE" }
& $CMakePath --build $BuildDirectory --config $Configuration --parallel $Parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
& $ctestPath --test-dir $BuildDirectory -C $Configuration --output-on-failure --no-tests=error
if ($LASTEXITCODE -ne 0) { throw "CTest failed with exit code $LASTEXITCODE" }
Write-Output "Built and processor-tested: $BuildDirectory\PocketBalance_artefacts\$Configuration\VST3\Pocket Balance.vst3"
Write-Output 'Run plugin validation and inspect the UI before deployment. This script does not install or package the plugin.'
