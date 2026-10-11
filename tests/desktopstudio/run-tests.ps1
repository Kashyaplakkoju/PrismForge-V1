param([switch]$Benchmark, [string]$VcpkgRoot='C:\vcpkg')
$ErrorActionPreference='Stop'
# Use the complete current engine/provider graph rather than the historical stack list.
& (Join-Path $PSScriptRoot '..\integration-engine\run-tests.ps1') -VcpkgRoot $VcpkgRoot -Fixtures @('desktop_studio_tests') -Benchmark:$Benchmark
exit $LASTEXITCODE
