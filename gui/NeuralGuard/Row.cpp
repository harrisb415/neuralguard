#include "pch.h"
#include "Row.h"
#include "Row.g.cpp"

#include <cwctype>
#include <string>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace winrt::NeuralGuard::implementation
{
    // Design tokens, baked in as ARGB. We build brushes directly instead of
    // looking up NG.Brush.Verdict.* from the app resources because those live in
    // a *merged* dictionary, and ResourceDictionary.HasKey() does not search
    // merged dictionaries - so a runtime lookup always missed and every pill came
    // back transparent. The colors mirror Themes/NeuralGuardColors.xaml exactly.
    static Brush Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ Windows::UI::Color{ a, r, g, b } };
    }
    static Brush Green()      { return Argb(0xFF, 0x00, 0xFF, 0x88); }
    static Brush GreenDim()   { return Argb(0x22, 0x00, 0xFF, 0x88); }
    static Brush Cyan()       { return Argb(0xFF, 0x00, 0xE5, 0xFF); }
    static Brush CyanDim()    { return Argb(0x22, 0x00, 0xE5, 0xFF); }
    static Brush Red()        { return Argb(0xFF, 0xFF, 0x33, 0x66); }
    static Brush RedDim()     { return Argb(0x22, 0xFF, 0x33, 0x66); }
    static Brush Amber()      { return Argb(0xFF, 0xFF, 0xAA, 0x00); }
    static Brush AmberDim()   { return Argb(0x22, 0xFF, 0xAA, 0x00); }
    static Brush TextPrimary(){ return Argb(0xFF, 0xE8, 0xEC, 0xF4); }
    static Brush TextMuted()  { return Argb(0x55, 0xE8, 0xEC, 0xF4); }
    static Brush Clear()      { return Argb(0x00, 0x00, 0x00, 0x00); }

    // Cell text -> pill palette. Substring, case-insensitive; tokens never collide.
    // key: 0 none, 1 green, 2 cyan, 3 red, 4 amber.
    static int KeyFor(std::wstring const& upper)
    {
        auto has = [&](std::wstring_view s) { return upper.find(s) != std::wstring::npos; };
        if (has(L"CAPALLOW"))                                return 2;  // before ALLOW
        if (has(L"MALICIOUS"))                               return 3;
        if (has(L"ALLOW"))                                   return 1;  // ALLOW / auto-allow
        if (has(L"BLOCK") || has(L"DROP") || has(L"DEMOTE")) return 3;  // block/DROP/DEMOTED/demote
        if (has(L"MONITOR") || has(L"REVIEW"))               return 4;
        if (has(L"PERMIT"))                                  return 1;  // permitted / permit
        if (has(L"BENIGN"))                                  return 1;
        return 0;
    }

    // Pill fg/bg/border for one cell. Non-token cells: primary-text fg, transparent
    // fill+border, so the badge disappears and the text just reads plainly.
    static void PillBrushes(hstring const& text, Brush& fg, Brush& bg, Brush& bd)
    {
        std::wstring upper{ text };
        for (auto& c : upper) c = (wchar_t)std::towupper((wint_t)c);
        switch (KeyFor(upper))
        {
        case 1: fg = Green(); bg = GreenDim(); bd = Green(); return;
        case 2: fg = Cyan();  bg = CyanDim();  bd = Cyan();  return;
        case 3: fg = Red();   bg = RedDim();   bd = Red();   return;
        case 4: fg = Amber(); bg = AmberDim(); bd = Amber(); return;
        default: fg = TextPrimary(); bg = Clear(); bd = Clear(); return;
        }
    }

    Row::Row(int64_t id, hstring const& c0, hstring const& c1, hstring const& c2,
             hstring const& c3, hstring const& c4)
        : m_id(id), m_c0(c0), m_c1(c1), m_c2(c2), m_c3(c3), m_c4(c4)
    {
        PillBrushes(c0, m_c0Fg, m_c0Bg, m_c0Bd);
        PillBrushes(c1, m_c1Fg, m_c1Bg, m_c1Bd);
        PillBrushes(c2, m_c2Fg, m_c2Bg, m_c2Bd);

        // Per-app Blocked count (col 1): red when > 0, else muted.
        {
            int n = 0; try { n = std::stoi(std::wstring{ c1 }); } catch (...) {}
            m_c1Blocked = n > 0 ? Red() : TextMuted();
        }
        // Flows anomaly (col 3): amber when negative, else default text.
        {
            double v = 0; try { v = std::stod(std::wstring{ c3 }); } catch (...) {}
            m_c3Anom = v < 0 ? Amber() : TextPrimary();
        }
        // Flows P(malicious) (col 4): red at/above 0.5, else default text.
        {
            double v = 0; try { v = std::stod(std::wstring{ c4 }); } catch (...) {}
            m_c4Mal = v >= 0.5 ? Red() : TextPrimary();
        }
    }
}
