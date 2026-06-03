param(
  [string]$InputDir = "D:\Music",
  [string]$OutputDir = "D:\tf_music\music",
  [string]$Ffmpeg = "ffmpeg"
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command $Ffmpeg -ErrorAction SilentlyContinue)) {
  throw "ffmpeg not found. Install ffmpeg or pass -Ffmpeg C:\path\to\ffmpeg.exe"
}
if (-not (Test-Path -LiteralPath $InputDir)) {
  throw "InputDir not found: $InputDir"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$supported = @(".mp3", ".wav", ".flac", ".m4a", ".aac", ".ogg")
$blocked = @(".kgm", ".kgma")
$index = 1

Get-ChildItem -LiteralPath $InputDir -File | Sort-Object Name | ForEach-Object {
  $ext = $_.Extension.ToLowerInvariant()
  if ($blocked -contains $ext) {
    Write-Warning "skip encrypted/unsupported: $($_.Name)"
    return
  }
  if (-not ($supported -contains $ext)) {
    return
  }

  $safeName = [IO.Path]::GetFileNameWithoutExtension($_.Name)
  $safeName = $safeName -replace '[^A-Za-z0-9._ -]', '_'
  $safeName = $safeName -replace '[\\/:*?"<>|]', '_'
  $safeName = $safeName -replace '_+', '_'
  $safeName = $safeName.Trim(' ', '.', '_')
  if ([string]::IsNullOrWhiteSpace($safeName)) {
    $safeName = "track"
  }
  if ($safeName.Length -gt 40) {
    $safeName = $safeName.Substring(0, 40).Trim(' ', '.', '_')
  }
  $outName = "{0:D2}_{1}.wav" -f $index, $safeName
  $outPath = Join-Path $OutputDir $outName

  Write-Host "convert: $($_.Name) -> $outName"
  & $Ffmpeg -hide_banner -loglevel error -y -i $_.FullName -vn -ac 1 -ar 16000 -sample_fmt s16 $outPath
  if ($LASTEXITCODE -ne 0) {
    throw "ffmpeg failed for $($_.FullName)"
  }
  $index++
}

Write-Host "done. Copy this folder to the TF card root so files are under /music."
