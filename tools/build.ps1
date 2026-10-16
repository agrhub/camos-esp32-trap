# Build / flash / monitor helper for Windows PowerShell (ESP-IDF v5.4). Usage, from the repo root or anywhere:
#   tools\build.ps1 freenove build            # Freenove ESP32-S3-WROOM CAM (FNK0085), the G-S2 board
#   tools\build.ps1 esp32cam build            # AI-Thinker ESP32-CAM
#   tools\build.ps1 esp32s3cam build          # generic ESP32-S3 N16R8 CAM (ESP32-S3-EYE map)
#   tools\build.ps1 freenove flash COM7       # flash (after a build)
#   tools\build.ps1 freenove monitor COM7     # serial console, Ctrl+] to exit
#   tools\build.ps1 freenove menuconfig
#   tools\build.ps1 freenove clean            # delete the build directory (the disk is small)
# Set $env:IDF_PATH and $env:IDF_TOOLS_PATH when ESP-IDF is not at C:\dev\esp-idf / C:\dev\espressif.
# The build directory is firmware\build-<board> (git-ignored).
param(
    [Parameter(Mandatory = $true)][ValidateSet('freenove', 'esp32cam', 'esp32s3cam')][string]$Board,
    [Parameter(Mandatory = $true)][string]$Action,
    [string]$Port = ''
)
$ErrorActionPreference = 'Stop'
# An IDF_PATH / IDF_TOOLS_PATH pointing at another (older) ESP-IDF install is ignored: this project needs v5.x
$idfPath = $env:IDF_PATH
if (-not $idfPath -or -not (Test-Path "$idfPath\version.txt") -or -not ((Get-Content "$idfPath\version.txt") -match '^v5')) { $idfPath = 'C:\dev\esp-idf' }
$toolsPath = $env:IDF_TOOLS_PATH
if (-not $toolsPath -or -not (Test-Path "$toolsPath\python_env\idf5.*")) { $toolsPath = 'C:\dev\espressif' }
if (-not (Test-Path "$idfPath\tools\idf.py")) { throw "ESP-IDF v5 not found at $idfPath (set IDF_PATH)" }

# export.ps1 insists on every target's toolchain; a minimal install (xtensa only) works with a manual PATH
$dirs = @()
$dirs += Get-ChildItem "$toolsPath\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin" -Directory | Select-Object -ExpandProperty FullName
$dirs += Get-ChildItem "$toolsPath\tools\cmake\*\bin", "$toolsPath\tools\ninja\*", "$toolsPath\tools\ccache\*", "$toolsPath\tools\idf-exe\*" -Directory | Select-Object -ExpandProperty FullName
$venv = Get-ChildItem "$toolsPath\python_env\idf5.*" -Directory | Select-Object -First 1 -ExpandProperty FullName
$env:PATH = ($dirs -join ';') + ";$venv\Scripts;$idfPath\tools;" + $env:PATH
$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = $toolsPath
$env:IDF_PYTHON_ENV_PATH = $venv

$map = @{
    freenove   = @{ target = 'esp32s3'; defaults = 'sdkconfig.defaults;sdkconfig.defaults.freenove_esp32s3_cam' }
    esp32cam   = @{ target = 'esp32';   defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32cam' }
    esp32s3cam = @{ target = 'esp32s3'; defaults = 'sdkconfig.defaults;sdkconfig.defaults.esp32s3cam' }
}
$fw = Join-Path (Split-Path $PSScriptRoot -Parent) 'firmware'
$build = "build-$Board"
Set-Location $fw
if ($Action -eq 'clean') { if (Test-Path $build) { Remove-Item -Recurse -Force $build }; exit 0 }
$common = @('-B', $build, "-DSDKCONFIG=$build/sdkconfig", "-DSDKCONFIG_DEFAULTS=$($map[$Board].defaults)")
$args = @()
if (-not (Test-Path "$build\sdkconfig")) { $args += @('set-target', $map[$Board].target) }
if ($Port) { $common += @('-p', $Port) }
$args += $Action
# idf.py writes warnings to stderr (e.g. the missing esp-rom-elfs tool, harmless): do not let PowerShell stop on them
$ErrorActionPreference = 'Continue'
& python "$idfPath\tools\idf.py" @common @args
exit $LASTEXITCODE
