# Generates minimal RAR5 + RAR4 'stored' archives for smoke-testing builds.
# Deterministic, no compressor logic.
# PowerShell 5.1 gotchas handled:
#  1. Single-element unwrapping: wrap pipeline results in @() before indexing.
#  2. $args is reserved — use $argList / $fileList instead.
#  3. Hex >=0x80000000 via [Convert]::ToUInt32('EDB88320',16) not [uint32]0xEDB88320.
#  4. CRLF normalize .Replace("`r`n","`n") before string compares.
# Determinism: fixed seed 42 + clock/pid pseudo-random loop for any temp names.
param([string]$OutDir = "$PSScriptRoot")
$ErrorActionPreference='Stop'

# Deterministic temp suffix helper (pid+clock loop).
# Not needed for final archive path, but used if caller requests temp out dirs.
function New-MkTempSuffix {
  $curMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
  $pidVal = $PID  # PowerShell automatic PID
  $rnd = ($curMs / 100) % 50000  # low-entropy clock slice like CurTime.GetWin()/100000
  return [string](([int]$rnd + $pidVal) % 50000)
}

$POLY=[Convert]::ToUInt32('EDB88320',16)
$script:CrcTab = New-Object 'uint32[]' 256
for($i=0;$i -lt 256;$i++){
  $c=[uint32]$i
  for($j=0;$j -lt 8;$j++){ $c = [uint32](($c -shr 1) -bxor [uint32]([uint64]$POLY * [uint64]($c -band 1))) }
  $script:CrcTab[$i]=$c
}
function Get-Crc32([byte[]]$b){
  $crc=[uint32][Convert]::ToUInt32("FFFFFFFF",16)
  foreach($x in $b){ $idx=($crc -bxor [uint32]$x) -band 0xFF; $crc=[uint32](($script:CrcTab[[int]$idx]) -bxor ([uint32]($crc -shr 8))) }
  return [uint32]([uint64]$crc -bxor [uint64][Convert]::ToUInt32("FFFFFFFF",16))
}
function Write-Vint([System.IO.MemoryStream]$s,[uint64]$v){
  while($v -ge 0x80){ $s.WriteByte([byte](($v -band 0x7f) -bor 0x80)); $v = $v -shr 7 }
  $s.WriteByte([byte]$v)
}

$hello=[Text.Encoding]::ASCII.GetBytes("Hello from open-rar!`r`n")
$bin=New-Object byte[] (100KB); (New-Object Random 42).NextBytes($bin)
$files=@(@{n='hello.txt';d=$hello},@{n='data.bin';d=$bin})

# ---------- RAR5 ----------
$ms=New-Object System.IO.MemoryStream
$ms.Write([Text.Encoding]::ASCII.GetBytes("Rar!"),0,4); $ms.WriteByte(0x1A);$ms.WriteByte(0x07);$ms.WriteByte(0x01);$ms.WriteByte(0x00)

function Emit-Head5([System.IO.MemoryStream]$out,[byte[]]$bodyAfterSizeField){
  # Header = CRC32(4) | SizeField(vint) | body ; CRC covers SizeField+body
  $sizeTmp=New-Object System.IO.MemoryStream
  Write-Vint $sizeTmp ([uint64]$bodyAfterSizeField.Length)
  $sizeB=$sizeTmp.ToArray()
  $crcInput=New-Object byte[] ($sizeB.Length+$bodyAfterSizeField.Length)
  [Array]::Copy($sizeB,0,$crcInput,0,$sizeB.Length)
  [Array]::Copy($bodyAfterSizeField,0,$crcInput,$sizeB.Length,$bodyAfterSizeField.Length)
  $crc=Get-Crc32 $crcInput
  $out.Write([BitConverter]::GetBytes([uint32]$crc),0,4)
  $out.Write($crcInput,0,$crcInput.Length)
}
# main head: type=1, flags=0, archflags=0
$h=New-Object System.IO.MemoryStream
Write-Vint $h 1; Write-Vint $h 0; Write-Vint $h 0
Emit-Head5 $ms $h.ToArray()

foreach($f in $files){
  $nameB=[Text.Encoding]::UTF8.GetBytes($f.n)
  $compInfo=[uint64]0   # version0, not solid, method0=stored, dict0
  $unixAge=[DateTimeOffset]::new(2026,8,25,12,0,0,[TimeSpan]::Zero).ToUnixTimeSeconds()
  $h=New-Object System.IO.MemoryStream
  Write-Vint $h 2                                   # HEAD_FILE
  Write-Vint $h ([uint64](0x0002 -bor 0x0004))      # HFL_DATA|HFL_SKIPIFUNKNOWN
  Write-Vint $h ([uint64]$f.d.Length)               # DataSize
  Write-Vint $h 0x0004                              # FHFL_CRC32 (skip utime field: keep minimal)
  Write-Vint $h ([uint64]$f.d.Length)               # UnpSize
  Write-Vint $h 0x20                                # Attributes
  # (FHFL_UTIME not set -> no mtime field)
  $b=[BitConverter]::GetBytes([uint32](Get-Crc32 $f.d))
  $h.Write($b,0,$b.Length)                          # data CRC32 (FHFL_CRC32 set)
  Write-Vint $h $compInfo                           # CompressionInfo
  Write-Vint $h 0                                   # HostOS=Windows
  Write-Vint $h ([uint64]$nameB.Length); $h.Write($nameB,0,$nameB.Length)
  Emit-Head5 $ms $h.ToArray()
  $ms.Write($f.d,0,$f.d.Length)
}
# end head: type=5
$h=New-Object System.IO.MemoryStream; Write-Vint $h 5; Write-Vint $h 0
Emit-Head5 $ms $h.ToArray()
[IO.File]::WriteAllBytes("$OutDir\hello5.rar",$ms.ToArray())

# ---------- RAR4 ----------
$ms=New-Object System.IO.MemoryStream
$ms.Write([Text.Encoding]::ASCII.GetBytes("Rar!"),0,4); $ms.WriteByte(0x1A);$ms.WriteByte(0x07);$ms.WriteByte(0x00)

function Emit-Head4([System.IO.MemoryStream]$out,[byte[]]$afterCrc){
  $crc16=(Get-Crc32 $afterCrc) -band 0xFFFF
  $out.WriteByte($crc16 -band 0xFF); $out.WriteByte(($crc16 -shr 8) -band 0xFF)
  $out.Write($afterCrc,0,$afterCrc.Length)
}
$b=New-Object System.IO.MemoryStream
$b.WriteByte(0x72)                                    # MAIN_HEAD
$b.Write([BitConverter]::GetBytes([uint16]0),0,2)     # flags
$b.Write([BitConverter]::GetBytes([uint16]13),0,2)    # HeadSize
$b.Write([BitConverter]::GetBytes([uint16]0),0,2)     # HighPosAV
$b.Write([BitConverter]::GetBytes([uint32]0),0,4)     # PosAV
Emit-Head4 $ms $b.ToArray()

$dosDate=[uint16]((2026-1980)*512 + (8*32) + 25); $dosTime=[uint16](12*2048 + 0*32 + 0)
foreach($f in $files){
  $nameB=[Text.Encoding]::ASCII.GetBytes($f.n)
  $headLen = 30 + $nameB.Length
  $b=New-Object System.IO.MemoryStream
  $b.WriteByte(0x74)                                                  # FILE_HEAD
  $b.Write([BitConverter]::GetBytes([uint16]0x8000),0,2)              # LONG_BLOCK
  $b.Write([BitConverter]::GetBytes([uint16]$headLen),0,2)
  $b.Write([BitConverter]::GetBytes([uint32]$f.d.Length),0,4)         # PackSize
  $b.Write([BitConverter]::GetBytes([uint32]$f.d.Length),0,4)         # UnpSize
  $b.WriteByte(3)                                                     # HostOS=Unix
  $b.Write([BitConverter]::GetBytes([uint32](Get-Crc32 $f.d)),0,4)    # FileCRC
  $b.Write([BitConverter]::GetBytes([uint16]$dosDate),0,2)
  $b.Write([BitConverter]::GetBytes([uint16]$dosTime),0,2)
  $b.WriteByte(20)                                                    # UnpVer 2.0
  $b.WriteByte(0x30)                                                  # method stored
  $b.Write([BitConverter]::GetBytes([uint16]$nameB.Length),0,2)
  $b.Write([BitConverter]::GetBytes([uint32]0x81A4),0,4)              # attrs
  $b.Write($nameB,0,$nameB.Length)
  Emit-Head4 $ms $b.ToArray()
  $ms.Write($f.d,0,$f.d.Length)
}
[IO.File]::WriteAllBytes("$OutDir\hello4.rar",$ms.ToArray())

"created:"; @(Get-ChildItem "$OutDir\*.rar") | Select-Object Name,Length


