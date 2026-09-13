# RAR5 format coverage driver.
# Builds a fixed-content test tree, creates archives with many Rar.exe flag
# combinations (twice each for determinism), runs the same tree through our
# OpenRAR writer, and produces tools\coverage-report.json plus a gap summary.
$ErrorActionPreference = 'Continue'

$root = Join-Path $env:TEMP 'rartest-cov'
$tree = Join-Path $root 'tree'
$out = Join-Path $root 'arc'
$cmt = Join-Path $root 'cmt.txt'
$rar = 'C:\Program Files\WinRAR\Rar.exe'
$ourExeCandidates = @(
  (Join-Path $PSScriptRoot '..\build\openrar64\Release\openrar.exe'),
  (Join-Path $PSScriptRoot '..\build\openrar32\Release\openrar.exe')
)
$ourExe = $ourExeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ourExe) { $ourExe = $ourExeCandidates[0] }
$analyze = Join-Path $PSScriptRoot 'rar5-coverage.js'

foreach ($d in @($root, $tree, $out)) {
  if (Test-Path $d) { Remove-Item $d -Recurse -Force }
  New-Item $d -ItemType Directory -Force | Out-Null
}

# ---- fixed-content test tree (fixed mtimes keep output reproducible) ----
# NOTE: kept ASCII-only; PS 5.1 misparses UTF-8 scripts without BOM handling.
New-Item "$tree\dirA\nested", "$tree\uni\unicode", "$tree\d1\d2\d3" -ItemType Directory -Force | Out-Null
Set-Content "$tree\text.txt" ("The quick brown fox jumps over the lazy dog. " * 200)
Set-Content "$tree\empty.txt" ""
Set-Content "$tree\uni\unicode\cyr.txt" "cyrillic"
Set-Content "$tree\d1\d2\d3\deep.txt" "deep"
Set-Content $cmt "OpenRAR coverage probe comment"
$bytes = New-Object byte[] (50KB)
(New-Object Random(7)).NextBytes($bytes)
[System.IO.File]::WriteAllBytes("$tree\data.bin", $bytes)
Copy-Item "$env:WINDIR\System32\notepad.exe" "$tree\note.exe"
Get-ChildItem $tree -Recurse -Force | ForEach-Object {
  $_.LastWriteTime = '2021-06-15 12:00:00'
  $_.CreationTime = '2020-01-01 00:00:00'
}
try { New-Item -ItemType Junction -Path "$tree\junc" -Target "$tree\dirA" -ErrorAction Stop | Out-Null } catch { Write-Host "junction failed: $_" }
try { New-Item -ItemType HardLink -Path "$tree\hard.txt" -Target "$tree\text.txt" -ErrorAction Stop | Out-Null } catch { Write-Host "hardlink failed: $_" }
try { New-Item -ItemType SymbolicLink -Path "$tree\sym.txt" -Target "$tree\text.txt" -ErrorAction Stop | Out-Null } catch { Write-Host "symlink skipped (needs admin/dev mode)" }
Set-Content -Path "$tree\text.txt" -Stream zone.identifier -Value "ads-probe"

if (-not (Test-Path "$tree\text.txt")) { throw "tree setup failed - aborting" }

function Run-Rar([string[]]$Switches, [string]$Arc) {
  Push-Location $tree
  try {
    & $rar a -y -r @Switches -- $Arc . *>$null
    return $LASTEXITCODE -eq 0
  } finally { Pop-Location }
}

function Run-Ours([string]$Switch, [string]$Arc) {
  Push-Location $tree
  try {
    & $ourExe a -y -r $Switch -- $Arc . *>$null
    return $LASTEXITCODE -eq 0
  } finally { Pop-Location }
}

function Archive-Files([string]$Base) {
  Get-ChildItem $out -Filter "$Base*" | Sort-Object Name | ForEach-Object { $_.FullName }
}

function Hash-Archive([string]$Base) {
  $hashes = foreach ($f in (Archive-Files $Base)) { (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash }
  return (($hashes -join ',')).ToLower()
}

function Analyze-Archive([string]$Base) {
  $paths = @(Archive-Files $base)
  if ($paths.Count -eq 0) { return $null }
  $json = & node $analyze --json @paths | Out-String
  if (-not $json.Trim()) { return $null }
  try { $obj = $json | ConvertFrom-Json } catch { Write-Host "analyze parse failed: $_"; return $null }
  # Union features across all parts.
  $union = $null
  foreach ($prop in $obj.PSObject.Properties) {
    $f = $prop.Value
    if (-not $union) { $union = $f; continue }
    foreach ($p in $f.PSObject.Properties) {
      $v = $p.Value
      if ($v -is [bool]) { $union.($p.Name) = ($union.($p.Name) -or $v) }
      elseif ($v -is [array]) {
        $cur = @($union.($p.Name))
        $union.($p.Name) = @(($cur + $v | Sort-Object -Unique))
      }
      elseif ($v -is [int]) { $union.($p.Name) += $v }
    }
  }
  return $union
}

$flagsets = @(
  @{ n = 'default'; s = @() },
  @{ n = 'store'; s = @('-m0') },
  @{ n = 'best'; s = @('-m5') },
  @{ n = 'solid'; s = @('-s') },
  @{ n = 'solid-best'; s = @('-m5', '-s') },
  @{ n = 'qo-off'; s = @('-qo-') },
  @{ n = 'recovery'; s = @('-rr5p') },
  @{ n = 'comment'; s = @("-z$cmt") },
  @{ n = 'times-all'; s = @('-tsm4', '-tsc4', '-tsa4') },
  @{ n = 'owner'; s = @('-ow') },
  @{ n = 'streams'; s = @('-os') },
  @{ n = 'versioning'; s = @('-ver') },
  @{ n = 'metadata'; s = @('-ams') },
  @{ n = 'links'; s = @('-ol', '-oh') },
  @{ n = 'multivol'; s = @('-v10k', '-m5') },
  @{ n = 'enc-headers'; s = @('-hppass123'); nd = $true },
  @{ n = 'enc-files'; s = @('-psecret123'); nd = $true }
)

# Warmup: the first scan after tree creation can observe lazily-settling
# NTFS directory metadata, which would poison the determinism check.
[void](Run-Rar @() "$out\warmup.rar")
Remove-Item "$out\warmup.rar" -Force -ErrorAction SilentlyContinue

$report = [ordered]@{}
foreach ($fs in $flagsets) {
  $name = $fs.n
  $base = "cov-$name"; $base2 = "det-$name"
  Remove-Item "$out\$base*", "$out\$base2*" -Force -ErrorAction SilentlyContinue

  $ok = Run-Rar $fs.s "$out\$base.rar"
  if (-not $ok) {
    $report[$name] = @{ built = $false }
    Write-Host ("{0,-14} BUILD FAILED" -f $name)
    continue
  }
  [void](Run-Rar $fs.s "$out\$base2.rar")

  $features = Analyze-Archive $base
  $det = (Hash-Archive $base) -eq (Hash-Archive $base2)
  if (-not $det) {
    Write-Host "    NONDET: keeping both builds ($base / $base2)"
  } else {
    Remove-Item "$out/$base2*" -Force -ErrorAction SilentlyContinue
  }

  $size = (Get-ChildItem $out -Filter "$base*" | Measure-Object Length -Sum).Sum
  $report[$name] = @{
    built = $true; deterministic = $det; expectedNondeterministic = [bool]$fs.nd
    size = $size; parts = @(Archive-Files $base).Count; features = $features
  }
  Write-Host ("{0,-14} ok det={1} size={2}" -f $name, $det, $size)
}

foreach ($m in @('m0', 'm3', 'm5')) {
  $name = "ours-$m"; $base = "ours-$m"
  Remove-Item "$out\$base*" -Force -ErrorAction SilentlyContinue
  [void](Run-Ours @{ m0 = '-m0'; m3 = '-m3'; m5 = '-m5' }[$m] "$out\$base.rar")
  $h1 = Hash-Archive $base
  [void](Run-Ours @{ m0 = '-m0'; m3 = '-m3'; m5 = '-m5' }[$m] "$out\$base.rar")
  $det = ($h1 -eq (Hash-Archive $base))
  $report[$name] = @{
    built = $true; deterministic = $det; ours = $true
    size = (Get-Item "$out\$base.rar").Length; parts = 1; features = (Analyze-Archive $base)
  }
  Write-Host ("{0,-14} ok det={1} size={2}" -f $name, $det, $report[$name].size)
}

# ---- Gap matrix ----
function FeatureKeys($features) {
  $keys = New-Object System.Collections.Generic.HashSet[string]
  if (-not $features) { return $keys }
  foreach ($p in $features.PSObject.Properties) {
    $v = $p.Value
    if ($v -is [bool]) { if ($v) { [void]$keys.Add($p.Name) } }
    elseif ($v -is [array]) { foreach ($item in $v) { [void]$keys.Add("$($p.Name)=$item") } }
    elseif ($v -is [int64] -or $v -is [int32] -or $v -is [double]) {
      if ($p.Name -in @('files', 'dirs')) { [void]$keys.Add("$($p.Name)>0=$($v -gt 0)") }
    }
    elseif ($v -is [PSCustomObject]) {
      foreach ($c in $v.PSObject.Properties) {
        if ($c.Value) { [void]$keys.Add("$($p.Name).$($c.Name)") }
      }
    }
  }
  return $keys
}

$rarUnion = New-Object System.Collections.Generic.HashSet[string]
$ourUnion = New-Object System.Collections.Generic.HashSet[string]
$featureOrigin = @{}
foreach ($name in $report.Keys) {
  $fk = FeatureKeys $report[$name].features
  $ours = $name -like 'ours-*'
  foreach ($k in $fk) {
    if ($ours) { [void]$ourUnion.Add($k) }
    else {
      [void]$rarUnion.Add($k)
      if (-not $featureOrigin.ContainsKey($k)) { $featureOrigin[$k] = @() }
      $featureOrigin[$k] += $name
    }
  }
}

Write-Host "`n==== GAPS: present in rar.exe output, absent from ALL of ours ===="
foreach ($g in ($rarUnion | Where-Object { -not $ourUnion.Contains($_) } | Sort-Object)) {
  Write-Host ("  MISSING: {0}   [{1}]" -f $g, ($featureOrigin[$g] -join ', '))
}
Write-Host "`n==== COVERED by ours AND seen in rar.exe output ===="
foreach ($k in ($rarUnion | Where-Object { $ourUnion.Contains($_) } | Sort-Object)) {
  Write-Host "  COVERED: $k"
}
Write-Host "`n==== OURS ONLY (not exercised by these rar.exe flagsets) ===="
foreach ($k in ($ourUnion | Where-Object { -not $rarUnion.Contains($_) } | Sort-Object)) {
  Write-Host "  OURS-ONLY: $k"
}

$report | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $PSScriptRoot 'coverage-report.json') -Encoding UTF8
Write-Host "`nJSON report: tools\coverage-report.json"
