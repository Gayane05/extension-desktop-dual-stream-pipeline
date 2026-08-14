// desktop/src/app/transcript_model.cpp
#include "app/transcript_model.h"

#include <algorithm>
#include <cstdio>

namespace dsp {

// Two-tier state per stream: at most one "pending" (interim) utterance,
// which is wholesale replaced by each new interim from the engine (there is
// only ever one in-flight utterance per lane -- sherpa/deepgram don't emit
// overlapping interims), plus a growing list of "final" utterances that are
// never edited once inserted. isFinal=false events therefore just overwrite
// pending_[stream]; isFinal=true events clear pending_ and graduate the text
// into finals_, keeping finals_ sorted by capture timestamp (not arrival
// order) via upper_bound insertion, since mic and tab finalize independently
// and network jitter can deliver them out of chronological order.
void TranscriptModel::apply(const TranscriptEvent& ev)
{
    std::lock_guard lk(mu_);
    auto& pending = pending_[static_cast<int>(ev.stream)];
    if (!ev.isFinal)
    {
        pending = Utterance{ev.stream, ev.text, false, ev.tsMs};
        return;
    }
    pending.reset();
    if (ev.text.empty())
    {
        return;
    }
    Utterance u{ev.stream, ev.text, true, ev.tsMs};
    auto it = std::upper_bound(finals_.begin(), finals_.end(), u.tsMs,
                               [](double ts, const Utterance& x) { return ts < x.tsMs; });
    finals_.insert(it, std::move(u));
}

// Finals are already sorted by tsMs (see apply()); each stream's pending
// interim is appended after them unconditionally, so an in-progress
// utterance always renders below all completed ones regardless of its own
// timestamp -- matching how a live transcript view is expected to read.
std::vector<Utterance> TranscriptModel::snapshot() const
{
    std::lock_guard lk(mu_);
    std::vector<Utterance> out = finals_;
    for (const auto& p : pending_)
    {
        if (p && !p->text.empty())
        {
            out.push_back(*p);
        }
    }
    return out;
}

void TranscriptModel::clear()
{
    std::lock_guard lk(mu_);
    finals_.clear();
    pending_[0].reset();
    pending_[1].reset();
}

std::string TranscriptModel::toText() const
{
    std::lock_guard lk(mu_);
    std::string out;
    for (const auto& u : finals_)
    {
        const auto totalSec = static_cast<long long>(u.tsMs / 1000.0);
        char ts[16];
        std::snprintf(ts, sizeof(ts), "%02lld:%02lld:%02lld", (totalSec / 3600) % 24,
                      (totalSec / 60) % 60, totalSec % 60);
        out += "[" + std::string(ts) + "] " + (u.stream == StreamId::Mic ? "You: " : "Others: ") +
               u.text + "\n";
    }
    return out;
}

}  // namespace dsp
