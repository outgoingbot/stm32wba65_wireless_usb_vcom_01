param([string]$CubeIDE = 'C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE', [switch]$Full)
$ErrorActionPreference = 'Stop'
$projectDir = $PSScriptRoot
$plugins = Join-Path $CubeIDE 'plugins'
$gcc = Get-ChildItem -LiteralPath $plugins -Directory -Filter '*gnu-tools-for-stm32*' |
    Sort-Object Name -Descending | Select-Object -First 1
$make = Get-ChildItem -LiteralPath $plugins -Directory -Filter '*externaltools.make.win32*' |
    Sort-Object Name -Descending | Select-Object -First 1
if (!$gcc -or !$make) { throw 'STM32CubeIDE GCC/make tools not found. Pass -CubeIDE with your installation path.' }
$savedPath = $env:Path
$artifactDir = Join-Path $projectDir 'artifacts'
New-Item -ItemType Directory -Path $artifactDir -Force | Out-Null
$env:Path = (Join-Path $gcc.FullName 'tools\bin') + ';' + (Join-Path $make.FullName 'tools\bin') + ';' + $env:Path
Push-Location (Join-Path $projectDir 'Debug')
try {
    $log = Join-Path $artifactDir 'build.log'
    $makeArgs = @('-j8', 'main-build')
    if ($Full) { $makeArgs = @('-B') + $makeArgs }
    & make @makeArgs *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 25; throw "Build failed; see $log" }
    $elf = Join-Path $artifactDir 'bridge.elf'
    Copy-Item -LiteralPath 'stm32wba65_wireless_usb_vcom_01.elf' -Destination $elf
    & arm-none-eabi-objcopy -O ihex $elf (Join-Path $artifactDir 'bridge.hex')
    if ($LASTEXITCODE -ne 0) { throw 'HEX conversion failed' }
    & arm-none-eabi-objcopy -O binary $elf (Join-Path $artifactDir 'bridge.bin')
    if ($LASTEXITCODE -ne 0) { throw 'BIN conversion failed' }
    & arm-none-eabi-size $elf
} finally {
    Pop-Location
    $env:Path = $savedPath
}
Get-FileHash -Algorithm SHA256 (Join-Path $artifactDir 'bridge.hex'), (Join-Path $artifactDir 'bridge.bin') |
    ForEach-Object { $_.Hash + '  ' + [IO.Path]::GetFileName($_.Path) } |
    Set-Content (Join-Path $artifactDir 'SHA256.txt')
Write-Output "Built ONE shared firmware in $artifactDir. Flash the same bridge.hex on both boards. No hardware was flashed."
