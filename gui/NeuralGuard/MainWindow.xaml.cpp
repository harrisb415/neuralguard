#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Db.h"
#include "Row.h"
#include "ColWidths.h"
#include "core/updater.h"              // shared in-app updater (compiled from src/core)
#include "core/version.h"              // NG_VERSION, for the About section
#include "SemBrush.h"                  // SemBrush::SetLightTheme (converter can't see ActualTheme)
#include "core/cmd.h"                  // command pipe to the running service
#include "Tray.h"                      // the tray icon, formerly ngtray.exe

#include <microsoft.ui.xaml.window.h>   // IWindowNative -> HWND for the file dialogs

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>
#include <thread>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Input;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace winrt::NeuralGuard::implementation
{
    static hstring U8(std::string const& s) { return to_hstring(std::string_view(s)); }

    static IInspectable MakeRow(int64_t id, hstring const& a, hstring const& b, hstring const& c,
                                hstring const& d, hstring const& e)
    {
        return winrt::make<Row>(id, a, b, c, d, e);
    }

    // A ListView's default template contains a ScrollViewer, but it isn't a named
    // part we can reach directly - only findable by walking the realized visual
    // tree. Used to save/restore scroll position across a refresh, since resetting
    // ItemsSource (RefreshCurrent's wholesale-rebuild approach - same reason it
    // already re-finds the selected row by id) snaps the view to the top otherwise.
    static ScrollViewer FindScrollViewer(DependencyObject const& root)
    {
        int n = VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < n; ++i)
        {
            auto child = VisualTreeHelper::GetChild(root, i);
            if (auto sv = child.try_as<ScrollViewer>()) return sv;
            if (auto found = FindScrollViewer(child)) return found;
        }
        return nullptr;
    }

    static int TagInt(IInspectable const& sender)
    {
        if (auto fe = sender.try_as<FrameworkElement>())
        {
            std::string t = to_string(unbox_value_or<hstring>(fe.Tag(), L"0"));
            return t.empty() ? 0 : atoi(t.c_str());
        }
        return 0;
    }

    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"NeuralGuard");
        SystemBackdrop(MicaBackdrop{});

        // Integrated neon title bar: draw our own bar in the caption area and make
        // its empty space draggable. Caption buttons blend with the dark surface.
        ExtendsContentIntoTitleBar(true);
        SetTitleBar(DragRegion());
        // Caption colours are applied by ApplyCaptionColors (called from
        // SyncThemeDependents) - they're an AppWindow API, not XAML resources,
        // so they can't follow ThemeResource and must be re-set per theme.

        // Window/taskbar icon: AppWindow doesn't inherit the exe's embedded .rc
        // icon on its own, so point it at the loose copy deployed next to the
        // exe (NeuralGuard.rc covers Explorer/shortcut icons; this covers the
        // live window, taskbar, and Alt-Tab).
        {
            wchar_t exePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring dir(exePath);
            size_t p = dir.find_last_of(L"\\/");
            if (p != std::wstring::npos) dir = dir.substr(0, p);
            AppWindow().SetIcon(dir + L"\\NeuralGuard.ico");
        }

        colW_ = Application::Current().Resources().Lookup(box_value(L"ColW")).as<NeuralGuard::ColWidths>();

        // Apply the persisted theme before first paint. Defaults to dark - the
        // designed look - so nothing changes for anyone who never touches the
        // setting; App.xaml no longer forces Dark app-wide (see the note there).
        ApplyTheme(MetaGet("theme", "dark"));

        StartTray();

        timer_ = DispatcherTimer{};
        timer_.Interval(std::chrono::seconds(1));
        timer_.Tick({ this, &MainWindow::OnTick });
        timer_.Start();

        toastTimer_ = DispatcherTimer{};
        toastTimer_.Interval(std::chrono::seconds(6));
        toastTimer_.Tick([this](IInspectable const&, IInspectable const&) {
            toastTimer_.Stop();
            Toast().IsOpen(false);
        });

        UpdateMode();
        ShowView(L"live");
        NavList().SelectedIndex(0);   // highlight Live in the sidebar
    }

    winrt::Microsoft::UI::Xaml::Controls::TextBlock MainWindow::HdrBlock(int i)
    {
        switch (i) { case 0: return H0(); case 1: return H1(); case 2: return H2(); case 3: return H3(); default: return H4(); }
    }
    double MainWindow::GetW(int i)
    {
        switch (i) { case 0: return colW_.W0().Value; case 1: return colW_.W1().Value; case 2: return colW_.W2().Value;
                     case 3: return colW_.W3().Value; default: return colW_.W4().Value; }
    }
    void MainWindow::SetW(int i, double px)
    {
        GridLength gl{ px, GridUnitType::Pixel };
        // Keep the value so freshly-realized rows pick it up via their one-time binding.
        switch (i) { case 0: colW_.W0(gl); break; case 1: colW_.W1(gl); break; case 2: colW_.W2(gl); break;
                     case 3: colW_.W3(gl); break; default: colW_.W4(gl); break; }
        // ColumnDefinition.Width bindings don't update live in WinUI, so set widths directly:
        // the header column by name, and every already-realized row via its container.
        switch (i) { case 0: HCol0().Width(gl); break; case 1: HCol1().Width(gl); break;
                     case 2: HCol2().Width(gl); break; case 3: HCol3().Width(gl); break;
                     default: HCol4().Width(gl); break; }
        uint32_t n = DataList().Items() ? DataList().Items().Size() : 0;
        for (uint32_t k = 0; k < n; ++k)
        {
            auto c = DataList().ContainerFromIndex(k).try_as<ListViewItem>();
            if (!c) continue;
            if (auto g = c.ContentTemplateRoot().try_as<Grid>())
                if ((uint32_t)i < g.ColumnDefinitions().Size())
                    g.ColumnDefinitions().GetAt(i).Width(gl);
        }
    }
    void MainWindow::ApplyHeaderText()
    {
        for (int i = 0; i < 5; ++i)
        {
            std::wstring u{ baseHdr_[i] };
            for (auto& c : u) c = (wchar_t)::towupper(c);   // uppercase column labels
            hstring t{ u };
            if (i == sortCol_ && !u.empty())
            {
                wchar_t arrow[2] = { (wchar_t)(sortAsc_ ? 0x25B2 : 0x25BC), 0 };
                t = t + L"  " + hstring(arrow);
            }
            HdrBlock(i).Text(t);
        }
    }
    void MainWindow::OnHeaderTap(IInspectable const& sender, TappedRoutedEventArgs const&)
    {
        int col = TagInt(sender);
        if (baseHdr_[col].empty()) return;   // blank column - not sortable
        if (col == sortCol_) sortAsc_ = !sortAsc_; else { sortCol_ = col; sortAsc_ = true; }
        ApplyHeaderText();
        RefreshCurrent();
    }
    // Manual column resize measured against ContentRoot (which does NOT move as columns
    // resize), so there's no feedback loop. Handled() stops the ListView/ScrollViewer
    // from stealing the pointer. Each column has an independent pixel width.
    void MainWindow::OnGripPressed(IInspectable const& sender, PointerRoutedEventArgs const& e)
    {
        resizeCol_ = TagInt(sender);
        dragStartX_ = e.GetCurrentPoint(ContentRoot()).Position().X;
        dragStartW_ = GetW(resizeCol_);
        if (auto b = sender.try_as<UIElement>()) b.CapturePointer(e.Pointer());
        e.Handled(true);
    }
    void MainWindow::OnGripMoved(IInspectable const&, PointerRoutedEventArgs const& e)
    {
        if (resizeCol_ < 0) return;
        double x = e.GetCurrentPoint(ContentRoot()).Position().X;
        double w = dragStartW_ + (x - dragStartX_);
        if (w < 44) w = 44;
        if (w > 1200) w = 1200;
        SetW(resizeCol_, w);
        e.Handled(true);
    }
    void MainWindow::OnGripReleased(IInspectable const& sender, PointerRoutedEventArgs const& e)
    {
        resizeCol_ = -1;
        if (auto b = sender.try_as<UIElement>()) b.ReleasePointerCapture(e.Pointer());
    }
    // Each row's Grid is rebuilt whenever the ItemsSource is replaced (every refresh),
    // so re-apply the current column widths as each container is realized. This is what
    // makes a resize survive the periodic refresh instead of snapping back to the
    // template's default widths.
    void MainWindow::OnContainerChanging(ListViewBase const&, ContainerContentChangingEventArgs const& args)
    {
        if (args.InRecycleQueue()) return;
        auto container = args.ItemContainer();
        if (!container) return;
        if (auto g = container.ContentTemplateRoot().try_as<Grid>())
        {
            auto cols = g.ColumnDefinitions();
            if (cols.Size() >= 5)
            {
                // Keep each row's columns in lockstep with the (per-view) header.
                cols.GetAt(0).Width(HCol0().Width()); cols.GetAt(1).Width(HCol1().Width());
                cols.GetAt(2).Width(HCol2().Width()); cols.GetAt(3).Width(HCol3().Width());
                cols.GetAt(4).Width(HCol4().Width());
            }
        }
    }

    // Per-view column widths for the header (rows follow via OnContainerChanging).
    // A negative value means a star (fills remaining space) column.
    void MainWindow::SetCols(double a, double b, double c, double d, double e)
    {
        auto gl = [](double v) {
            return v < 0 ? GridLength{ 1, GridUnitType::Star } : GridLength{ v, GridUnitType::Pixel };
        };
        HCol0().Width(gl(a)); HCol1().Width(gl(b)); HCol2().Width(gl(c));
        HCol3().Width(gl(d)); HCol4().Width(gl(e));
    }

    void MainWindow::SetHeaders(hstring const& h0, hstring const& h1, hstring const& h2,
                                hstring const& h3, hstring const& h4)
    {
        baseHdr_[0] = h0; baseHdr_[1] = h1; baseHdr_[2] = h2; baseHdr_[3] = h3; baseHdr_[4] = h4;
        ApplyHeaderText();
    }

    // The dashboard always lives in a "dashboard\" subfolder of the install
    // root (ngd.exe, ngctl.exe, and ngpolicy.db live one level up - see
    // ngtray's ExeDir()+"\\dashboard" convention). Derive that root from our
    // own module path instead of assuming a fixed %USERPROFILE%\NeuralGuard
    // location, so the app works wherever it's installed.
    std::wstring MainWindow::NgDir()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir(path);
        size_t p = dir.find_last_of(L"\\/");
        dir = (p == std::wstring::npos) ? L"." : dir.substr(0, p);          // ...\dashboard
        size_t q = dir.find_last_of(L"\\/");
        return (q == std::wstring::npos) ? dir : dir.substr(0, q);          // install root
    }

    std::string MainWindow::DbPathU8()
    {
        std::wstring w = NgDir() + L"\\ngpolicy.db";
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        return s;
    }

    void MainWindow::Log(hstring const& line)
    {
        Notify(line, InfoBarSeverity::Informational);
    }
    void MainWindow::Notify(hstring const& message, InfoBarSeverity severity)
    {
        hstring title = L"NeuralGuard";
        switch (severity)
        {
            case InfoBarSeverity::Success: title = L"Success";  break;
            case InfoBarSeverity::Warning: title = L"Heads up";  break;
            case InfoBarSeverity::Error:   title = L"Error";     break;
            default:                       title = L"NeuralGuard"; break;
        }
        Toast().Title(title);
        Toast().Severity(severity);
        Toast().Message(message);
        Toast().IsOpen(true);
        toastTimer_.Stop();
        toastTimer_.Start();   // restart the auto-dismiss countdown
    }

    void MainWindow::UpdateMode()
    {
        ng::Db d;
        std::string mode = "idle";
        if (d.open(DbPathU8().c_str()))
        {
            std::string m = d.scalar("SELECT v FROM meta WHERE k='mode';");
            if (!m.empty()) mode = m;
        }
        ModeText().Text(to_hstring(mode));
        ngtray::SetMode(mode);   // one poller drives both the status bar and the tray icon
    }

    // Own the tray icon in this process. It used to be a whole separate executable
    // (ngtray.exe) purely because a LocalSystem service can't show UI - but that
    // never meant the frontend had to be two programs. Two of them meant two mode
    // pollers, two panic paths that drifted, and a tray whose Status could only
    // shell out to a cmd.exe window, having no UI of its own to render into.
    void MainWindow::StartTray()
    {
        ngtray::Callbacks cb;
        cb.openDashboard = [this] { ShowFromTray(); };
        cb.quit = [this] { ExitApp(); };

        // Status and Panic now render in-app instead of spawning a console.
        cb.showStatus = [this] {
            bool ok = false;
            const std::string reply = ng::CmdSend("STATUS", &ok);
            ShowFromTray();
            Notify(ok ? U8(reply) : hstring{ L"Service not reachable (not installed or not running)." },
                   ok ? InfoBarSeverity::Informational : InfoBarSeverity::Warning);
        };
        cb.panic = [this] { OnPanic(nullptr, nullptr); };

        // Runs on the pipe thread, so it must not touch XAML. MessageBoxW is
        // thread-safe and is what the block-notify-retry flow has always used.
        cb.prompt = [](std::wstring const& app, std::wstring const& dest, std::wstring const& port) -> char {
            std::wstring text = app + L"\n\nwants to connect to  " + dest + L":" + port +
                L"\n\nYes = Always allow this app on this port"
                L"\nNo = Allow once"
                L"\nCancel = Block";
            int r = MessageBoxW(nullptr, text.c_str(), L"NeuralGuard",
                                MB_YESNOCANCEL | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
            return (r == IDYES) ? 'A' : (r == IDNO) ? 'O' : 'B';
        };

        ngtray::Start(GetModuleHandleW(nullptr), cb);

        // Closing the window hides to tray. The tray icon is the app's real
        // lifetime now: an installed service with no frontend running would leave
        // nothing to answer ngd's prompts or show you what it's doing.
        AppWindow().Closing([this](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
            args.Cancel(true);
            AppWindow().Hide();
        });
    }

    void MainWindow::ShowFromTray()
    {
        AppWindow().Show();
        HWND h = WindowHandle();
        if (h) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
    }

    void MainWindow::ExitApp()
    {
        ngtray::Stop();          // pull the icon before the process goes, or it ghosts
        Application::Current().Exit();
    }

    void MainWindow::RefreshCurrent()
    {
        struct RowData { int64_t id; hstring c[5]; };
        std::vector<RowData> rows;
        ng::Db d;
        bool ok = d.open(DbPathU8().c_str());

        if (curView_ == L"live")
        {
            SetHeaders(L"Time", L"Verdict", L"Application", L"Destination", L"Port");
            std::string sql =
                "SELECT fe.id, fe.ts_utc, fe.verdict,"
                " COALESCE(pi.signer, pi.image_path, fe.image_path),"
                " COALESCE(fe.remote_domain, fe.remote_addr), fe.remote_port"
                " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id=pi.id "
                "ORDER BY fe.id DESC LIMIT 300;";
            if (ok) d.each(sql.c_str(), [&](sqlite3_stmt* s) {
                std::string ts = ng::Db::ColText(s, 1);
                std::string tm = ts.size() >= 19 ? ts.substr(11, 8) : ts;
                rows.push_back({ sqlite3_column_int64(s, 0),
                                 { U8(tm), U8(ng::Db::ColText(s, 2)), U8(ng::Db::ColText(s, 3)),
                                   U8(ng::Db::ColText(s, 4)), to_hstring(sqlite3_column_int(s, 5)) } });
            });
        }
        else if (curView_ == L"habits")
        {
            SetHeaders(L"Count", L"Port", L"Application", L"Destination", L"");
            if (ok) d.each("SELECT process_label, dest, remote_port, round(count,1) FROM habits"
                           " ORDER BY count DESC LIMIT 1000;", [&](sqlite3_stmt* s) {
                rows.push_back({ 0, { to_hstring(sqlite3_column_double(s, 3)), to_hstring(sqlite3_column_int(s, 2)),
                                      U8(ng::Db::ColText(s, 0)), U8(ng::Db::ColText(s, 1)), L"" } });
            });
        }
        else if (curView_ == L"apps")
        {
            SetHeaders(L"Events", L"Blocked", L"Dests", L"Application", L"");
            // Reads the app_stats/app_dests rollup the daemon maintains, not the
            // raw flow_events - so this is O(#apps + #distinct-dests) and stays
            // fast no matter how large the event log grows. Same columns as before
            // (signer-grouped, distinct destinations). events/blocked and the
            // distinct-dest count are aggregated in SEPARATE CTEs and joined on the
            // app label - joining app_stats to app_dests directly would fan the
            // one-to-many out and multiply events by each app's dest count.
            // Unattributed (no image_id) events aren't apps and don't appear here
            // (they're still in Live).
            if (ok) d.each(
                "WITH stats AS ("
                "  SELECT COALESCE(pi.signer, pi.image_path, '(unknown)') app,"
                "         SUM(s.events) ev, SUM(s.blocked) bl"
                "  FROM app_stats s LEFT JOIN process_identity pi ON s.image_id = pi.id GROUP BY app),"
                " dests AS ("
                "  SELECT COALESCE(pi.signer, pi.image_path, '(unknown)') app,"
                "         COUNT(DISTINCT ad.remote_addr) dests"
                "  FROM app_dests ad LEFT JOIN process_identity pi ON ad.image_id = pi.id GROUP BY app)"
                " SELECT stats.app, COALESCE(dests.dests,0), stats.ev, stats.bl"
                " FROM stats LEFT JOIN dests ON stats.app = dests.app"
                " ORDER BY stats.ev DESC LIMIT 500;", [&](sqlite3_stmt* s) {
                rows.push_back({ 0, { to_hstring(sqlite3_column_int(s, 2)), to_hstring(sqlite3_column_int(s, 3)),
                                      to_hstring(sqlite3_column_int(s, 1)), U8(ng::Db::ColText(s, 0)), L"" } });
            });
        }
        else if (curView_ == L"rules")
        {
            SetHeaders(L"Action", L"Port", L"Info", L"Target (app / IP)", L"");
            if (ok) d.each("SELECT id, action, COALESCE(app_path, remote_addr, '(any)'),"
                           " COALESCE(remote_port,0), enabled, COALESCE(expires_epoch,0)"
                           " FROM rules ORDER BY id DESC;", [&](sqlite3_stmt* s) {
                int port = sqlite3_column_int(s, 3);
                std::string info = sqlite3_column_int(s, 4) ? "" : "disabled";
                if (sqlite3_column_double(s, 5) > 0) info += info.empty() ? "timed" : ", timed";
                rows.push_back({ sqlite3_column_int64(s, 0),
                                 { U8(ng::Db::ColText(s, 1)), port ? to_hstring(port) : hstring(L"any"),
                                   U8(info), U8(ng::Db::ColText(s, 2)), L"" } });
            });
        }
        else if (curView_ == L"flows")
        {
            // Phase 4: completed flows with their shadow-mode ML scores. Sortable
            // by clicking Anomaly (lower = more anomalous) or P(malicious).
            SetHeaders(L"Time", L"Application", L"Destination", L"Anomaly", L"P(malicious)");
            if (ok) d.each("SELECT ts_utc, COALESCE(process_label,''), COALESCE(dest,''), remote_port,"
                           " anomaly_score, malicious_score FROM flow_features ORDER BY id DESC LIMIT 300;",
                           [&](sqlite3_stmt* s) {
                std::string ts = ng::Db::ColText(s, 0);
                std::string tm = ts.size() >= 19 ? ts.substr(11, 8) : ts;
                std::string dest = ng::Db::ColText(s, 2) + ":" + std::to_string(sqlite3_column_int(s, 3));
                char anom[24] = "-", mal[24] = "-";
                if (sqlite3_column_type(s, 4) != SQLITE_NULL) snprintf(anom, sizeof anom, "%+.3f", sqlite3_column_double(s, 4));
                if (sqlite3_column_type(s, 5) != SQLITE_NULL) snprintf(mal, sizeof mal, "%.2f", sqlite3_column_double(s, 5));
                rows.push_back({ 0, { U8(tm), U8(ng::Db::ColText(s, 1)), U8(dest), U8(anom), U8(mal) } });
            });
        }
        else if (curView_ == L"flags")
        {
            // Phase 4d: active-mode demotions + anomaly review flags. Right-click a
            // row to remove it; "Clear all flags" is in the toolbar.
            SetHeaders(L"Time", L"Kind", L"Score", L"Application", L"Destination");
            if (ok) d.each("SELECT id, ts_utc, kind, printf('%.2f', score), COALESCE(process_label,''),"
                           " COALESCE(dest,'')||':'||remote_port FROM ml_flags ORDER BY id DESC LIMIT 500;",
                           [&](sqlite3_stmt* s) {
                std::string ts = ng::Db::ColText(s, 1);
                std::string tm = ts.size() >= 19 ? ts.substr(11, 8) : ts;
                rows.push_back({ sqlite3_column_int64(s, 0),
                                 { U8(tm), U8(ng::Db::ColText(s, 2)), U8(ng::Db::ColText(s, 3)),
                                   U8(ng::Db::ColText(s, 4)), U8(ng::Db::ColText(s, 5)) } });
            });
        }
        else if (curView_ == L"inbound")
        {
            // Inbound services WE blocked, for passive review. Inbound is never
            // prompted (a remote party must not be able to pop a dialog), so this
            // list is where the tray balloon sends you: right-click to allow.
            // C0 is the state, which TplGeneric renders as a pill - "blocked" hits
            // the Block palette (red), "ALLOWED" the Allow palette (green).
            SetHeaders(L"State", L"Port", L"Attempts", L"Application", L"Last peer");
            if (ok) d.each("SELECT id, allowed, local_port, attempts,"
                           " COALESCE(NULLIF(process_label,''), app_path), COALESCE(last_peer,'')"
                           " FROM inbound_blocked ORDER BY allowed, attempts DESC LIMIT 500;",
                           [&](sqlite3_stmt* s) {
                rows.push_back({ sqlite3_column_int64(s, 0),
                                 { sqlite3_column_int(s, 1) ? hstring{ L"ALLOWED" } : hstring{ L"blocked" },
                                   to_hstring(sqlite3_column_int(s, 2)),
                                   to_hstring(sqlite3_column_int(s, 3)),
                                   U8(ng::Db::ColText(s, 4)), U8(ng::Db::ColText(s, 5)) } });
            });
        }
        else if (curView_ == L"feedback")
        {
            // Phase 4e: prompt verdicts as training labels (read-only view).
            SetHeaders(L"Time", L"Decision", L"Label", L"Application", L"Destination");
            if (ok) d.each("SELECT ts_utc, decision, CASE label WHEN 1 THEN 'malicious' ELSE 'benign' END,"
                           " COALESCE(process_label,''), COALESCE(dest,'')||':'||remote_port"
                           " FROM feedback_labels ORDER BY id DESC LIMIT 500;", [&](sqlite3_stmt* s) {
                std::string ts = ng::Db::ColText(s, 0);
                std::string tm = ts.size() >= 19 ? ts.substr(11, 8) : ts;
                rows.push_back({ 0, { U8(tm), U8(ng::Db::ColText(s, 1)), U8(ng::Db::ColText(s, 2)),
                                      U8(ng::Db::ColText(s, 3)), U8(ng::Db::ColText(s, 4)) } });
            });
        }
        else if (curView_ == L"baseline")
        {
            // Phase 4d: the stable (app, port) permits enforce would install, with
            // each one's demote state. Right-click to distrust / re-trust an app.
            SetHeaders(L"Port", L"Conns", L"State", L"Application", L"Proto");
            if (ok) d.each(
                "SELECT fe.remote_port, COUNT(DISTINCT fe.local_port||'|'||fe.remote_addr) AS conns,"
                " EXISTS(SELECT 1 FROM ml_flags m WHERE m.kind='demote' AND m.app_path=pi.image_path"
                "   AND m.remote_port=fe.remote_port AND m.protocol=fe.protocol) AS demoted,"
                " pi.image_path, fe.protocol"
                " FROM flow_events fe JOIN process_identity pi ON fe.image_id=pi.id"
                " WHERE fe.remote_port>0 AND fe.remote_port<49152 AND pi.image_path LIKE '_:\\%'"
                "   AND fe.verdict IN ('ALLOW','CAPALLOW')"
                " GROUP BY pi.image_path, fe.protocol, fe.remote_port HAVING conns>=3"
                " ORDER BY demoted DESC, conns DESC LIMIT 500;", [&](sqlite3_stmt* s) {
                bool demoted = sqlite3_column_int(s, 2) != 0;
                int proto = sqlite3_column_int(s, 4);
                hstring pname = proto == 6 ? L"TCP" : proto == 17 ? L"UDP" : to_hstring(proto);
                // Stash the protocol in the row Id so a demote targets the exact
                // (app, port, proto) - the same key the enforce baseline groups by.
                rows.push_back({ proto, { to_hstring(sqlite3_column_int(s, 0)), to_hstring(sqlite3_column_int(s, 1)),
                                          demoted ? hstring(L"DEMOTED") : hstring(L"permitted"),
                                          U8(ng::Db::ColText(s, 3)), pname } });
            });
        }
        else if (curView_ == L"app-detail")
        {
            // Destination breakdown for one app: filter the raw log to this app's
            // image_id(s) (a signer can span several binaries) and group by
            // destination + port + type. Fast via idx_flow_events_image_id.
            SetHeaders(L"Destination", L"Port", L"Type", L"Events", L"Blocked");
            std::string label = to_string(detailApp_);
            if (ok)
            {
                sqlite3_stmt* s = nullptr;
                if (sqlite3_prepare_v2(d.handle(),
                        "SELECT COALESCE(fe.remote_domain, fe.remote_addr, '(none)'),"
                        " COALESCE(fe.remote_port, 0), fe.protocol, fe.direction,"
                        " COUNT(*), SUM(CASE WHEN fe.verdict LIKE '%DROP%' OR fe.verdict='BLOCK' THEN 1 ELSE 0 END)"
                        " FROM flow_events fe"
                        " WHERE fe.image_id IN (SELECT id FROM process_identity"
                        "   WHERE COALESCE(signer, image_path, '(unknown)') = ?)"
                        " GROUP BY 1, 2, fe.protocol, fe.direction"
                        " ORDER BY 5 DESC LIMIT 500;", -1, &s, nullptr) == SQLITE_OK)
                {
                    sqlite3_bind_text(s, 1, label.c_str(), (int)label.size(), SQLITE_TRANSIENT);
                    while (sqlite3_step(s) == SQLITE_ROW)
                    {
                        int proto = sqlite3_column_int(s, 2);
                        std::string dir = ng::Db::ColText(s, 3);
                        hstring pname = proto == 6 ? L"TCP" : proto == 17 ? L"UDP"
                                      : proto == 1 ? L"ICMP" : proto == 58 ? L"ICMPv6"
                                      : proto ? to_hstring(proto) : hstring(L"");
                        // "TCP out" / "UDP in" / just the proto if direction unknown.
                        hstring type = dir.empty() ? pname
                                     : pname + L" " + U8(dir);
                        rows.push_back({ 0, { U8(ng::Db::ColText(s, 0)), to_hstring(sqlite3_column_int(s, 1)),
                                              type, to_hstring(sqlite3_column_int(s, 4)),
                                              to_hstring(sqlite3_column_int(s, 5)) } });
                    }
                    sqlite3_finalize(s);
                }
            }

            // Header: totals (reused from the clicked row) + trust signals.
            std::string firstSeen, lastSeen;
            if (ok) d.each_bind(
                "SELECT MIN(ts_utc), MAX(ts_utc) FROM flow_events"
                " WHERE image_id IN (SELECT id FROM process_identity"
                "   WHERE COALESCE(signer, image_path, '(unknown)') = ?);",
                label, [&](sqlite3_stmt* s) {
                std::string mn = ng::Db::ColText(s, 0), mx = ng::Db::ColText(s, 1);
                firstSeen = mn.size() >= 19 ? mn.substr(11, 8) : mn;
                lastSeen  = mx.size() >= 19 ? mx.substr(11, 8) : mx;
            });
            int habits = 0, mlflags = 0;
            if (ok) d.each_bind("SELECT COUNT(*) FROM habits WHERE process_label = ?;", label,
                                [&](sqlite3_stmt* s) { habits = sqlite3_column_int(s, 0); });
            if (ok) d.each_bind(
                "SELECT COUNT(*) FROM ml_flags WHERE app_path IN (SELECT image_path FROM process_identity"
                "  WHERE COALESCE(signer, image_path, '(unknown)') = ?);", label,
                [&](sqlite3_stmt* s) { mlflags = sqlite3_column_int(s, 0); });

            AppDetailTotals().Text(U8(to_string(detailEvents_) + " events · " + to_string(detailBlocked_) +
                                      " blocked · " + to_string(detailDests_) + " destinations" +
                                      (lastSeen.empty() ? "" : "  ·  last seen " + lastSeen) +
                                      "   (last 14 days)"));
            AppDetailTrust().Text(U8("Learned habits: " + std::to_string(habits) +
                                     "    ML flags: " + (mlflags ? std::to_string(mlflags) : std::string("none"))));
        }
        // (the Settings and Digest views use their own panels, not the data table)

        if (!filter_.empty())
        {
            std::string f = to_string(filter_);
            for (auto& ch : f) ch = (char)tolower((unsigned char)ch);
            rows.erase(std::remove_if(rows.begin(), rows.end(), [&](RowData const& r) {
                for (int i = 0; i < 5; ++i)
                {
                    std::string c = to_string(r.c[i]);
                    for (auto& ch : c) ch = (char)tolower((unsigned char)ch);
                    if (c.find(f) != std::string::npos) return false;   // a column matches - keep
                }
                return true;   // nothing matched - drop
            }), rows.end());
        }

        if (rows.empty())
            rows.push_back({ 0, { L"", L"", !filter_.empty() ? hstring(L"(no matches)")
                                          : ok ? hstring(L"(no rows yet)")
                                               : U8("(DB not found at " + DbPathU8() + ")"), L"", L"" } });

        if (sortCol_ >= 0 && sortCol_ < 5)
        {
            int col = sortCol_; bool asc = sortAsc_;
            std::stable_sort(rows.begin(), rows.end(), [col, asc](RowData const& a, RowData const& b) {
                std::string sa = to_string(a.c[col]), sb = to_string(b.c[col]);
                char* ea = nullptr; char* eb = nullptr;
                double da = std::strtod(sa.c_str(), &ea), db = std::strtod(sb.c_str(), &eb);
                bool na = !sa.empty() && ea && *ea == 0, nb = !sb.empty() && eb && *eb == 0;
                int cmp;
                if (na && nb) cmp = da < db ? -1 : da > db ? 1 : 0;
                else {
                    for (auto& ch : sa) ch = (char)tolower((unsigned char)ch);
                    for (auto& ch : sb) ch = (char)tolower((unsigned char)ch);
                    int c = sa.compare(sb); cmp = c < 0 ? -1 : c > 0 ? 1 : 0;
                }
                return asc ? cmp < 0 : cmp > 0;
            });
        }

        // Remember the selected row (by id) and the scroll position, so both
        // survive the wholesale rebuild below - a live-updating list (Live
        // refreshes every second) is otherwise unscrollable, since replacing
        // ItemsSource resets a ListView's ScrollViewer to the top on every tick.
        int64_t selId = 0;
        if (auto sel = DataList().SelectedItem().try_as<NeuralGuard::Row>()) selId = sel.Id();
        auto scroller = FindScrollViewer(DataList());
        double vOffset = scroller ? scroller.VerticalOffset() : 0;

        // Live refreshes once a second; a wholesale ItemsSource replacement (every
        // other view's approach, still used below) flickers every tick, since a
        // new ItemsSource is entirely new content to WinUI even when only a
        // couple of rows actually changed. While Live's already-realized
        // collection is valid (liveItemsValid_ - cleared on every tab switch by
        // ShowView), mutate it in place instead.
        //
        // A naive index-aligned prefix/suffix diff does NOT work here: the query
        // is capped at LIMIT 300, so once the feed is full, every new row at the
        // front pushes one off the back - which shifts every surviving row's
        // INDEX by however many new ones arrived. Comparing old[i] to new[i]
        // then finds nothing matching anywhere, degenerating to "remove all,
        // insert all" - worse than the ItemsSource swap it was meant to replace.
        //
        // The right model exploits what this feed actually is: zero or more
        // brand-new rows prepended at the front, then the SAME old rows in the
        // same relative order, with some possibly trimmed off the tail by the
        // LIMIT. So: find where the old list's first row reappears in the new
        // list (that position is how many rows are genuinely new), then verify
        // the rest really does line up before trusting it - if a filter or sort
        // is active this won't hold, and falling through to a full rebuild is
        // correct instead of applying a wrong patch.
        if (curView_ == L"live" && liveItemsValid_ && !liveIds_.empty())
        {
            std::vector<int64_t> newIds;
            newIds.reserve(rows.size());
            for (auto const& r : rows) newIds.push_back(r.id);

            size_t oldN = liveIds_.size(), newN = newIds.size();
            size_t k = 0;
            while (k < newN && newIds[k] != liveIds_[0]) ++k;

            bool aligned = k < newN;
            size_t overlap = 0;
            if (aligned)
            {
                overlap = (oldN < newN - k) ? oldN : (newN - k);
                for (size_t i = 0; i < overlap; ++i)
                    if (newIds[k + i] != liveIds_[i]) { aligned = false; break; }
            }

            if (aligned)
            {
                for (size_t i = 0; i < k; ++i)
                {
                    auto const& r = rows[i];
                    liveItems_.InsertAt((uint32_t)i, MakeRow(r.id, r.c[0], r.c[1], r.c[2], r.c[3], r.c[4]));
                }
                while (liveItems_.Size() > (uint32_t)newN) liveItems_.RemoveAt(liveItems_.Size() - 1);
                liveIds_ = std::move(newIds);

                if (selId != 0)
                {
                    uint32_t idx = 0;
                    for (auto const& it : liveItems_)
                    {
                        if (auto r = it.try_as<NeuralGuard::Row>(); r && r.Id() == selId)
                        {
                            DataList().SelectedIndex((int32_t)idx);
                            break;
                        }
                        ++idx;
                    }
                }
                // No scroll-offset restore needed here: inserting/removing
                // individual items doesn't reset the ScrollViewer the way a full
                // ItemsSource replacement does.
                return;
            }
            // Not aligned (filter/sort active, or a burst bigger than the page) -
            // fall through to the full rebuild below.
        }

        auto items = single_threaded_observable_vector<IInspectable>();
        for (auto const& r : rows)
            items.Append(MakeRow(r.id, r.c[0], r.c[1], r.c[2], r.c[3], r.c[4]));
        DataList().ItemsSource(items);

        if (curView_ == L"live")
        {
            // First landing on Live (or the tick right after a tab switch): seed
            // the persisted collection so subsequent ticks can diff against it.
            liveItems_ = items;
            liveIds_.clear();
            liveIds_.reserve(rows.size());
            for (auto const& r : rows) liveIds_.push_back(r.id);
            liveItemsValid_ = true;
        }

        if (selId != 0)
        {
            uint32_t idx = 0;
            for (auto const& it : items)
            {
                if (auto r = it.try_as<NeuralGuard::Row>(); r && r.Id() == selId)
                {
                    DataList().SelectedIndex((int32_t)idx);
                    break;
                }
                ++idx;
            }
        }

        // Restoring immediately would race the layout pass that the new
        // ItemsSource still needs to run (the ScrollViewer's scrollable extent
        // isn't updated yet) - defer to the next UI-thread tick, by which point
        // WinUI has measured/arranged the new items.
        if (vOffset > 0)
        {
            DispatcherQueue().TryEnqueue([this, vOffset] {
                if (auto sv = FindScrollViewer(DataList()))
                    sv.ChangeView(nullptr, box_value(vOffset).as<IReference<double>>(), nullptr, true);
            });
        }
    }

    void MainWindow::ShowView(hstring const& tag)
    {
        curView_ = tag;
        viewReady_ = true;
        liveItemsValid_ = false;   // force one full rebuild before Live's incremental diff resumes
        hstring title = L"Live";
        if (tag == L"rules") title = L"Rules";
        else if (tag == L"habits") title = L"Habits";
        else if (tag == L"apps") title = L"Per-app";
        else if (tag == L"flows") title = L"Flows";
        else if (tag == L"flags") title = L"Flags";
        else if (tag == L"baseline") title = L"Baseline";
        else if (tag == L"inbound") title = L"Inbound";
        else if (tag == L"feedback") title = L"Feedback";
        else if (tag == L"insights") title = L"Insights";
        else if (tag == L"settings") title = L"Settings";
        else if (tag == L"app-detail") title = detailApp_;   // the drilled-into app
        ViewTitle().Text(title);

        // app-detail is a sub-view of Per-app: its back button + trust card show
        // only here; every other view collapses them.
        bool appDetail = (tag == L"app-detail");
        AppDetailBack().Visibility(appDetail ? Visibility::Visible : Visibility::Collapsed);
        AppDetailCard().Visibility(appDetail ? Visibility::Visible : Visibility::Collapsed);

        bool settings = (tag == L"settings");
        bool insights = (tag == L"insights");
        bool table = !settings && !insights;   // the shared sortable data table
        TableCard().Visibility(table ? Visibility::Visible : Visibility::Collapsed);
        SettingsPanel().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);
        InsightsPanel().Visibility(insights ? Visibility::Visible : Visibility::Collapsed);
        SearchBox().Visibility(table ? Visibility::Visible : Visibility::Collapsed);
        RulesTools().Visibility((tag == L"rules") ? Visibility::Visible : Visibility::Collapsed);
        FlagsTools().Visibility((tag == L"flags") ? Visibility::Visible : Visibility::Collapsed);
        FeedbackTools().Visibility((tag == L"feedback") ? Visibility::Visible : Visibility::Collapsed);
        filter_ = L"";
        SearchBox().Text(L"");   // reset the filter when switching views
        if (settings) LoadSettings();
        else if (insights) BuildInsights();
        else
        {
            // Pick the row template for this view - pill badges where verdict/state/
            // kind/label tokens live, red Blocked for Per-app, plain otherwise.
            hstring tpl = L"TplGeneric";
            if (tag == L"live" || tag == L"flags") tpl = L"TplLive";
            else if (tag == L"baseline") tpl = L"TplBaseline";
            else if (tag == L"feedback") tpl = L"TplFeedback";
            else if (tag == L"apps") tpl = L"TplPerApp";
            else if (tag == L"flows") tpl = L"TplFlows";
            // app-detail uses the plain generic template (destination breakdown).
            auto res = ContentRoot().Resources();
            if (res.HasKey(box_value(tpl)))
                DataList().ItemTemplate(res.Lookup(box_value(tpl)).as<winrt::Microsoft::UI::Xaml::DataTemplate>());

            // Per-view column widths (the * column fills; 0 = unused 5th column).
            // Widths mirror the prototype grid-template-columns exactly.
            if (tag == L"live") SetCols(130, 128, -1, 210, 84);
            else if (tag == L"flags")                SetCols(120, 120, 90, -1, 240);
            else if (tag == L"baseline")             SetCols(80, 90, 130, -1, 80);
            else if (tag == L"feedback")             SetCols(120, 120, 120, -1, 220);
            else if (tag == L"apps")                 SetCols(120, 110, 100, -1, 0);
            else if (tag == L"flows")                SetCols(120, -1, 230, 110, 110);
            else if (tag == L"rules")                SetCols(120, 90, 120, -1, 0);
            else if (tag == L"inbound")              SetCols(110, 80, 90, -1, 150);
            else if (tag == L"habits")               SetCols(110, 90, -1, 260, 0);
            else if (tag == L"app-detail")           SetCols(-1, 80, 130, 110, 90);
            else                                     SetCols(130, 128, -1, 210, 84);

            // Flows right-aligns its two numeric headers (Anomaly, P(malicious)) to
            // sit above the right-aligned score cells; every other view is left-aligned.
            using winrt::Microsoft::UI::Xaml::TextAlignment;
            bool rightNums = (tag == L"flows");
            H3().TextAlignment(rightNums ? TextAlignment::Right : TextAlignment::Left);
            H4().TextAlignment(rightNums ? TextAlignment::Right : TextAlignment::Left);
            H3().Margin(rightNums ? Thickness{ 0, 0, 22, 0 } : Thickness{ 0, 0, 0, 0 });
            H4().Margin(rightNums ? Thickness{ 0, 0, 4, 0 } : Thickness{ 0, 0, 0, 0 });
            RefreshCurrent();
        }
    }

    void MainWindow::OnSearchChanged(Controls::AutoSuggestBox const& box,
                                     Controls::AutoSuggestBoxTextChangedEventArgs const&)
    {
        filter_ = box.Text();
        if (curView_ != L"settings") RefreshCurrent();
    }

    HWND MainWindow::WindowHandle()
    {
        HWND h = nullptr;
        if (auto native = this->try_as<::IWindowNative>())
            native->get_WindowHandle(&h);
        return h;
    }

    // Rules are exported/imported as pipe-delimited text, one rule per line:
    //   action|app_path|remote_addr|remote_port|protocol|enabled|expires_epoch
    void MainWindow::OnExportRules(IInspectable const&, RoutedEventArgs const&)
    {
        wchar_t file[MAX_PATH] = L"neuralguard-rules.txt";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = WindowHandle();
        ofn.lpstrFilter = L"Rules (*.txt)\0*.txt\0All files\0*.*\0";
        ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"txt";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&ofn)) return;   // cancelled

        ng::Db d;
        if (!d.open(DbPathU8().c_str())) { Notify(L"Couldn't open the policy database.", InfoBarSeverity::Error); return; }
        std::string out;
        int n = 0;
        d.each("SELECT action, COALESCE(app_path,''), COALESCE(remote_addr,''), COALESCE(remote_port,0),"
               " COALESCE(protocol,0), enabled, CAST(COALESCE(expires_epoch,0) AS INTEGER)"
               " FROM rules ORDER BY id;", [&](sqlite3_stmt* s) {
            out += ng::Db::ColText(s, 0); out += '|';
            out += ng::Db::ColText(s, 1); out += '|';
            out += ng::Db::ColText(s, 2); out += '|';
            out += std::to_string(sqlite3_column_int(s, 3)); out += '|';
            out += std::to_string(sqlite3_column_int(s, 4)); out += '|';
            out += std::to_string(sqlite3_column_int(s, 5)); out += '|';
            out += std::to_string(sqlite3_column_int64(s, 6)); out += "\r\n";
            ++n;
        });

        HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { Notify(L"Couldn't write that file.", InfoBarSeverity::Error); return; }
        DWORD wr = 0; WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr); CloseHandle(h);
        Notify(L"Exported " + to_hstring(n) + L" rule(s).", InfoBarSeverity::Success);
    }

    void MainWindow::OnImportRules(IInspectable const&, RoutedEventArgs const&)
    {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = WindowHandle();
        ofn.lpstrFilter = L"Rules (*.txt)\0*.txt\0All files\0*.*\0";
        ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return;

        HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { Notify(L"Couldn't read that file.", InfoBarSeverity::Error); return; }
        std::string content; char buf[4096]; DWORD rd = 0;
        while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) content.append(buf, rd);
        CloseHandle(h);

        ng::Db d;
        if (!d.open(DbPathU8().c_str())) { Notify(L"Couldn't open the policy database.", InfoBarSeverity::Error); return; }
        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "INSERT INTO rules(action,app_path,remote_addr,remote_port,protocol,enabled,expires_epoch,created_at)"
            " VALUES(?,?,?,?,?,?,?,datetime('now'));", -1, &ins, nullptr);
        int n = 0;
        double now = (double)time(nullptr);
        size_t pos = 0;
        while (pos < content.size())
        {
            size_t eol = content.find('\n', pos);
            std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = (eol == std::string::npos) ? content.size() : eol + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line.empty()) continue;

            std::vector<std::string> f;
            size_t p = 0;
            for (;;) { size_t bar = line.find('|', p);
                       f.push_back(line.substr(p, bar == std::string::npos ? std::string::npos : bar - p));
                       if (bar == std::string::npos) break; p = bar + 1; }
            if (f.size() < 7) continue;
            if (f[0] != "permit" && f[0] != "block") continue;   // skip headers / junk

            sqlite3_reset(ins);
            sqlite3_bind_text(ins, 1, f[0].c_str(), -1, SQLITE_TRANSIENT);
            if (f[1].empty()) sqlite3_bind_null(ins, 2); else sqlite3_bind_text(ins, 2, f[1].c_str(), -1, SQLITE_TRANSIENT);
            if (f[2].empty()) sqlite3_bind_null(ins, 3); else sqlite3_bind_text(ins, 3, f[2].c_str(), -1, SQLITE_TRANSIENT);
            int port = atoi(f[3].c_str());   if (port)  sqlite3_bind_int(ins, 4, port);  else sqlite3_bind_null(ins, 4);
            int proto = atoi(f[4].c_str());  if (proto) sqlite3_bind_int(ins, 5, proto); else sqlite3_bind_null(ins, 5);
            sqlite3_bind_int(ins, 6, atoi(f[5].c_str()) ? 1 : 0);
            double exp = atof(f[6].c_str()); if (exp > now) sqlite3_bind_double(ins, 7, exp); else sqlite3_bind_null(ins, 7);
            if (sqlite3_step(ins) == SQLITE_DONE) ++n;
        }
        sqlite3_finalize(ins);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        Notify(L"Imported " + to_hstring(n) + L" rule(s).", InfoBarSeverity::Success);
        if (curView_ == L"rules") RefreshCurrent();
    }

    void MainWindow::LoadSettings()
    {
        loadingSettings_ = true;   // syncing the controls must not write back / toast
        std::string theme = MetaGet("theme", "dark");
        Theme0().IsChecked(theme == "dark");
        Theme1().IsChecked(theme == "light");
        Theme2().IsChecked(theme == "system");
        int a = ReadAutonomy();
        Auto0().IsChecked(a == 0);
        Auto1().IsChecked(a == 1);
        Auto2().IsChecked(a == 2);
        FeatureToggle().IsOn(MetaGet("feature_archive", "0") == "1");
        std::string mode = MetaGet("ml_mode", "shadow");
        Ml0().IsChecked(mode == "off");
        Ml1().IsChecked(mode == "shadow");
        Ml2().IsChecked(mode == "active");
        MalThresh().Value(std::strtod(MetaGet("ml_malicious_threshold", "0.9").c_str(), nullptr));
        AnomThresh().Value(std::strtod(MetaGet("ml_anomaly_threshold", "-0.15").c_str(), nullptr));
        GatesPanel().Visibility(mode == "active" ? Visibility::Visible : Visibility::Collapsed);
        loadingSettings_ = false;
        RefreshServiceStatus();
        AboutVersion().Text(U8("v" + std::string(NG_VERSION)));
    }

    int MainWindow::ReadAutonomy()
    {
        ng::Db d;
        if (d.open(DbPathU8().c_str()))
        {
            std::string v = d.scalar("SELECT v FROM meta WHERE k='autonomy';");
            if (!v.empty()) return atoi(v.c_str());
        }
        return 0;
    }

    void MainWindow::WriteAutonomy(int level)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "INSERT INTO meta(k,v) VALUES('autonomy',?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
            -1, &s, nullptr);
        std::string lv = std::to_string(level);
        sqlite3_bind_text(s, 1, lv.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_finalize(s);
    }

    void MainWindow::OnAutonomyChanged(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        int level = TagInt(sender);
        WriteAutonomy(level);   // enforce daemon re-reads meta('autonomy') live per drop
        hstring msg = level == 0 ? L"Autonomy: prompt on every new connection."
                    : level == 1 ? L"Autonomy: auto-allow apps you already use."
                                 : L"Autonomy: auto-allow everything (log only).";
        Notify(msg, InfoBarSeverity::Success);
    }

    // key is always a hardcoded constant here (no user input -> no injection risk).
    std::string MainWindow::MetaGet(const char* key, const char* dflt)
    {
        ng::Db d;
        if (d.open(DbPathU8().c_str()))
        {
            std::string v = d.scalar((std::string("SELECT v FROM meta WHERE k='") + key + "';").c_str());
            if (!v.empty()) return v;
        }
        return dflt;
    }
    void MainWindow::MetaSet(const char* key, const char* val)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v;",
            -1, &s, nullptr);
        sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(s, 2, val, -1, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_finalize(s);
    }

    void MainWindow::OnFeatureToggle(IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        bool on = FeatureToggle().IsOn();
        MetaSet("feature_archive", on ? "1" : "0");
        Notify(on ? L"Flow feature collection ON — applies next time enforcement or learning runs."
                  : L"Flow feature collection OFF.", InfoBarSeverity::Success);
    }
    // Theme goes on the root FrameworkElement, not the Application: an app-level
    // RequestedTheme is fixed for the process lifetime, while an element-level one
    // can change live. ElementTheme::Default means "inherit from the app", and the
    // app no longer forces Dark, so Default = follow the Windows setting - which
    // is also what makes the 'system' option track an OS theme change without any
    // listener of our own.
    void MainWindow::ApplyTheme(std::string const& theme)
    {
        auto root = Content().try_as<FrameworkElement>();
        if (!root) return;
        root.RequestedTheme(theme == "light"  ? ElementTheme::Light
                          : theme == "system" ? ElementTheme::Default
                                              : ElementTheme::Dark);

        // ActualTheme, not the requested one: 'system' resolves to whatever
        // Windows is currently set to, and SemBrush needs the concrete answer.
        SyncThemeDependents();

        // 'system' means the theme can change without us: hook ActualThemeChanged
        // once so SemBrush + the caption buttons follow an OS switch too. (The
        // token brushes are ThemeResource and re-resolve on their own.)
        if (!themeHooked_)
        {
            themeHooked_ = true;
            root.ActualThemeChanged([this](FrameworkElement const&, IInspectable const&) {
                SyncThemeDependents();
            });
        }
    }

    // Minimize/maximize/close glyphs. Transparent backgrounds either way so the
    // custom title bar shows through; only the glyph colour flips. Hardcoded to
    // match NG.Color.Text.Primary per theme - these are set through an AppWindow
    // API that can't read XAML resources.
    void MainWindow::ApplyCaptionColors(bool light)
    {
        auto tb = AppWindow().TitleBar();
        if (!tb) return;
        using winrt::Windows::UI::Color;
        const Color fg     = light ? Color{ 255, 17, 21, 28 }    : Color{ 255, 232, 236, 244 };
        const Color fgIdle = light ? Color{ 120, 17, 21, 28 }    : Color{ 120, 232, 236, 244 };
        const Color hover  = light ? Color{ 26, 0, 0, 0 }        : Color{ 40, 255, 255, 255 };
        const Color hoverFg= light ? Color{ 255, 0, 0, 0 }       : Color{ 255, 255, 255, 255 };

        tb.ButtonBackgroundColor(Color{ 0, 0, 0, 0 });
        tb.ButtonInactiveBackgroundColor(Color{ 0, 0, 0, 0 });
        tb.ButtonForegroundColor(fg);
        tb.ButtonInactiveForegroundColor(fgIdle);
        tb.ButtonHoverBackgroundColor(hover);
        tb.ButtonHoverForegroundColor(hoverFg);
    }

    // The two things that don't follow ThemeResource on their own: the SemBrush
    // converter (no element, so it can't see ActualTheme) and the caption
    // buttons (an AppWindow TitleBar API, not part of the XAML resource system).
    void MainWindow::SyncThemeDependents()
    {
        auto root = Content().try_as<FrameworkElement>();
        const bool light = root && root.ActualTheme() == ElementTheme::Light;

        // Row bakes its brushes at construction, so it needs the theme before any
        // rows are built. (SemBrush is currently unreferenced by any XAML - the
        // pills come from Row - but it's still registered in App.xaml, so keep it
        // in sync rather than leave a stale trap for whoever wires it up.)
        Row::SetLightTheme(light);
        SemBrush::SetLightTheme(light);
        ApplyCaptionColors(light);

        // Existing rows were built with the old brushes; the converter only runs
        // on (re)binding, so force a rebuild to repaint the verdict pills.
        // Guarded: ApplyTheme runs from the constructor, before ShowView() has
        // established a view - refreshing then would query for a view that isn't
        // set up yet. The initial ShowView paints with the right theme anyway,
        // since SetLightTheme has already landed by that point.
        liveItemsValid_ = false;
        if (!curView_.empty() && viewReady_) RefreshCurrent();
    }

    void MainWindow::OnThemeChanged(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        auto fe = sender.try_as<FrameworkElement>();
        std::string theme = fe ? to_string(unbox_value_or<hstring>(fe.Tag(), L"dark")) : "dark";
        MetaSet("theme", theme.c_str());
        ApplyTheme(theme);
    }

    void MainWindow::OnMlModeChanged(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        auto fe = sender.try_as<FrameworkElement>();
        std::string mode = fe ? to_string(unbox_value_or<hstring>(fe.Tag(), L"shadow")) : "shadow";
        if (mode != MetaGet("ml_mode", "shadow")) {
            MetaSet("ml_mode", mode.c_str());
            // Record when the mode last changed, for the Insights Status card.
            ng::Db d;
            if (d.open(DbPathU8().c_str()))
                d.each("SELECT strftime('%Y-%m-%dT%H:%M:%SZ','now');", [&](sqlite3_stmt* s) {
                    MetaSet("ml_mode_since", ng::Db::ColText(s, 0).c_str());
                });
        }
        GatesPanel().Visibility(mode == "active" ? Visibility::Visible : Visibility::Collapsed);
        if (mode == "active")
            Notify(L"Active scoring on. A strongly-malicious score can now demote a trusted app so it "
                   L"prompts again (it never auto-blocks). Takes effect next time enforcement runs. "
                   L"Use trained models, not the placeholders.", InfoBarSeverity::Warning);
        else
            Notify(L"ML scoring mode: " + to_hstring(mode) + L".", InfoBarSeverity::Success);
    }

    void MainWindow::OnMlThresholdChanged(Controls::NumberBox const& sender,
                                          Controls::NumberBoxValueChangedEventArgs const&)
    {
        if (loadingSettings_) return;
        double v = sender.Value();
        if (v != v) return;   // NaN = the box was cleared; ignore
        char buf[32]; snprintf(buf, sizeof buf, "%.3f", v);
        MetaSet(sender.Name() == L"MalThresh" ? "ml_malicious_threshold" : "ml_anomaly_threshold", buf);
        Notify(L"Confidence gate updated (takes effect next time enforcement runs).", InfoBarSeverity::Success);
    }

    void MainWindow::OnClearFlags(IInspectable const&, RoutedEventArgs const&)
    {
        ClearMlFlags();
        if (curView_ == L"flags" || curView_ == L"baseline" || curView_ == L"inbound") RefreshCurrent();
    }

    void MainWindow::OnExportFeedback(IInspectable const&, RoutedEventArgs const&)
    {
        wchar_t file[MAX_PATH] = L"neuralguard-feedback.csv";
        OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = WindowHandle();
        ofn.lpstrFilter = L"CSV (*.csv)\0*.csv\0All files\0*.*\0";
        ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"csv";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&ofn)) return;

        ng::Db d;
        if (!d.open(DbPathU8().c_str())) { Notify(L"Couldn't open the policy database.", InfoBarSeverity::Error); return; }
        std::string out = "duration_ms,bytes_in,bytes_out,remote_port,label\r\n";
        int n = 0;
        d.each("SELECT COALESCE(MAX(ff.duration_ms),0), COALESCE(MAX(ff.bytes_in),0),"
               " COALESCE(MAX(ff.bytes_out),0), fl.remote_port, fl.label FROM feedback_labels fl"
               " LEFT JOIN flow_features ff ON ff.process_key=fl.process_key AND ff.dest=fl.dest"
               "   AND ff.remote_port=fl.remote_port GROUP BY fl.id;", [&](sqlite3_stmt* s) {
            out += std::to_string(sqlite3_column_int(s, 0)) + "," +
                   std::to_string(sqlite3_column_int64(s, 1)) + "," +
                   std::to_string(sqlite3_column_int64(s, 2)) + "," +
                   std::to_string(sqlite3_column_int(s, 3)) + "," +
                   std::to_string(sqlite3_column_int(s, 4)) + "\r\n";
            ++n;
        });
        HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { Notify(L"Couldn't write that file.", InfoBarSeverity::Error); return; }
        DWORD wr = 0; WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr); CloseHandle(h);
        Notify(L"Exported " + to_hstring(n) + L" label(s).", InfoBarSeverity::Success);
    }

    // --- Software updates (Settings card) -----------------------------------
    // Shared ng::Updater runs on a background thread; UI is touched only back on
    // the dispatcher. SCAFFOLD: capturing `this` in a detached thread assumes the
    // window outlives the check - fine for now, revisit with a weak ref / cancel
    // token if this ships. Not yet exercised against a real release.
    void MainWindow::OnCheckUpdate(IInspectable const&, RoutedEventArgs const&)
    {
        UpdateStatus().Text(L"Checking for updates...");
        InstallUpdateBtn().IsEnabled(false);
        UpdateNotesLink().Visibility(Visibility::Collapsed);
        auto dq = DispatcherQueue();
        std::thread([this, dq]() {
            ng::UpdateInfo info = ng::Updater().check();
            dq.TryEnqueue([this, info]() {
                if (!info.error.empty()) {
                    UpdateStatus().Text(to_hstring("Check failed: " + info.error));
                } else if (info.available) {
                    UpdateStatus().Text(to_hstring("Update available: " + info.latestVersion +
                                                   "  (you have " + info.currentVersion + ")"));
                    InstallUpdateBtn().IsEnabled(true);
                    if (!info.notes.empty()) {
                        UpdateNotesLink().NavigateUri(Windows::Foundation::Uri{ to_hstring(info.notes) });
                        UpdateNotesLink().Visibility(Visibility::Visible);
                    }
                } else {
                    UpdateStatus().Text(to_hstring("You are up to date (" + info.currentVersion + ")."));
                }
            });
        }).detach();
    }

    void MainWindow::OnInstallUpdate(IInspectable const&, RoutedEventArgs const&)
    {
        InstallUpdateBtn().IsEnabled(false);
        UpdateProgress().IsIndeterminate(true);
        UpdateProgress().Visibility(Visibility::Visible);
        UpdateStatus().Text(L"Preparing...");
        auto dq = DispatcherQueue();
        std::thread([this, dq]() {
            ng::Updater up;
            ng::UpdateInfo info = up.check();
            if (info.error.empty() && info.available) {
                auto prog = [this, dq](ng::UpdateStage, int pct, std::string const& msg) {
                    dq.TryEnqueue([this, pct, msg]() {
                        if (pct >= 0) { UpdateProgress().IsIndeterminate(false); UpdateProgress().Value(pct); }
                        else UpdateProgress().IsIndeterminate(true);
                        if (!msg.empty()) UpdateStatus().Text(to_hstring(msg));
                    });
                };
                std::string path = up.download(info, prog);
                bool launched = !path.empty() && up.apply(path, prog);
                if (launched) {
                    dq.TryEnqueue([this]() {
                        UpdateStatus().Text(L"Installer launched - closing NeuralGuard to finish updating...");
                    });
                    // Give the user a beat to read it, then exit so our files unlock
                    // for the silent installer (which also force-closes us as a backstop).
                    Sleep(1500);
                    dq.TryEnqueue([]() {
                        if (auto app = Application::Current()) app.Exit();
                    });
                } else {
                    dq.TryEnqueue([this]() {
                        UpdateStatus().Text(L"Update failed (download or verify). Try again later.");
                        UpdateProgress().Visibility(Visibility::Collapsed);
                        InstallUpdateBtn().IsEnabled(true);
                    });
                }
            } else {
                dq.TryEnqueue([this]() {
                    UpdateStatus().Text(L"Nothing to install - run Check for updates first.");
                    UpdateProgress().Visibility(Visibility::Collapsed);
                });
            }
        }).detach();
    }

    // Direct-DB helpers (same pattern as the rules editor: write + bump rules_gen
    // so a running enforce daemon re-applies the baseline live).
    void MainWindow::DemoteApp(hstring const& appPath, int port, int proto)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "INSERT OR IGNORE INTO ml_flags(ts_utc,kind,process_key,process_label,app_path,dest,"
            "remote_port,protocol,score) VALUES(strftime('%Y-%m-%dT%H:%M:%SZ','now'),'demote','',"
            "'(manual)',?,'(manual)',?,?,1.0);", -1, &s, nullptr);
        std::string p = to_string(appPath);
        sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, port);
        sqlite3_bind_int(s, 3, proto);
        sqlite3_step(s); sqlite3_finalize(s);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        Notify(L"Demoted - it will prompt on its next connection.", InfoBarSeverity::Success);
    }

    void MainWindow::RetrustApp(hstring const& appPath, int port, int proto)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "DELETE FROM ml_flags WHERE kind='demote' AND app_path=? AND remote_port=? AND protocol=?;",
            -1, &s, nullptr);
        std::string p = to_string(appPath);
        sqlite3_bind_text(s, 1, p.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(s, 2, port);
        sqlite3_bind_int(s, 3, proto);
        sqlite3_step(s); sqlite3_finalize(s);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        Notify(L"Re-trusted - it auto-permits again next time enforcement runs.", InfoBarSeverity::Success);
    }

    void MainWindow::RemoveFlag(int64_t id)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(), "DELETE FROM ml_flags WHERE id=?;", -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, id);
        sqlite3_step(s); sqlite3_finalize(s);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        Notify(L"Flag removed.", InfoBarSeverity::Success);
    }

    // Permit (or revoke) an inbound service from the blocked-inbound review list.
    // Bumping rules_gen makes a running enforce daemon re-apply, which rebuilds the
    // inbound baseline including this row - so it takes effect without a restart.
    void MainWindow::SetInboundAllowed(int64_t id, bool allowed)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(), "UPDATE inbound_blocked SET allowed=? WHERE id=?;", -1, &s, nullptr);
        sqlite3_bind_int(s, 1, allowed ? 1 : 0);
        sqlite3_bind_int64(s, 2, id);
        sqlite3_step(s); sqlite3_finalize(s);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        Notify(allowed ? L"Inbound service allowed (applies live)."
                       : L"Inbound service blocked again (applies live).",
               InfoBarSeverity::Success);
    }

    void MainWindow::ClearMlFlags()
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_exec(d.handle(), "DELETE FROM ml_flags;", nullptr, nullptr, nullptr);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        Notify(L"Cleared all ML flags; demoted apps are trusted again.", InfoBarSeverity::Success);
    }

    // Insights helpers -------------------------------------------------------
    // Brushes get baked from literal ARGB, not looked up from resources: the
    // NG.Brush.* live in a *merged* dictionary and code-side lookup misses them
    // (same reason Row.cpp bakes its pills). XAML ThemeResource still resolves
    // fine, so only the mode pill (set here) needs this.
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

    Media::Geometry MainWindow::RingArc(double frac, double cx, double cy, double r)
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

    // Insights = the learning/ML summary that replaced the text Digest. Every
    // number here is advisory and read-only; nothing is enforced from this view.
    void MainWindow::BuildInsights()
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;

        // --- Status: ML mode pill + thresholds ---
        std::string mode = MetaGet("ml_mode", "shadow");
        std::string malThr = MetaGet("ml_malicious_threshold", "0.9");
        std::string anomThr = MetaGet("ml_anomaly_threshold", "-0.15");
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
        std::string since = MetaGet("ml_mode_since", "");
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

    void MainWindow::OnInsEditThresholds(IInspectable const&, RoutedEventArgs const&) { NavTo(L"settings"); }
    void MainWindow::OnInsViewFlags(IInspectable const&, RoutedEventArgs const&) { NavTo(L"flags"); }
    void MainWindow::OnInsViewFlows(IInspectable const&, RoutedEventArgs const&) { NavTo(L"flows"); }

    // Select a sidebar item by tag (in either list); the SelectionChanged handler
    // then switches the view, so the highlight follows the jump.
    void MainWindow::NavTo(hstring const& tag)
    {
        for (auto lv : { NavList(), NavFooter() })
            for (auto const& it : lv.Items())
                if (auto lvi = it.try_as<ListViewItem>();
                    lvi && unbox_value_or<hstring>(lvi.Tag(), L"") == tag) {
                    lv.SelectedItem(lvi);
                    return;
                }
    }

    void MainWindow::RefreshServiceStatus()
    {
        hstring text = L"Not installed.";
        if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT))
        {
            if (SC_HANDLE svc = OpenServiceW(scm, L"NeuralGuard", SERVICE_QUERY_STATUS))
            {
                SERVICE_STATUS st{};
                if (QueryServiceStatus(svc, &st))
                    text = (st.dwCurrentState == SERVICE_RUNNING) ? hstring(L"Installed and running.")
                                                                  : hstring(L"Installed (stopped).");
                else
                    text = L"Installed.";
                CloseServiceHandle(svc);
            }
            CloseServiceHandle(scm);
        }
        SvcStatus().Text(text);
    }

    void MainWindow::OnServiceInstall(IInspectable const&, RoutedEventArgs const&)
    {
        if (RunTool(L"ngd.exe", L"install \"" + NgDir() + L"\\ngpolicy.db\""))
            Notify(L"Installing the NeuralGuard service...", InfoBarSeverity::Informational);
    }

    void MainWindow::OnServiceRemove(IInspectable const&, RoutedEventArgs const&)
    {
        if (RunTool(L"ngd.exe", L"uninstall"))
            Notify(L"Removing the NeuralGuard service...", InfoBarSeverity::Informational);
    }

    // ngd/ngctl manage WFP filters, which needs administrator rights. If this
    // process is already elevated we launch the tool directly; otherwise we ask
    // for elevation via the "runas" verb (one UAC prompt). Returns true only if
    // the tool actually started, so callers don't post a false "started" toast.
    bool MainWindow::RunTool(std::wstring const& exe, std::wstring const& args)
    {
        std::wstring dir  = NgDir();
        std::wstring file = dir + L"\\" + exe;

        bool elevated = false;
        HANDLE tok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        {
            TOKEN_ELEVATION te{}; DWORD cb = 0;
            if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &cb))
                elevated = te.TokenIsElevated != 0;
            CloseHandle(tok);
        }

        SHELLEXECUTEINFOW sei{}; sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = elevated ? L"open" : L"runas";
        sei.lpFile = file.c_str();
        sei.lpParameters = args.c_str();
        sei.lpDirectory = dir.c_str();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei))
        {
            if (sei.hProcess) CloseHandle(sei.hProcess);
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED)
            Notify(L"Elevation cancelled; " + hstring(exe) + L" not started.", InfoBarSeverity::Warning);
        else
            Notify(L"Couldn't launch " + hstring(exe), InfoBarSeverity::Error);
        return false;
    }

    void MainWindow::OnTick(IInspectable const&, IInspectable const&)
    {
        if (resizeCol_ >= 0 || menuOpen_) return;   // don't rebuild mid-drag or while a menu is open
        UpdateMode();
        if (curView_ == L"live") RefreshCurrent();
        else if (curView_ == L"settings") RefreshServiceStatus();   // reflect install/remove

        // Piggyback the periodic update check on this same once-a-second timer
        // instead of running a second one. First check ~10s after launch (let
        // the window paint before any network call); then once a day for as
        // long as the tray stays up, which - since it now starts at login and
        // stays running (see the Phase C/D tray merge) - is the only way an
        // update would ever be noticed without the user remembering to check.
        ++tickCount_;
        constexpr int64_t kFirstCheck = 10;
        constexpr int64_t kCheckInterval = 24LL * 60 * 60;
        if (tickCount_ == kFirstCheck ||
            (tickCount_ > kFirstCheck && (tickCount_ - kFirstCheck) % kCheckInterval == 0))
            CheckForUpdateInBackground();
    }

    // Same network check OnCheckUpdate runs, just unattended. Updates the
    // Settings panel regardless of which tab is showing (harmless if it's not
    // visible right now), and balloons through the tray - but only once per
    // newly-seen version, via meta('update_notified_version'), so a daily
    // recheck doesn't nag about the same release the user already saw and
    // hasn't gotten around to installing yet.
    void MainWindow::CheckForUpdateInBackground()
    {
        auto dq = DispatcherQueue();
        std::thread([this, dq]() {
            ng::UpdateInfo info = ng::Updater().check();
            if (info.error.empty() && info.available) {
                dq.TryEnqueue([this, info]() {
                    UpdateStatus().Text(to_hstring("Update available: " + info.latestVersion +
                                                   "  (you have " + info.currentVersion + ")"));
                    InstallUpdateBtn().IsEnabled(true);
                    if (!info.notes.empty()) {
                        UpdateNotesLink().NavigateUri(Windows::Foundation::Uri{ to_hstring(info.notes) });
                        UpdateNotesLink().Visibility(Visibility::Visible);
                    }
                    if (MetaGet("update_notified_version", "") != info.latestVersion) {
                        MetaSet("update_notified_version", info.latestVersion.c_str());
                        hstring msg = U8("v" + info.latestVersion + " is available (you're on v" +
                                         info.currentVersion + "). Open Settings to install.");
                        ngtray::Balloon(L"NeuralGuard update available", std::wstring(msg.c_str()));
                    }
                });
            }
        }).detach();
    }

    void MainWindow::OnNavSelect(IInspectable const& sender, SelectionChangedEventArgs const&)
    {
        if (navSyncing_) return;
        auto lv = sender.try_as<ListView>();
        if (!lv) return;
        auto item = lv.SelectedItem().try_as<ListViewItem>();
        if (!item) return;   // deselected (from clearing the other list) - ignore
        // Single-select across both sidebar lists: clear the other one.
        navSyncing_ = true;
        (lv == NavList() ? NavFooter() : NavList()).SelectedItem(nullptr);
        navSyncing_ = false;
        ShowView(unbox_value_or<hstring>(item.Tag(), L"live"));
    }

    void MainWindow::OnRowRightTapped(IInspectable const&, RightTappedRoutedEventArgs const& e)
    {
        // Find the ListViewItem (and its Row) under the pointer.
        DependencyObject src = e.OriginalSource().try_as<DependencyObject>();
        ListViewItem container{ nullptr };
        while (src)
        {
            if (auto lvi = src.try_as<ListViewItem>()) { container = lvi; break; }
            src = VisualTreeHelper::GetParent(src);
        }
        if (!container) return;
        auto row = DataList().ItemFromContainer(container).try_as<NeuralGuard::Row>();
        if (!row) return;   // per-view branches below decide what (if anything) applies
        ctxRow_ = row;

        MenuFlyout menu;
        auto add = [&](hstring const& text, std::function<void()> action) {
            MenuFlyoutItem it;
            it.Text(text);
            it.Click([action](IInspectable const&, RoutedEventArgs const&) { action(); });
            menu.Items().Append(it);
        };

        if (curView_ == L"rules")
        {
            if (ctxRow_.Id() == 0) return;
            add(L"Delete rule", [this] { DelRule(ctxRow_.Id()); RefreshCurrent(); });
        }
        else if (curView_ == L"live")
        {
            int64_t id = ctxRow_.Id();
            add(L"Block this destination", [this, id] { AddRuleFromEvent(id, true, false, 0); });
            add(L"Allow this destination", [this, id] { AddRuleFromEvent(id, false, false, 0); });
            add(L"Allow this destination for 1 hour", [this, id] { AddRuleFromEvent(id, false, false, 3600); });
            add(L"Allow this app (any port)", [this, id] { AddRuleFromEvent(id, false, true, 0); });
        }
        else if (curView_ == L"inbound")
        {
            int64_t id = ctxRow_.Id();
            if (id == 0) return;
            const bool allowed = (ctxRow_.C0() == L"ALLOWED");
            if (allowed)
                add(L"Block this service again", [this, id] { SetInboundAllowed(id, false); RefreshCurrent(); });
            else
                add(L"Allow this service", [this, id] { SetInboundAllowed(id, true); RefreshCurrent(); });
        }
        else if (curView_ == L"baseline")
        {
            hstring path = ctxRow_.C3();
            int port = _wtoi(ctxRow_.C0().c_str());
            int proto = (int)ctxRow_.Id();   // stashed protocol (6=TCP, 17=UDP)
            if (path.empty() || port == 0) return;
            if (ctxRow_.C2() == L"DEMOTED")
                add(L"Re-trust this app", [this, path, port, proto] { RetrustApp(path, port, proto); RefreshCurrent(); });
            else
                add(L"Distrust (demote) this app", [this, path, port, proto] { DemoteApp(path, port, proto); RefreshCurrent(); });
        }
        else if (curView_ == L"flags")
        {
            int64_t id = ctxRow_.Id();
            if (id == 0) return;
            add(L"Remove this flag", [this, id] { RemoveFlag(id); RefreshCurrent(); });
        }
        else if (curView_ == L"apps")
        {
            auto r = ctxRow_;
            if (r.C3().empty()) return;   // the "(no rows yet)" placeholder
            add(L"View destinations", [this, r] { OpenAppDetail(r); });
        }
        if (menu.Items().Size() == 0) return;

        DataList().SelectedItem(row);   // highlight the row the menu acts on
        menuOpen_ = true;               // pause the live refresh so the menu isn't torn down
        menu.Closed([this](auto&&, auto&&) { menuOpen_ = false; });

        FlyoutShowOptions opt;
        opt.Position(e.GetPosition(container));
        menu.ShowAt(container, opt);
    }

    // Double-click a Per-app row = the same drill-in as the right-click menu
    // item, as a shortcut. Discoverability lives in the menu; this is the accel.
    void MainWindow::OnRowDoubleTapped(IInspectable const&, DoubleTappedRoutedEventArgs const& e)
    {
        if (curView_ != L"apps") return;
        DependencyObject src = e.OriginalSource().try_as<DependencyObject>();
        ListViewItem container{ nullptr };
        while (src)
        {
            if (auto lvi = src.try_as<ListViewItem>()) { container = lvi; break; }
            src = VisualTreeHelper::GetParent(src);
        }
        if (!container) return;
        auto row = DataList().ItemFromContainer(container).try_as<NeuralGuard::Row>();
        if (row && !row.C3().empty()) OpenAppDetail(row);
    }

    void MainWindow::OnAppDetailBack(IInspectable const&, RoutedEventArgs const&)
    {
        ShowView(L"apps");
    }

    // Stash the clicked row's app label + its totals (so the header needs no
    // re-query), then switch to the detail view which renders the breakdown.
    void MainWindow::OpenAppDetail(NeuralGuard::Row const& row)
    {
        detailApp_     = row.C3();   // Application column
        detailEvents_  = row.C0();
        detailBlocked_ = row.C1();
        detailDests_   = row.C2();
        ShowView(L"app-detail");
    }

    void MainWindow::DelRule(int64_t id)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(), "DELETE FROM rules WHERE id=?;", -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, id);
        sqlite3_step(s); sqlite3_finalize(s);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);
        Notify(L"Rule deleted (applies live).", InfoBarSeverity::Success);
    }

    void MainWindow::AddRuleFromEvent(int64_t eventId, bool block, bool useApp, int ttlSeconds)
    {
        ng::Db d;
        if (!d.open(DbPathU8().c_str())) return;

        std::string ip, path; int port = 0;
        sqlite3_stmt* s = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "SELECT fe.remote_addr, fe.remote_port, COALESCE(pi.image_path, fe.image_path)"
            " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id=pi.id WHERE fe.id=?;",
            -1, &s, nullptr);
        sqlite3_bind_int64(s, 1, eventId);
        if (sqlite3_step(s) == SQLITE_ROW)
        {
            ip = ng::Db::ColText(s, 0);
            port = sqlite3_column_int(s, 1);
            path = ng::Db::ColText(s, 2);
        }
        sqlite3_finalize(s);

        if (!useApp && ip.find('.') == std::string::npos) { Notify(L"That row has no IPv4 destination.", InfoBarSeverity::Warning); return; }
        if (useApp && (path.size() < 3 || path[1] != ':')) { Notify(L"That row has no app path.", InfoBarSeverity::Warning); return; }

        sqlite3_stmt* ins = nullptr;
        sqlite3_prepare_v2(d.handle(),
            "INSERT INTO rules(action,app_path,remote_addr,remote_port,protocol,enabled,expires_epoch,created_at)"
            " VALUES(?,?,?,?,?,1,?,datetime('now'));", -1, &ins, nullptr);
        std::string action = block ? "block" : "permit";
        sqlite3_bind_text(ins, 1, action.c_str(), -1, SQLITE_TRANSIENT);
        if (useApp)
        {
            sqlite3_bind_text(ins, 2, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_null(ins, 3); sqlite3_bind_null(ins, 4); sqlite3_bind_null(ins, 5);
        }
        else
        {
            sqlite3_bind_null(ins, 2);
            sqlite3_bind_text(ins, 3, ip.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins, 4, port); sqlite3_bind_int(ins, 5, 6);
        }
        if (ttlSeconds > 0) sqlite3_bind_double(ins, 6, (double)time(nullptr) + ttlSeconds);
        else sqlite3_bind_null(ins, 6);
        sqlite3_step(ins); sqlite3_finalize(ins);
        sqlite3_exec(d.handle(), "UPDATE meta SET v=CAST(v AS INTEGER)+1 WHERE k='rules_gen';", nullptr, nullptr, nullptr);

        hstring what = useApp ? U8(path) : U8(ip + ":" + std::to_string(port));
        Notify((block ? L"Blocked " : L"Allowed ") + what + L" (applies live).", InfoBarSeverity::Success);
    }

    // Ask the running service to change mode. It's already up and already
    // elevated, so this needs no UAC and no new process - where these buttons used
    // to launch a whole second ngd.exe that knew nothing about the installed
    // service and fought it for the same WFP provider. If no service is installed
    // we fall back to the old foreground worker, so a service-less setup still
    // works exactly as before.
    bool MainWindow::SetMode(const char* mode, hstring const& okMsg)
    {
        bool ok = false;
        const std::string reply = ng::CmdSend(std::string("MODE ") + mode, &ok);
        if (ok)
        {
            const bool good = reply.rfind("OK", 0) == 0;
            Notify(good ? okMsg : U8(reply), good ? InfoBarSeverity::Success : InfoBarSeverity::Error);
            UpdateMode();
            return good;
        }
        return false;   // service not reachable; caller decides on a fallback
    }

    void MainWindow::OnEnforce(IInspectable const&, RoutedEventArgs const&)
    {
        if (SetMode("enforcing", L"Enforcing (default-deny + prompts).")) return;
        if (RunTool(L"ngd.exe", L"enforce \"" + NgDir() + L"\\ngpolicy.db\" 0"))
            Notify(L"Enforce started (no service installed - running in the foreground).",
                   InfoBarSeverity::Success);
    }
    void MainWindow::OnLearn(IInspectable const&, RoutedEventArgs const&)
    {
        if (SetMode("learning", L"Learning (recording baseline, enforcing nothing).")) return;
        if (RunTool(L"ngd.exe", L"record \"" + NgDir() + L"\\ngpolicy.db\""))
            Notify(L"Learn started (no service installed - running in the foreground).",
                   InfoBarSeverity::Success);
    }
    void MainWindow::OnStop(IInspectable const&, RoutedEventArgs const&)
    {
        if (SetMode("idle", L"Stopped; filters removed (failing open). Stays off across reboots."))
            return;
        StopDaemons();   // no service: kill the foreground worker instead
        Notify(L"Stopping NeuralGuard (failing open).", InfoBarSeverity::Informational);
    }
    void MainWindow::OnPanic(IInspectable const&, RoutedEventArgs const&)
    {
        // Prefer the service's own panic: a local one only rips the filters out
        // from under a daemon that keeps running and still thinks it's enforcing.
        bool ok = false;
        const std::string reply = ng::CmdSend("PANIC", &ok);
        if (ok)
        {
            Notify(L"PANIC - filters removed, enforcement off (and stays off).",
                   InfoBarSeverity::Warning);
            UpdateMode();
            return;
        }
        StopDaemons();
        if (RunTool(L"ngctl.exe", L"panic"))
            Notify(L"PANIC - all NeuralGuard filters removed.", InfoBarSeverity::Warning);
    }

    // panic() only pulls the WFP filters; the ngd worker keeps running and would
    // keep meta('mode') pinned at 'enforcing'/'learning', so the status bar lied.
    // Stop the worker(s) so Stop/Panic are honest.
    //
    // This delegates to `ngd stop` rather than killing ngd.exe by image name here,
    // because that kill was wrong for the one process it was most likely to hit:
    // if the background service is installed, its process is called ngd.exe too,
    // and terminating it looks like a crash to the SCM - whose restart-on-failure
    // policy brought it straight back up enforcing. ngd owns the distinction (SCM
    // stop for the service, kill for a foreground worker) and, being the thing
    // that actually performs the stop, is also the only thing that can honestly
    // set meta('mode')=idle afterwards - so we no longer write that here.
    void MainWindow::StopDaemons()
    {
        // --off = "and stay off": pressing Stop is a decision, so it has to outlive
        // the process. The service is auto-start, and without a persisted intent it
        // would come back enforcing at the next boot as if Stop never happened.
        RunTool(L"ngd.exe", L"stop \"" + NgDir() + L"\\ngpolicy.db\" --off");
        UpdateMode();   // the 2s tick settles it once ngd has actually finished
    }
}
