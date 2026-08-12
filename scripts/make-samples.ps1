# scripts/make-samples.ps1
# Generates two 16 kHz mono PCM16 WAVs with distinct spoken sentences (SAPI TTS).
param([string]$OutDir = "$PSScriptRoot\..\desktop\samples")
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Speech
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.SetOutputToWaveFile((Join-Path $OutDir "mic.wav"), $fmt)
$synth.Speak("Hello everyone, can you hear me clearly today?")
$synth.SetOutputToWaveFile((Join-Path $OutDir "tab.wav"), $fmt)
$synth.Speak("Yes, we can hear you. Let us begin the meeting now.")
$synth.Dispose()
Write-Host "Samples written to $OutDir"
