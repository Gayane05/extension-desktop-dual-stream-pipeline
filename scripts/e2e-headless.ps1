# scripts/e2e-headless.ps1
# Full desktop-pipeline E2E without Chrome: headless app + wav_client.
param(
    [string]$BuildDir = "$PSScriptRoot\..\desktop\build",
    [string]$Engine = "sherpa"
)
$ErrorActionPreference = "Stop"
$exeDir = Join-Path $BuildDir "Release"       # single-config layouts: adjust
if (-not (Test-Path (Join-Path $exeDir "transcriber.exe"))) {
    $exeDir = Get-ChildItem -Recurse $BuildDir -Filter transcriber.exe | Select-Object -First 1 -ExpandProperty DirectoryName
}
& "$PSScriptRoot\make-samples.ps1"
$samples = "$PSScriptRoot\..\desktop\samples"
$out = Join-Path $env:TEMP "e2e-transcript.jsonl"
$proc = Start-Process -FilePath (Join-Path $exeDir "transcriber.exe") `
    -ArgumentList "--headless", "--duration", "40", "--engine", $Engine, "--model-dir", "$PSScriptRoot\..\desktop\models" `
    -RedirectStandardOutput $out -RedirectStandardError (Join-Path $env:TEMP "e2e-stderr.log") `
    -PassThru -NoNewWindow
Start-Sleep -Seconds 3   # let the engine load
& (Join-Path $exeDir "wav_client.exe") (Join-Path $samples "mic.wav") (Join-Path $samples "tab.wav")
$proc.WaitForExit()
$lines = Get-Content $out
Write-Host "--- transcript ---"; $lines | Write-Host
$micOk = $lines | Where-Object { $_ -match '"stream":"mic"' -and $_ -match 'hear' }
$tabOk = $lines | Where-Object { $_ -match '"stream":"tab"' -and $_ -match 'meeting' }
if ($micOk -and $tabOk) { Write-Host "E2E PASS" -ForegroundColor Green; exit 0 }
Write-Host "E2E FAIL (mic:$([bool]$micOk) tab:$([bool]$tabOk))" -ForegroundColor Red
exit 1
