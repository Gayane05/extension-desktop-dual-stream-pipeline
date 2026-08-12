// DELETE in Task 11 (replaced by the real ImGui UI).
#include "app/config.h"
#include "app/pipeline.h"
#include "app/transcript_model.h"
#include "stt/stt_engine.h"
#include <cstdio>
namespace dsp {
int runUi(Pipeline&, TranscriptModel&, ISttEngine&, const Config&) {
    std::fprintf(stderr, "UI not built yet — use --headless\n");
    return 1;
}
}
