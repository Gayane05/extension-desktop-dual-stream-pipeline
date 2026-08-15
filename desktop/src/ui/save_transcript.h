// desktop/src/ui/save_transcript.h
//
// Invoked from main_window.cpp's "Save transcript" button with
// TranscriptModel::toText() as input; writes to transcript.txt next to the
// executable. Split out from main_window.cpp specifically so it can be unit
// tested (see below) without pulling ImGui/D3D into the test binary.
#pragma once
#include <string>

namespace dsp
{

// Writes `text` to `path` as raw bytes ("wb" mode). Returns true only on
// full success: the file opened, every byte of `text` was written, and the
// file closed cleanly. On any failure, fills `error` with an ASCII-only,
// human-readable reason and returns false -- callers (the UI) must not treat
// a failed open, a short write (e.g. disk full), or a failed close as a
// silent success.
//
// Deliberately Win32-CRT-free at the *header* level (plain std::string in,
// bool + std::string out) so it is unit-testable from dsp_tests without
// pulling in any windows.h / d3d11.h UI dependencies.
bool saveTranscriptFile(const std::string& path, const std::string& text, std::string& error);

}  // namespace dsp
