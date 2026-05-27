param(
  [string]$Port = "COM3",
  [string]$AdfPath = "D:\esp32\esp32-robot\_tmp_esp_adf_official",
  [string]$IdfExport = "D:\esp32\esp32-robot\_tmp_esp_adf_official\esp-idf\export.ps1",
  [switch]$NoMonitor,
  [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$IdfPath = Split-Path -Parent $IdfExport
$FreertosPatch = Join-Path $AdfPath "idf_patches\idf_v5.5_freertos.patch"
$FreertosAdditionsHeader = Join-Path $IdfPath "components\freertos\esp_additions\include\freertos\idf_additions.h"
$FreertosAdditionsSource = Join-Path $IdfPath "components\freertos\esp_additions\freertos_tasks_c_additions.h"
$FreertosLinker = Join-Path $IdfPath "components\freertos\linker_common.lf"

function Invoke-Checked {
  param(
    [Parameter(Mandatory = $true)]
    [string]$FilePath,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments
  )

  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$FilePath failed with exit code $LASTEXITCODE"
  }
}

if (-not (Test-Path $IdfExport)) {
  throw "ESP-IDF export.ps1 not found: $IdfExport"
}
if (-not (Test-Path $AdfPath)) {
  throw "ADF_PATH not found: $AdfPath"
}
if (-not (Test-Path $FreertosPatch)) {
  throw "Missing ADF FreeRTOS patch: $FreertosPatch"
}

$PatchApplied =
  (Select-String -Path $FreertosAdditionsHeader -Pattern "xTaskCreateRestrictedPinnedToCore" -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1) -and
  (Select-String -Path $FreertosAdditionsSource -Pattern "xTaskCreateRestrictedPinnedToCore" -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1) -and
  (Select-String -Path $FreertosLinker -Pattern "xTaskCreateRestrictedPinnedToCore" -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1)

if ($PatchApplied) {
  Write-Host "ADF FreeRTOS patch already applied."
} else {
  Write-Host "Applying ADF FreeRTOS patch..."
  Invoke-Checked git -C $IdfPath apply $FreertosPatch
}

$env:ADF_PATH = $AdfPath
. $IdfExport

Set-Location $ProjectDir

if (-not (Test-Path (Join-Path $ProjectDir "sdkconfig"))) {
  Invoke-Checked idf.py set-target esp32s3
} else {
  $ConfiguredTarget = Select-String -Path (Join-Path $ProjectDir "sdkconfig") -Pattern '^CONFIG_IDF_TARGET="esp32s3"$' -ErrorAction SilentlyContinue
  if (-not $ConfiguredTarget) {
    Invoke-Checked idf.py set-target esp32s3
  }
}

Invoke-Checked idf.py -DCCACHE_ENABLE=0 build

if (-not $NoFlash) {
  Invoke-Checked idf.py -p $Port flash
}

if (-not $NoMonitor) {
  idf.py -p $Port monitor
}
