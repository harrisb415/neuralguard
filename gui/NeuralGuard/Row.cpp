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
    // back transparent.
    static Brush Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ Windows::UI::Color{ a, r, g, b } };
    }

    // Set by MainWindow::SyncThemeDependents. See Row::SetLightTheme.
    static bool g_light = false;
    void Row::SetLightTheme(bool light) { g_light = light; }

    // --- Pills -------------------------------------------------------------
    // The two themes use genuinely different pill styles, because what makes a
    // badge readable differs with the background:
    //
    //   Dark  - the original neon look: dim tint fill, bright neon text, matching
    //           border. Neon glows against near-black; this is the designed look.
    //   Light - solid saturated fill, white text, no border. The neon style washes
    //           out on white (neon green text on a 13%-alpha tint is barely
    //           legible), so light carries its contrast in the fill instead.
    //
    // Same semantics either way - green=allow, blue/cyan=capallow, red=block,
    // orange=warning, yellow=monitor - so a verdict reads the same in both.
    static Brush NeonGreen()  { return Argb(0xFF, 0x00, 0xFF, 0x88); }
    static Brush NeonCyan()   { return Argb(0xFF, 0x00, 0xE5, 0xFF); }
    static Brush NeonRed()    { return Argb(0xFF, 0xFF, 0x33, 0x66); }
    static Brush NeonOrange() { return Argb(0xFF, 0xFF, 0x8A, 0x3D); }
    static Brush NeonAmber()  { return Argb(0xFF, 0xFF, 0xAA, 0x00); }
    static Brush Dim(Brush const& b)   // same hue as the neon, at tint alpha
    {
        auto c = b.as<SolidColorBrush>().Color();
        return Argb(0x22, c.R, c.G, c.B);
    }

    static Brush PillGreen()  { return Argb(0xFF, 0x24, 0xB3, 0x57); }  // allow / permitted / benign
    static Brush PillBlue()   { return Argb(0xFF, 0x1E, 0x9C, 0xF0); }  // capallow
    static Brush PillRed()    { return Argb(0xFF, 0xE1, 0x20, 0x2D); }  // drop / block
    static Brush PillOrange() { return Argb(0xFF, 0xF1, 0x74, 0x1F); }  // malicious / demoted
    static Brush PillYellow() { return Argb(0xFF, 0xF5, 0xC5, 0x18); }  // monitor / review
    static Brush OnPill()     { return Argb(0xFF, 0xFF, 0xFF, 0xFF); }
    // Yellow is the one fill white can't sit on legibly (~1.9:1), so it alone
    // takes dark text (~11:1) instead of being darkened into a muddy brown.
    static Brush OnYellow()   { return Argb(0xFF, 0x1A, 0x14, 0x00); }
    static Brush Clear()      { return Argb(0x00, 0x00, 0x00, 0x00); }

    // --- Plain text (not pills) --------------------------------------------
    // These DO depend on the theme: light-theme text on a light background would
    // be invisible, which is exactly what the hardcoded dark values did.
    static Brush TextPrimary() { return g_light ? Argb(0xFF, 0x11, 0x15, 0x1C) : Argb(0xFF, 0xE8, 0xEC, 0xF4); }
    static Brush TextMuted()   { return g_light ? Argb(0x6B, 0x11, 0x15, 0x1C) : Argb(0x55, 0xE8, 0xEC, 0xF4); }
    static Brush DangerText()  { return g_light ? Argb(0xFF, 0xC0, 0x00, 0x30) : Argb(0xFF, 0xFF, 0x33, 0x66); }
    static Brush WarnText()    { return g_light ? Argb(0xFF, 0x9A, 0x5B, 0x00) : Argb(0xFF, 0xFF, 0xAA, 0x00); }

    // Cell text -> pill palette. Substring, case-insensitive; tokens never collide.
    // key: 0 none, 1 green, 2 blue, 3 red, 4 yellow, 5 orange.
    static int KeyFor(std::wstring const& upper)
    {
        auto has = [&](std::wstring_view s) { return upper.find(s) != std::wstring::npos; };
        if (has(L"CAPALLOW"))                  return 2;  // before ALLOW
        // Orange, not red: malicious is a score/label and DEMOTED is a loss of
        // trust - both are warnings, distinct from an actual block.
        if (has(L"MALICIOUS") || has(L"DEMOTE")) return 5;
        if (has(L"ALLOW"))                     return 1;  // ALLOW / auto-allow
        if (has(L"BLOCK") || has(L"DROP"))     return 3;  // block / DROP / CAPDROP
        if (has(L"MONITOR") || has(L"REVIEW")) return 4;
        if (has(L"PERMIT"))                    return 1;  // permitted / permit
        if (has(L"BENIGN"))                    return 1;
        return 0;
    }

    // Pill fg/bg/border for one cell.
    //   Light: border matches the fill, so the badge reads as one solid shape.
    //   Dark:  neon text on a dim tint of the same hue, with a neon border.
    // Non-token cells: plain text fg, transparent fill, so the badge disappears
    // and the text just reads normally.
    static void PillBrushes(hstring const& text, Brush& fg, Brush& bg, Brush& bd)
    {
        std::wstring upper{ text };
        for (auto& c : upper) c = (wchar_t)std::towupper((wint_t)c);
        const int key = KeyFor(upper);

        if (!g_light)   // dark: the original neon badges
        {
            Brush neon{ nullptr };
            switch (key)
            {
            case 1: neon = NeonGreen();  break;
            case 2: neon = NeonCyan();   break;
            case 3: neon = NeonRed();    break;
            case 4: neon = NeonAmber();  break;
            case 5: neon = NeonOrange(); break;
            default: fg = TextPrimary(); bg = Clear(); bd = Clear(); return;
            }
            fg = neon; bg = Dim(neon); bd = neon;
            return;
        }

        switch (key)   // light: solid badges
        {
        case 1: fg = OnPill();   bg = PillGreen();  bd = PillGreen();  return;
        case 2: fg = OnPill();   bg = PillBlue();   bd = PillBlue();   return;
        case 3: fg = OnPill();   bg = PillRed();    bd = PillRed();    return;
        case 4: fg = OnYellow(); bg = PillYellow(); bd = PillYellow(); return;
        case 5: fg = OnPill();   bg = PillOrange(); bd = PillOrange(); return;
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

        // These three are coloured TEXT, not pills, so they use the theme-aware
        // readable variants - the pill fills would be unreadable as bare text on
        // a light background.

        // Per-app Blocked count (col 1): red when > 0, else muted.
        {
            int n = 0; try { n = std::stoi(std::wstring{ c1 }); } catch (...) {}
            m_c1Blocked = n > 0 ? DangerText() : TextMuted();
        }
        // Flows anomaly (col 3): amber when negative, else default text.
        {
            double v = 0; try { v = std::stod(std::wstring{ c3 }); } catch (...) {}
            m_c3Anom = v < 0 ? WarnText() : TextPrimary();
        }
        // Flows P(malicious) (col 4): red at/above 0.5, else default text.
        {
            double v = 0; try { v = std::stod(std::wstring{ c4 }); } catch (...) {}
            m_c4Mal = v >= 0.5 ? DangerText() : TextPrimary();
        }
    }
}
