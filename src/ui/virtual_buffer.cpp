#include "ui/virtual_buffer.h"

// Faithful port of ff1-screen-reader/Core/NavigationBuffer.cs. Every method mirrors the
// C# original's wrap/clamp/group logic exactly; see virtual_buffer.h for the contract.

namespace VB {

VirtualBuffer::VirtualBuffer(std::vector<Entry> entries,
                             std::vector<int> groupStarts,
                             std::vector<std::wstring> groupNames)
    : entries_(std::move(entries)),
      groupStarts_(std::move(groupStarts)),
      groupNames_(std::move(groupNames)),
      index_(0) {}

std::wstring VirtualBuffer::Current() {
    if (entries_.empty()) return std::wstring();
    if (index_ < 0 || index_ >= static_cast<int>(entries_.size())) index_ = 0;
    const Entry& e = entries_[index_];
    return e ? e() : std::wstring();
}

std::wstring VirtualBuffer::Next() {
    if (entries_.empty()) return std::wstring();
    index_ = (index_ + 1) % static_cast<int>(entries_.size());
    return Current();
}

std::wstring VirtualBuffer::Previous() {
    if (entries_.empty()) return std::wstring();
    --index_;
    if (index_ < 0) index_ = static_cast<int>(entries_.size()) - 1;
    return Current();
}

std::wstring VirtualBuffer::JumpTop() {
    if (entries_.empty()) return std::wstring();
    index_ = 0;
    return Current();
}

std::wstring VirtualBuffer::JumpBottom() {
    if (entries_.empty()) return std::wstring();
    index_ = static_cast<int>(entries_.size()) - 1;
    return Current();
}

std::wstring VirtualBuffer::NextGroup() {
    if (entries_.empty()) return std::wstring();
    if (groupStarts_.empty()) return Current();

    int gi = 0, target = groupStarts_[0];
    for (int i = 0; i < static_cast<int>(groupStarts_.size()); ++i) {
        if (groupStarts_[i] > index_) { gi = i; target = groupStarts_[i]; break; }
    }
    index_ = target;
    return withGroup(gi);
}

std::wstring VirtualBuffer::PreviousGroup() {
    if (entries_.empty()) return std::wstring();
    if (groupStarts_.empty()) return Current();

    int gi = static_cast<int>(groupStarts_.size()) - 1;
    int target = groupStarts_[gi];
    for (int i = static_cast<int>(groupStarts_.size()) - 1; i >= 0; --i) {
        if (groupStarts_[i] < index_) { gi = i; target = groupStarts_[i]; break; }
    }
    index_ = target;
    return withGroup(gi);
}

void VirtualBuffer::CurrentGroupPosition(int* localIndex, int* groupCount) const {
    if (entries_.empty()) { *localIndex = 0; *groupCount = 0; return; }
    if (groupStarts_.empty()) { *localIndex = index_; *groupCount = Count(); return; }

    int gStart = 0, gEnd = Count();
    for (int i = 0; i < static_cast<int>(groupStarts_.size()); ++i) {
        if (groupStarts_[i] <= index_) {
            gStart = groupStarts_[i];
            gEnd = (i + 1 < static_cast<int>(groupStarts_.size())) ? groupStarts_[i + 1] : Count();
        } else {
            break;
        }
    }
    *localIndex = index_ - gStart;
    *groupCount = gEnd - gStart;
}

std::wstring VirtualBuffer::withGroup(int groupIndex) {
    std::wstring cur = Current();
    if (groupIndex >= 0 && groupIndex < static_cast<int>(groupNames_.size())) {
        const std::wstring& name = groupNames_[groupIndex];
        // whitespace-only names are treated as "no name" (matches the C# IsNullOrWhiteSpace check).
        bool hasName = false;
        for (wchar_t c : name) { if (c != L' ' && c != L'\t') { hasName = true; break; } }
        if (hasName) return name + L". " + cur;
    }
    return cur;
}

} // namespace VB
