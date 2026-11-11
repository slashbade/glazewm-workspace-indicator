param([string]$Windhawk = 'C:\Program Files\Windhawk', [switch]$Test)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$compiler = Join-Path $Windhawk 'Compiler\bin\clang++.exe'
$engine = Get-ChildItem (Join-Path $Windhawk 'Engine') -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!(Test-Path $compiler) -or !$engine) { throw 'Install Windhawk including its compiler first.' }
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force $build | Out-Null
$flags = @('-target', 'x86_64-w64-mingw32', '-std=c++23', '-O2', '-DUNICODE', '-D_UNICODE',
    '-DWH_MOD', '-DWH_MOD_ID=L"glazewm-workspace-indicator"', '-DWH_MOD_VERSION=L"0.1.0"',
    '-include', 'windhawk_api.h', '-Wall', '-Wextra', '-Wno-unused-parameter')
$libs = @('-lole32', '-loleaut32', '-lruntimeobject', '-lwinhttp')
if ($Test) {
    $output = Join-Path $build 'tests.exe'
    & $compiler @flags '-static' '-DWH_EDITING' (Join-Path $root 'tests\tests.cpp') @libs '-o' $output
} else {
    $output = Join-Path $build 'glazewm-workspace-indicator.dll'
    & $compiler @flags '-shared' (Join-Path $root 'glazewm-workspace-indicator.wh.cpp') (Join-Path $engine.FullName '64\windhawk.lib') @libs '-o' $output
}
if ($LASTEXITCODE -ne 0) { throw "Compiler failed: $LASTEXITCODE" }
Write-Output "Built $output"
if ($Test) {
    $previousPath = $env:PATH
    try {
        $env:PATH = (Join-Path $engine.FullName '64') + ';' + (Join-Path $Windhawk 'Compiler\x86_64-w64-mingw32\bin') + ';' + $env:PATH
        & $output
        if ($LASTEXITCODE -ne 0) { throw "Tests failed: $LASTEXITCODE" }
    } finally { $env:PATH = $previousPath }
}
