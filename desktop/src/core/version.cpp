// desktop/src/core/version.cpp
//
// Placeholder translation unit. dsp_core is a STATIC library; with zero
// compiled sources MSVC's archiver never runs and dsp_core.lib is never
// emitted, breaking downstream linking. This file gives the target one real
// translation unit until later tasks append real sources via target_sources.
#include "core/version.h"
