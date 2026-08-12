#include "app/transcript_model.h"

#include <algorithm>
#include <cstdio>

namespace dsp {

void TranscriptModel::apply(const TranscriptEvent& ev) {
    std::lock_guard lk(mu_);
    auto& pending = pending_[static_cast<int>(ev.stream)];
    if (!ev.isFinal) {
        pending = Utterance{ev.stream, ev.text, false, ev.tsMs};
        return;
    }
    pending.reset();
    if (ev.text.empty()) return;
    Utterance u{ev.stream, ev.text, true, ev.tsMs};
    auto it = std::upper_bound(finals_.begin(), finals_.end(), u.tsMs,
                               [](double ts, const Utterance& x) { return ts < x.tsMs; });
    finals_.insert(it, std::move(u));
}

std::vector<Utterance> TranscriptModel::snapshot() const {
    std::lock_guard lk(mu_);
    std::vector<Utterance> out = finals_;
    for (const auto& p : pending_) if (p && !p->text.empty()) out.push_back(*p);
    return out;
}

void TranscriptModel::clear() {
    std::lock_guard lk(mu_);
    finals_.clear();
    pending_[0].reset();
    pending_[1].reset();
}

std::string TranscriptModel::toText() const {
    std::lock_guard lk(mu_);
    std::string out;
    for (const auto& u : finals_) {
        const auto totalSec = static_cast<long long>(u.tsMs / 1000.0);
        char ts[16];
        std::snprintf(ts, sizeof(ts), "%02lld:%02lld:%02lld",
                      (totalSec / 3600) % 24, (totalSec / 60) % 60, totalSec % 60);
        out += "[" + std::string(ts) + "] " +
               (u.stream == StreamId::Mic ? "You: " : "Others: ") + u.text + "\n";
    }
    return out;
}

}  // namespace dsp
