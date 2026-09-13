$ErrorActionPreference = "SilentlyContinue"
$scratchDirs = "bench_out", "ci_out", "ci_out_linux2", "ci_out_wsl", "ci_test_data", "ci_test_data_wsl", "ext_*", "rt_out", "scratch", "alt", "dist", "build-linux"
foreach ($dir in $scratchDirs) {
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
}
Remove-Item -Force *.rar
Remove-Item -Force *.bin
Remove-Item -Force errr*.txt, outr*.txt
Write-Host "Scratch directories and files cleaned up."
