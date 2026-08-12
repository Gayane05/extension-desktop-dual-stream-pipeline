# scripts/download-model.ps1
# Downloads a pinned streaming Zipformer English model for sherpa-onnx.
#
# Verified against the k2-fsa/sherpa-onnx "asr-models" GitHub release
# (2026-08-12): sherpa-onnx-streaming-zipformer-en-2023-06-26.tar.bz2 is
# present (~296 MiB), so no fallback model was needed.
param(
    [string]$ModelDir = "$PSScriptRoot\..\desktop\models"
)
$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 defaults to an old TLS version for outbound HTTPS
# and GitHub requires TLS 1.2+; without this, Invoke-WebRequest fails with
# "The underlying connection was closed: The connection was closed
# unexpectedly." Force it before making any request.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$name = "sherpa-onnx-streaming-zipformer-en-2023-06-26"
$url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/$name.tar.bz2"
$dest = Join-Path $ModelDir $name
if (Test-Path (Join-Path $dest "tokens.txt")) {
    Write-Host "Model already present at $dest"
    exit 0
}
New-Item -ItemType Directory -Force $ModelDir | Out-Null
$archive = Join-Path $ModelDir "$name.tar.bz2"
Write-Host "Downloading $url ..."
# -UseBasicParsing avoids Invoke-WebRequest's IE-engine HTML parsing, which
# throws "Windows PowerShell is in NonInteractive mode" on machines without
# IE first-run configured (irrelevant here anyway since this is a binary
# download, not HTML).
Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing
tar -xjf $archive -C $ModelDir
Remove-Item $archive
Write-Host "Model ready: $dest"
