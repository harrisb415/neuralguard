#include "pch.h"
#include "InsightsView.xaml.h"
#if __has_include("InsightsView.g.cpp")
#include "InsightsView.g.cpp"
#endif

#include "Db.h"
#include "MainWindow.Shared.h"   // U8

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace winrt::NeuralGuard::implementation
{
    // Brushes baked from literal ARGB, not looked up from resources: the NG.Brush.*
    // live in a *merged* dictionary and code-side lookup misses them (same reason
    // Row.cpp bakes its pills). XAML ThemeResource still resolves fine, so only the
    // mode pill (set here) needs this.
    static Brush InsArgb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
    {
        return SolidColorBrush{ winrt::Windows::UI::Color{ a, r, g, b } };
    }
    static std::string InsCommas(long long n)
    {
        std::string s = std::to_string(n), out;
        int c = 0;
        for (int i = (int)s.size() - 1; i >= 0; --i) {
            out.push_back(s[i]);
            if (++c % 3 == 0 && i > 0) out.push_back(',');
        }
        std::reverse(out.begin(), out.end());
        return out;
    }
    static std::string InsFriendly(const std::string& iso)   // 2026-07-16T19:03:04Z -> Jul 16, 2026  19:03
    {
        if (iso.size() < 16) return iso;
        static const char* mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
        int y = atoi(iso.substr(0, 4).c_str()), m = atoi(iso.substr(5, 2).c_str()), d = atoi(iso.substr(8, 2).c_str());
        if (m < 1 || m > 12) return iso;
        char buf[64];
        snprintf(buf, sizeof buf, "%s %d, %d  %s UTC", mon[m - 1], d, y, iso.substr(11, 5).c_str());
        return buf;
    }

    // A rounded arc for `frac` (0..1) of the circle centered at (cx,cy) radius r,
    // starting at 12 o'clock, clockwise. Set on a ring Path's Data.
    static Geometry RingArc(double frac, double cx, double cy, double r)
    {
        if (frac <= 0.0) return nullptr;
        if (frac >= 1.0) frac = 0.9999;   // a full 360 sweep degenerates (start == end)
        const double PI = 3.14159265358979323846;
        double a0 = -PI / 2.0;            // 12 o'clock
        double a1 = a0 + frac * 2.0 * PI; // clockwise
        Point p0{ (float)(cx + r * std::cos(a0)), (float)(cy + r * std::sin(a0)) };
        Point p1{ (float)(cx + r * std::cos(a1)), (float)(cy + r * std::sin(a1)) };
        ArcSegment arc;
        arc.Point(p1);
        arc.Size(Size{ (float)r, (float)r });
        arc.SweepDirection(SweepDirection::Clockwise);
        arc.IsLargeArc(frac > 0.5);
        PathFigure fig;
        fig.StartPoint(p0);
        fig.IsClosed(false);
        fig.Segments().Append(arc);
        PathGeometry geo;
        geo.Figures().Append(fig);
        return geo;
    }

    void InsightsView::OnEditThresholds(IInspectable const&, RoutedEventArgs const&) { if (navigate_) navigate_(L"settings"); }
    void InsightsView::OnViewFlags(IInspectable const&, RoutedEventArgs const&) { if (navigate_) navigate_(L"flags"); }
    void InsightsView::OnViewFlows(IInspectable const&, RoutedEventArgs const&) { if (navigate_) navigate_(L"flows"); }

    // The learning/ML summary. Every number is advisory and read-only; nothing is
    // enforced from this view.
    void InsightsView::Refresh(std::string const& dbPath)
    {
        ng::Db d;
        if (!d.open(dbPath.c_str())) return;
        auto meta = [&](const char* k, const char* dflt) -> std::string {
            std::string v = d.scalar(("SELECT v FROM meta WHERE k='" + std::string(k) + "'").c_str());
            return v.empty() ? std::string(dflt) : v;
        };

        // --- Status: ML mode pill + thresholds ---
        std::string mode = meta("ml_mode", "shadow");
        std::string malThr = meta("ml_malicious_threshold", "0.9");
        std::string anomThr = meta("ml_anomaly_threshold", "-0.15");
        double malThrN = atof(malThr.c_str()), anomThrN = atof(anomThr.c_str());

        hstring modeLabel = mode == "active" ? L"ACTIVE" : mode == "off" ? L"OFF" : L"SHADOW";
        Brush modeColor = mode == "active" ? InsArgb(0xFF, 0x24, 0xB3, 0x57)
                        : mode == "off"    ? InsArgb(0xFF, 0x88, 0x92, 0xA0)
                                           : InsArgb(0xFF, 0x00, 0xB5, 0xD6);
        auto mc = modeColor.as<SolidColorBrush>().Color();
        InsModePillText().Text(modeLabel);
        InsModePillText().Foreground(modeColor);
        InsModePill().BorderBrush(modeColor);
        InsModePill().Background(InsArgb(0x22, mc.R, mc.G, mc.B));
        InsModeText().Text(modeLabel);
        InsModeText().Foreground(modeColor);
        std::string since = meta("ml_mode_since", "");
        InsModeSince().Text(since.empty() ? hstring(L"\x2014") : U8(InsFriendly(since)));
        InsThreshMal().Text(U8("\xE2\x89\xA5 " + malThr));    // >= (U+2265, UTF-8)
        InsThreshAnom().Text(U8("\xE2\x89\xA4 " + anomThr));  // <= (U+2264, UTF-8)

        // --- Habit model ---
        d.each("SELECT (SELECT count(*) FROM habits),(SELECT count(DISTINCT process_label) FROM habits),"
               "(SELECT count(DISTINCT dest) FROM habits),"
               "(SELECT count(*) FROM habits WHERE first_seen >= strftime('%Y-%m-%dT%H:%M:%SZ','now','-7 days'));",
               [&](sqlite3_stmt* s) {
            InsHabits().Text(U8(InsCommas(sqlite3_column_int64(s, 0))));
            InsApps().Text(U8(InsCommas(sqlite3_column_int64(s, 1))));
            InsDests().Text(U8(InsCommas(sqlite3_column_int64(s, 2))));
            InsNew7().Text(U8(InsCommas(sqlite3_column_int64(s, 3))));
        });

        // --- Scoring activity ---
        long long total = 0, scored = 0, demote = 0, review = 0;
        d.each("SELECT count(*), SUM(anomaly_score IS NOT NULL OR malicious_score IS NOT NULL) FROM flow_features;",
               [&](sqlite3_stmt* s) {
            total = sqlite3_column_int64(s, 0);
            scored = sqlite3_column_int64(s, 1);
        });
        {
            char q[160];
            snprintf(q, sizeof q, "SELECT count(*) FROM flow_features WHERE malicious_score >= %.6f;", malThrN);
            d.each(q, [&](sqlite3_stmt* s) { demote = sqlite3_column_int64(s, 0); });
            snprintf(q, sizeof q, "SELECT count(*) FROM flow_features WHERE anomaly_score <= %.6f;", anomThrN);
            d.each(q, [&](sqlite3_stmt* s) { review = sqlite3_column_int64(s, 0); });
        }
        double scoredFrac = total ? (double)scored / (double)total : 0.0;
        InsFlowsTotal().Text(U8(InsCommas(total)));
        char pct[16]; snprintf(pct, sizeof pct, "%.0f%%", scoredFrac * 100.0);
        InsScoredPct().Text(U8(pct));
        ScoreRingArc().Data(RingArc(scoredFrac, 56, 56, 50));
        InsScDemote().Text(U8(InsCommas(demote)));
        InsScReview().Text(U8(InsCommas(review)));
        InsScUnscored().Text(U8(InsCommas(total - scored)));

        // --- Demotions & review flags ---
        d.each("SELECT SUM(kind='demote'), SUM(kind='review') FROM ml_flags;", [&](sqlite3_stmt* s) {
            InsDemotions().Text(U8(InsCommas(sqlite3_column_int64(s, 0))));
            InsReviewFlags().Text(U8(InsCommas(sqlite3_column_int64(s, 1))));
        });

        // --- Top flagged right now: recent scored flows, worst first ---
        Brush redB = InsArgb(0xFF, 0xE1, 0x20, 0x2D), amberB = InsArgb(0xFF, 0xC8, 0x7A, 0x00);
        InsTopFlagged().Children().Clear();
        int shown = 0;
        d.each("SELECT ts_utc, COALESCE(process_label,''), COALESCE(dest,'')||':'||remote_port,"
               " anomaly_score, malicious_score FROM flow_features"
               " WHERE anomaly_score IS NOT NULL OR malicious_score IS NOT NULL"
               " ORDER BY COALESCE(malicious_score,0) DESC, COALESCE(anomaly_score,999) ASC, id DESC LIMIT 6;",
               [&](sqlite3_stmt* s) {
            std::string ts = ng::Db::ColText(s, 0);
            hstring tm = U8(ts.size() >= 19 ? ts.substr(11, 8) : ts);
            hstring app = U8(ng::Db::ColText(s, 1));
            hstring dest = U8(ng::Db::ColText(s, 2));
            bool hasA = sqlite3_column_type(s, 3) != SQLITE_NULL;
            bool hasM = sqlite3_column_type(s, 4) != SQLITE_NULL;
            double a = hasA ? sqlite3_column_double(s, 3) : 0.0;
            double m = hasM ? sqlite3_column_double(s, 4) : 0.0;
            hstring reason; Brush col{ nullptr }; char score[24];
            if (hasM && m >= malThrN) { reason = L"Malicious"; col = redB; snprintf(score, sizeof score, "%.2f", m); }
            else if (hasA && a <= anomThrN) { reason = L"Anomalous"; col = amberB; snprintf(score, sizeof score, "%+.3f", a); }
            else if (hasM) { reason = L"Watch"; snprintf(score, sizeof score, "%.2f", m); }
            else { reason = L"Watch"; snprintf(score, sizeof score, "%+.3f", a); }

            Grid g;
            for (double w : { 88.0, -1.0, 180.0, 110.0, 64.0 }) {
                ColumnDefinition cd;
                cd.Width(w < 0 ? GridLength{ 1, GridUnitType::Star } : GridLength{ w, GridUnitType::Pixel });
                g.ColumnDefinitions().Append(cd);
            }
            g.Padding(Thickness{ 0, 7, 0, 7 });
            auto cell = [&](hstring text, int col_, bool right, Brush fg) {
                TextBlock t;
                t.Text(text);
                t.FontSize(13);
                t.TextTrimming(TextTrimming::CharacterEllipsis);
                if (right) t.TextAlignment(TextAlignment::Right);
                if (fg) t.Foreground(fg);
                Grid::SetColumn(t, col_);
                g.Children().Append(t);
            };
            cell(tm, 0, false, nullptr);
            cell(app, 1, false, nullptr);
            cell(dest, 2, false, nullptr);
            cell(reason, 3, false, col);
            cell(U8(score), 4, true, col);
            InsTopFlagged().Children().Append(g);
            ++shown;
        });
        if (!shown) {
            TextBlock t;
            t.Text(L"No scored flows yet.");
            t.Foreground(InsArgb(0xFF, 0x88, 0x92, 0xA0));
            t.Margin(Thickness{ 0, 6, 0, 6 });
            InsTopFlagged().Children().Append(t);
        }

        // --- Feedback loop ---
        d.each("SELECT count(*), SUM(CASE WHEN label=1 THEN 1 ELSE 0 END) FROM feedback_labels;", [&](sqlite3_stmt* s) {
            long long fbTotal = sqlite3_column_int64(s, 0);
            long long fbMal = sqlite3_column_int64(s, 1);
            long long fbBen = fbTotal - fbMal;
            InsFbTotal().Text(U8(InsCommas(fbTotal)));
            auto withPct = [&](long long n) {
                if (!fbTotal) return std::to_string(n);
                char b[48]; snprintf(b, sizeof b, "%lld (%.0f%%)", n, 100.0 * (double)n / (double)fbTotal);
                return std::string(b);
            };
            InsFbBenign().Text(U8(withPct(fbBen)));
            InsFbMalicious().Text(U8(withPct(fbMal)));
            FeedbackRingArc().Data(RingArc(fbTotal ? (double)fbBen / (double)fbTotal : 0.0, 48, 48, 42));
        });
    }
}
