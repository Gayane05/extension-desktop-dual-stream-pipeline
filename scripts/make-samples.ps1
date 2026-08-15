# scripts/make-samples.ps1
# Generates two 16 kHz mono PCM16 WAVs with distinct spoken sentences (SAPI
# TTS). wav_client streams both files simultaneously (one per lane), so the
# tab lane's speech is delayed via an SSML break: the "remote participant"
# answers AFTER the mic lane's question finishes, making the demo transcript
# read like a real turn-taking conversation instead of two people talking
# over each other.
param([string]$OutDir = "$PSScriptRoot\..\desktop\samples")
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Speech
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
$synth.SetOutputToWaveFile((Join-Path $OutDir "mic.wav"), $fmt)
$synth.Speak("Hello everyone, can you hear me clearly today?")
$synth.SetOutputToWaveFile((Join-Path $OutDir "tab.wav"), $fmt)
# The leading break (~4.5 s) covers the mic question's duration; SAPI renders
# it as silence at the start of the tab WAV.
$tabSsml = '<speak version="1.0" xmlns="http://www.w3.org/2001/10/synthesis" xml:lang="en-US">' +
           '<break time="4500ms"/>Yes, we can hear you. Let us begin the meeting now.</speak>'
$synth.SpeakSsml($tabSsml)
$synth.Dispose()
Write-Host "Samples written to $OutDir (tab lane delayed 4.5s for turn-taking)"
