$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot)
New-Item -ItemType Directory -Force -Path 'test-output' | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -property installationPath
if (-not $vsRoot) { throw 'Visual Studio C++ Build Tools are needed for the host protocol check.' }
$devCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
# The batch file only configures the compiler and compiles/runs this workspace test.
$batch = "@echo off`r`ncall `"$devCmd`" -arch=x64 -host_arch=x64 >nul`r`ncl /nologo /EHsc /std:c++17 /Iinclude tests\protocol_test.cpp /Fetest-output\protocol_test.exe /Fotest-output\protocol_test.obj`r`nif errorlevel 1 exit /b 1`r`ntest-output\protocol_test.exe`r`n"
Set-Content -LiteralPath 'test-output\compile-test.cmd' -Value $batch -Encoding ascii
& cmd.exe /d /c 'test-output\compile-test.cmd'
if ($LASTEXITCODE) { throw 'Protocol tests failed.' }
$linkBatch = $batch.Replace('tests\protocol_test.cpp', 'tests\link_policy_test.cpp').Replace('protocol_test.exe', 'link_policy_test.exe').Replace('protocol_test.obj', 'link_policy_test.obj')
Set-Content -LiteralPath 'test-output\compile-link-test.cmd' -Value $linkBatch -Encoding ascii
& cmd.exe /d /c 'test-output\compile-link-test.cmd'
if ($LASTEXITCODE) { throw 'Link recovery tests failed.' }
$labBatch = $batch.Replace('tests\protocol_test.cpp', 'tests\lab_test.cpp').Replace('protocol_test.exe', 'lab_test.exe').Replace('protocol_test.obj', 'lab_test.obj')
Set-Content -LiteralPath 'test-output\compile-lab-test.cmd' -Value $labBatch -Encoding ascii
& cmd.exe /d /c 'test-output\compile-lab-test.cmd'
if ($LASTEXITCODE) { throw 'Radio coordinator tests failed.' }
$rtkBatch = $batch.Replace('tests\protocol_test.cpp', 'tests\rtk_sim_test.cpp').Replace('protocol_test.exe', 'rtk_sim_test.exe').Replace('protocol_test.obj', 'rtk_sim_test.obj')
Set-Content -LiteralPath 'test-output\compile-rtk-test.cmd' -Value $rtkBatch -Encoding ascii
& cmd.exe /d /c 'test-output\compile-rtk-test.cmd'
if ($LASTEXITCODE) { throw 'RTK simulation tests failed.' }
$qualityBatch = $batch.Replace('tests\protocol_test.cpp', 'tests\radio_quality_test.cpp').Replace('protocol_test.exe', 'radio_quality_test.exe').Replace('protocol_test.obj', 'radio_quality_test.obj')
Set-Content -LiteralPath 'test-output\compile-quality-test.cmd' -Value $qualityBatch -Encoding ascii
& cmd.exe /d /c 'test-output\compile-quality-test.cmd'
if ($LASTEXITCODE) { throw 'Radio quality tests failed.' }
python scripts/check_web.py
if ($LASTEXITCODE) { throw 'Web checks failed.' }
