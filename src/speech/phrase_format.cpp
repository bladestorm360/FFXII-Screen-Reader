#include "speech/phrase_format.h"

namespace PhraseFormat {

std::wstring Percent(Phrase::Id prefix, long long cur, long long max) {
    if (max <= 0) return std::wstring();
    const long long pct = cur * 100 / max;
    return std::wstring(Phrase::Get(prefix)) + std::to_wstring(pct) +
           Phrase::Get(Phrase::Id::PercentSuffix);
}

} // namespace PhraseFormat
