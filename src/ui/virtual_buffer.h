#pragma once

#include <functional>
#include <string>
#include <vector>

// Virtual-buffer navigation -- a C++ port of the FF1 Pixel-Remaster reader's
// NavigationBuffer (ff1-screen-reader/Core/NavigationBuffer.cs). Holds an ordered list
// of entries, each a std::function<std::wstring()> so a feature reads its value LIVE at
// navigation time (status stats) or returns a pre-rendered string. Owns ALL the index
// wrap/clamp and group-jump logic so feature readers never duplicate it. Holds DATA
// only; the caller speaks the returned string.
//
// The index is public (Index()/SetIndex) so a feature that persists position elsewhere
// -- e.g. the status reader, which keeps its cursor across a page rebuild -- can sync it
// in and out around a navigation call.
//
// Groups are OPTIONAL. Group navigation is a no-op (returns the current entry) when no
// group boundaries were supplied. Group NAMES are optional too: when present a group
// jump prefixes "Name. " (FF1 bestiary), when absent it does not (FF1 status). Named
// after "VirtualBuffer" rather than "NavigationBuffer" to avoid confusion with the
// pathfinder's navigation/ module.
namespace VB {

class VirtualBuffer {
public:
    using Entry = std::function<std::wstring()>;

    VirtualBuffer() = default;
    VirtualBuffer(std::vector<Entry> entries,
                  std::vector<int> groupStarts = {},
                  std::vector<std::wstring> groupNames = {});

    int  Index() const { return index_; }
    void SetIndex(int i) { index_ = i; }
    int  Count() const { return static_cast<int>(entries_.size()); }
    bool IsEmpty() const { return entries_.empty(); }

    // The announce string for the current entry (empty when the buffer is empty).
    // Clamps a stray index back into range.
    std::wstring Current();
    std::wstring Next();       // wraps to top
    std::wstring Previous();   // wraps to bottom
    std::wstring JumpTop();
    std::wstring JumpBottom();
    std::wstring NextGroup();      // first entry of the next group (wraps); no groups -> Current()
    std::wstring PreviousGroup();  // first entry of the previous group (wraps); no groups -> Current()

    // Position of the current entry for an "(X of Y)" announce. With groups, returns the
    // (within-group index, group size); with no groups, (Index, Count). Both out-params
    // are written; safe to pass the same buffer that Current() came from.
    void CurrentGroupPosition(int* localIndex, int* groupCount) const;

private:
    std::wstring withGroup(int groupIndex);

    std::vector<Entry>        entries_;
    std::vector<int>          groupStarts_;   // empty -> no groups
    std::vector<std::wstring> groupNames_;    // parallel to groupStarts_; empty -> no name prefix
    int                       index_ = 0;
};

} // namespace VB
