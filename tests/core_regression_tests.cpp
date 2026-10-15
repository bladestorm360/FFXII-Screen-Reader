#include "core/game_text.h"
#include "core/game_glyphs.h"
#include "core/game_glyphs_pl.h"
#include "core/logger.h"
#include "runtime_stubs.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
std::wstring Decode(std::initializer_list<uint8_t> bytes, size_t cap = 4096) {
    std::vector<uint8_t> input(bytes);
    if (cap > input.size()) cap = input.size();
    return GameText::Decode(input.data(), cap);
}
std::wstring NestedSprite(int) {
    const uint8_t label[] = {0x20, 0x00};
    return GameText::Decode(label, sizeof(label));
}
void DecoderCases() {
    GameText::SetVariant(GameText::Variant::Standard);
    Check(Decode({0x20, 0x0F, 0x3F, 0x46, 0x00, 0x21, 0x00}) == L"AB",
          "zero sprite parameter must not erase following text");
    Check(Decode({0x20, 0x0F, 0x40, 0x00, 0x21, 0x00}) == L"AB",
          "zero icon parameter must not terminate a string");
    Check(Decode({0x20, 0x08, 0x00, 0x00, 0x21, 0x00}) == L"AB",
          "control parameters are not string terminators");
    Check(Decode({0x20, 0x10, 0x00, 0x21, 0x00}) == L"AB",
          "extended glyph consumes its trailing byte");
    Check(Decode({0x20, 0x00, 0x21, 0x00}) == L"A", "boundary NUL terminates");
    Check(Decode({0x20, 0x0B, 0x21, 0x00}) == L"A", "codec terminator remains a terminator");
    Check(Decode({0x20, 0x0E, 0x21, 0x00}) == L"A", "unknown control preserves only safe prefix");
    Check(GameText::LastDecodeBail().offset == 1, "unknown control diagnostic names its offset");
    Check(Decode({0x20, 0x0F, 0xFF, 0x21, 0x00}) == L"A", "unknown escape never guesses a length");
    Check(GameText::LastDecodeBail().offset == 2, "unknown selector diagnostic names its offset");
    Check(Decode({0x20, 0x0F, 0x29, 0x82, 0x00, 0x00, 0x21, 0x00}) == L"AB",
          "variable colour payload can contain zero bytes");
    Check(Decode({0x20, 0x0F, 0x2A, 0x00, 0x82, 0x00, 0x00, 0x21, 0x00}) == L"AB",
          "annotation length is read before interpreting its payload");

    const uint8_t field[] = {0x0F, 0x2D, 0x80, 0xA2, 0x00};
    { GameText::NumericFieldScope scope(7, 2);
      Check(GameText::Decode(field, 3).empty(), "truncated numeric field must not invent a value");
      Check(GameText::LastDecodeBail().reason != nullptr, "truncation is diagnosed"); }
    { GameText::NumericFieldScope scope(7, 2);
      Check(GameText::Decode(field, sizeof(field)) == L"07", "complete field preserves format"); }
    { GameText::NumericFieldScope scope(7, 2);
      Check(Decode({0x0F, 0x2D, 0x80, 0xA2, 0x04, 0x0F, 0x2D, 0x80, 0xA2, 0x00}) == L"07 ",
            "one numeric scope is consumed once"); }
    Check(GameText::Decode(field, sizeof(field)).empty(), "numeric value does not escape its scope");
    { GameText::NumericFieldScope outer(7, 2);
      { GameText::NumericFieldScope inner(9, 2);
        Check(GameText::Decode(field, sizeof(field)) == L"09", "nested field uses its own value"); }
      Check(GameText::Decode(field, sizeof(field)) == L"07", "nested field restores the outer scope"); }
    Check(Decode({0x0F,0x2E,0x81,0x90,0x04,0x0F,0x2E,0x82,0x90,0x00}) == L"7 14",
          "multiple macros retain their distinct indices");
    Check(Decode({0x0F,0x2E,0x80,0x00,0x00}) == L"0", "zero macro parameter is still payload");

    std::vector<std::wstring> pages;
    const uint8_t paged[] = {0x04, 0x20, 0x03, 0x02, 0x21, 0x04, 0x00};
    GameText::DecodePages(paged, sizeof(paged), pages);
    Check(pages == std::vector<std::wstring>{L"A", L"B"}, "page boundaries and whitespace retained");
    Check(GameText::Decode(paged, sizeof(paged)) == L" A\nB ", "flat decode retains its existing spacing");
    const uint8_t wide[] = {'e','x','0','0','A',0,'{',0,'i',0,'}',0,'B',0,0,0};
    Check(GameText::Decode(wide, sizeof(wide)) == L"AB", "wide strings remain a separate format");

    GameText::SetElementSpriteResolver(&NestedSprite);
    Check(Decode({0x0F,0x3F,0x81,0x8A,0x0F,0xFF,0x00}) == L"A", "nested sprite text resolves");
    Check(GameText::LastDecodeBail().offset == 5 && GameText::LastDecodeBail().reason != nullptr,
          "nested sprite decode must not erase outer failure diagnostic");
    GameText::SetElementSpriteResolver(nullptr);
}
void GuardedBoundary() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const size_t page = info.dwPageSize;
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(memory != nullptr, "allocate guarded boundary");
    if (!memory) return;
    DWORD old = 0;
    Check(VirtualProtect(memory + page, page, PAGE_NOACCESS, &old) != 0, "protect next page");
    uint8_t* p = memory + page - 3;
    p[0] = 0x20; p[1] = 0x0F; p[2] = 0x40;
    Check(GameText::Decode(p, 3) == L"A", "byte budget ends safely at an incomplete escape");
    Check(GameText::Decode(p, 4).empty(), "unmapped source fails safely under SEH");
    p = memory + page - 2;
    p[0] = 0x20; p[1] = 0;
    Check(GameText::Decode(p, 512) == L"A", "terminator stops before an unmapped page");
    VirtualFree(memory, 0, MEM_RELEASE);
}
void OverrideDuringDetection() {
    GameText::SetVariant(GameText::Variant::Standard);
    TestRuntime::PauseNextLookup();
    std::thread detector([] { Decode({0x20, 0x00}); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!TestRuntime::LookupStarted() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    Check(TestRuntime::LookupStarted(), "font probe reaches the controlled lookup");
    GameText::SetVariant(GameText::Variant::PolishPatch);
    TestRuntime::ReleaseLookup();
    detector.join();
    const int lookups = TestRuntime::LookupCount();
    Check(GameText::GetVariant() == GameText::Variant::PolishPatch,
          "in-flight detection cannot replace the manual Polish override");
    const auto glyph = GameText::kGlyphPolish[0];
    Check(Decode({glyph.byte, 0x00}) == std::wstring(1, glyph.ch),
          "manual override still selects the Polish atlas after probe failure");
    Check(TestRuntime::LookupCount() == lookups,
          "failed probe cannot restart detection while an override is active");
    GameText::SetVariant(GameText::Variant::Standard);
}
void ConcurrentGlyphs() {
    const auto glyph = GameText::kGlyphPolish[0];
    const std::wstring stock(128, GameText::kGlyph[glyph.byte]);
    const std::wstring polish(128, glyph.ch);
    std::vector<uint8_t> input(129, glyph.byte);
    input.back() = 0;
    std::atomic<bool> consistent{true};
    std::thread writer([] {
        for (int i = 0; i < 8000; ++i)
            GameText::SetVariant(i & 1 ? GameText::Variant::PolishPatch : GameText::Variant::Standard);
    });
    for (int i = 0; i < 8000; ++i) {
        const auto line = GameText::Decode(input.data(), input.size());
        if (line != stock && line != polish) consistent.store(false);
    }
    writer.join();
    Check(consistent.load(), "concurrent variant changes cannot mix atlases within a string");
    GameText::SetVariant(GameText::Variant::Standard);
}
void Utf8Cases() {
    char out[8];
    std::memset(out, '!', sizeof(out));
    Log::ToUtf8(std::wstring(4096, L'A'), out, sizeof(out));
    Check(std::strcmp(out, "AAAAAAA") == 0, "long log text truncates to a readable prefix");
    Log::ToUtf8(L"\u00E9\u00E9", out, 4);
    Check(std::strcmp(out, "\xC3\xA9") == 0, "UTF-8 truncation keeps a complete character");
    const std::wstring emoji{wchar_t(0xD83D), wchar_t(0xDE00), L'x'};
    Log::ToUtf8(emoji, out, 4);
    Check(out[0] == '\0', "a truncated surrogate pair is not replaced with wrong text");
    Log::ToUtf8(emoji, out, 5);
    Check(std::strcmp(out, "\xF0\x9F\x98\x80") == 0, "a complete surrogate pair fits its byte budget");
    out[0] = '!';
    Log::ToUtf8(L"A", out, 0);
    Check(out[0] == '!', "zero-capacity output is not written");
    Log::ToUtf8(L"A", nullptr, 0);
    Log::ToUtf8(L"A", out, 1);
    Check(out[0] == '\0', "one-byte output retains its terminator");
}
}

int main() {
    DecoderCases();
    GuardedBoundary();
    OverrideDuringDetection();
    ConcurrentGlyphs();
    Utf8Cases();
    if (failures) return 1;
    std::puts("Core regression tests passed: codec boundaries, pages, numeric fields, nested decodes, concurrent atlases, UTF-8.");
    return 0;
}
