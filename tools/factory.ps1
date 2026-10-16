# Factory provisioning from PowerShell: runs tools/factory.sh with Git for Windows' bash (same options).
#   tools\factory.ps1 COM7
#   tools\factory.ps1 COM7 --cert D:\...\data\secrets\ESP...cert.pem --key D:\...\data\secrets\ESP...key.pem
#   tools\factory.ps1 --mac 02:00:00:00:00:01 --no-flash
$bash = @("$env:ProgramFiles\Git\bin\bash.exe", "${env:ProgramFiles(x86)}\Git\bin\bash.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $bash) { throw 'Git for Windows (bash.exe) not found: install it or run tools/factory.sh from another shell' }
& $bash (Join-Path $PSScriptRoot 'factory.sh') @args
exit $LASTEXITCODE
