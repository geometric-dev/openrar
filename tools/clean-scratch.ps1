$ErrorActionPreference = "SilentlyContinue"
$scratchDirs = @(
    "bench_out", "ci_out", "ci_out_linux2", "ci_out_wsl", "ci_test_data", "ci_test_data_wsl",
    "ext_*", "rt_out", "scratch", "alt", "dist", "build-linux", "test_*", "dest_*", "out_*",
    "debug_ext", "ext_openrar", "ext_rar", "tests/out_*"
)
foreach ($pattern in $scratchDirs) {
    Get-Item $pattern -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.PSIsContainer) {
            Remove-Item -Recurse -Force $_.FullName -ErrorAction SilentlyContinue
        }
    }
}
Remove-Item -Force *.rar -ErrorAction SilentlyContinue
Remove-Item -Force *.bin -ErrorAction SilentlyContinue
Remove-Item -Force *.bad -ErrorAction SilentlyContinue
Remove-Item -Force *.log -ErrorAction SilentlyContinue
Remove-Item -Force *.tmp -ErrorAction SilentlyContinue
Remove-Item -Force errr*.txt, outr*.txt -ErrorAction SilentlyContinue
Write-Host "Scratch directories and files cleaned up."
