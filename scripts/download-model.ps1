# scripts/download-model.ps1
# Downloads models for sherpa-onnx from the k2-fsa/sherpa-onnx "asr-models"
# GitHub release. Two asset kinds are supported: model archives (name
# without extension, fetched as <name>.tar.bz2 and extracted) and bare
# .onnx files (e.g. silero_vad.onnx, downloaded into the model dir as-is).
param(
    [string]$ModelDir = "$PSScriptRoot\..\desktop\models",
    # Default: LibriSpeech-only streaming model (smaller). Alternatives:
    #   -Model sherpa-onnx-streaming-zipformer-en-2023-06-21
    #       (LibriSpeech + GigaSpeech, better on meeting/YouTube audio)
    #   -Model sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8
    #       (offline Parakeet for --engine parakeet; also needs silero_vad.onnx)
    #   -Model silero_vad.onnx
    #       (VAD model required by --engine parakeet)
    [string]$Model = "sherpa-onnx-streaming-zipformer-en-2023-06-26"
)
$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 defaults to an old TLS version for outbound HTTPS
# and GitHub requires TLS 1.2+; without this, Invoke-WebRequest fails with
# "The underlying connection was closed: The connection was closed
# unexpectedly." Force it before making any request.
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$name = $Model

# Bare .onnx assets (like silero_vad.onnx) are single files, not archives.
if ($name.EndsWith(".onnx")) {
    $target = Join-Path $ModelDir $name
    if (Test-Path $target) {
        Write-Host "Model already present at $target"
        exit 0
    }
    New-Item -ItemType Directory -Force $ModelDir | Out-Null
    $onnxUrl = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/$name"
    Write-Host "Downloading $onnxUrl ..."
    Invoke-WebRequest -Uri $onnxUrl -OutFile $target -UseBasicParsing
    if (-not (Test-Path $target)) {
        throw "download finished but $target is missing"
    }
    Write-Host "Model ready: $target"
    exit 0
}

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
# Use Windows' own bsdtar explicitly: a GNU tar earlier on PATH (e.g. from
# Git Bash) misparses "C:\..." as a remote host ("Cannot connect to C:").
& "$env:SystemRoot\System32\tar.exe" -xjf $archive -C $ModelDir
if ($LASTEXITCODE -ne 0) {
    throw "tar extraction failed (exit $LASTEXITCODE) for $archive"
}
Remove-Item $archive
if (-not (Test-Path (Join-Path $dest "tokens.txt"))) {
    throw "extraction finished but $dest\tokens.txt is missing - archive layout unexpected"
}
Write-Host "Model ready: $dest"
