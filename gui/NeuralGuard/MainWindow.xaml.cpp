#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include "Db.h"
#include "Row.h"
#include "ColWidths.h"

#include <microsoft.ui.xaml.window.h>   // IWindowNative -> HWND for the file dialogs

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string_view>

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

        colW_ = Application::Current().Resources().Lookup(box_value(L"ColW")).as<NeuralGuard::ColWidths>();

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
            hstring t = baseHdr_[i];
            if (i == sortCol_ && !t.empty())
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
                cols.GetAt(0).Width(colW_.W0()); cols.GetAt(1).Width(colW_.W1());
                cols.GetAt(2).Width(colW_.W2()); cols.GetAt(3).Width(colW_.W3());
                cols.GetAt(4).Width(colW_.W4());
            }
        }
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
    }

    void MainWindow::RefreshCurrent()
    {
        struct RowData { int64_t id; hstring c[5]; };
        std::vector<RowData> rows;
        ng::Db d;
        bool ok = d.open(DbPathU8().c_str());

        if (curView_ == L"live" || curView_ == L"history")
        {
            SetHeaders(L"Time", L"Verdict", L"Application", L"Destination", L"Port");
            std::string sql =
                "SELECT fe.id, fe.ts_utc, fe.verdict,"
                " COALESCE(pi.signer, pi.image_path, fe.image_path),"
                " COALESCE(fe.remote_domain, fe.remote_addr), fe.remote_port"
                " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id=pi.id ";
            if (curView_ == L"history") sql += "WHERE fe.verdict LIKE '%DROP%' OR fe.verdict='BLOCK' ";
            sql += "ORDER BY fe.id DESC LIMIT 300;";
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
            if (ok) d.each("SELECT COALESCE(pi.signer,pi.image_path,fe.image_path) app,"
                           " COUNT(DISTINCT fe.remote_addr), COUNT(*),"
                           " SUM(CASE WHEN fe.verdict LIKE '%DROP%' OR fe.verdict='BLOCK' THEN 1 ELSE 0 END)"
                           " FROM flow_events fe LEFT JOIN process_identity pi ON fe.image_id=pi.id"
                           " GROUP BY app ORDER BY COUNT(*) DESC LIMIT 500;", [&](sqlite3_stmt* s) {
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
        // (the Settings view uses a form panel, not the data table)

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

        // Remember the selected row so the highlight survives the wholesale rebuild.
        int64_t selId = 0;
        if (auto sel = DataList().SelectedItem().try_as<NeuralGuard::Row>()) selId = sel.Id();

        auto items = single_threaded_observable_vector<IInspectable>();
        for (auto const& r : rows)
            items.Append(MakeRow(r.id, r.c[0], r.c[1], r.c[2], r.c[3], r.c[4]));
        DataList().ItemsSource(items);

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
    }

    void MainWindow::ShowView(hstring const& tag)
    {
        curView_ = tag;
        hstring title = L"Live";
        if (tag == L"rules") title = L"Rules";
        else if (tag == L"habits") title = L"Habits";
        else if (tag == L"apps") title = L"Per-app";
        else if (tag == L"history") title = L"History";
        else if (tag == L"flows") title = L"Flows";
        else if (tag == L"settings") title = L"Settings";
        ViewTitle().Text(title);

        bool settings = (tag == L"settings");
        HeaderCard().Visibility(settings ? Visibility::Collapsed : Visibility::Visible);
        DataList().Visibility(settings ? Visibility::Collapsed : Visibility::Visible);
        SettingsPanel().Visibility(settings ? Visibility::Visible : Visibility::Collapsed);
        SearchBox().Visibility(settings ? Visibility::Collapsed : Visibility::Visible);
        RulesTools().Visibility((tag == L"rules") ? Visibility::Visible : Visibility::Collapsed);
        filter_ = L"";
        SearchBox().Text(L"");   // reset the filter when switching views
        if (settings) LoadSettings();
        else RefreshCurrent();
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
        int a = ReadAutonomy();
        Auto0().IsChecked(a == 0);
        Auto1().IsChecked(a == 1);
        Auto2().IsChecked(a == 2);
        FeatureToggle().IsOn(MetaGet("feature_archive", "0") == "1");
        std::string mode = MetaGet("ml_mode", "shadow");
        Ml0().IsChecked(mode == "off");
        Ml1().IsChecked(mode == "shadow");
        Ml2().IsChecked(mode == "active");
        loadingSettings_ = false;
        RefreshServiceStatus();
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
    void MainWindow::OnMlModeChanged(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        auto fe = sender.try_as<FrameworkElement>();
        std::string mode = fe ? to_string(unbox_value_or<hstring>(fe.Tag(), L"shadow")) : "shadow";
        MetaSet("ml_mode", mode.c_str());
        if (mode == "active")
            Notify(L"Active scoring on. A strongly-malicious score can now demote a trusted app so it "
                   L"prompts again (it never auto-blocks). Takes effect next time enforcement runs. "
                   L"Use trained models, not the placeholders.", InfoBarSeverity::Warning);
        else
            Notify(L"ML scoring mode: " + to_hstring(mode) + L".", InfoBarSeverity::Success);
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
        if (curView_ == L"live" || curView_ == L"history") RefreshCurrent();
        else if (curView_ == L"settings") RefreshServiceStatus();   // reflect install/remove
    }

    void MainWindow::OnNavChanged(NavigationView const&, NavigationViewSelectionChangedEventArgs const& e)
    {
        if (auto item = e.SelectedItem().try_as<NavigationViewItem>())
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
        if (!row || row.Id() == 0 && curView_ != L"live" && curView_ != L"history") return;
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
        else if (curView_ == L"live" || curView_ == L"history")
        {
            int64_t id = ctxRow_.Id();
            add(L"Block this destination", [this, id] { AddRuleFromEvent(id, true, false, 0); });
            add(L"Allow this destination", [this, id] { AddRuleFromEvent(id, false, false, 0); });
            add(L"Allow this destination for 1 hour", [this, id] { AddRuleFromEvent(id, false, false, 3600); });
            add(L"Allow this app (any port)", [this, id] { AddRuleFromEvent(id, false, true, 0); });
        }
        if (menu.Items().Size() == 0) return;

        DataList().SelectedItem(row);   // highlight the row the menu acts on
        menuOpen_ = true;               // pause the live refresh so the menu isn't torn down
        menu.Closed([this](auto&&, auto&&) { menuOpen_ = false; });

        FlyoutShowOptions opt;
        opt.Position(e.GetPosition(container));
        menu.ShowAt(container, opt);
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

    void MainWindow::OnEnforce(IInspectable const&, RoutedEventArgs const&)
    {
        if (RunTool(L"ngd.exe", L"enforce \"" + NgDir() + L"\\ngpolicy.db\" 0"))
            Notify(L"Enforce started (default-deny + prompts).", InfoBarSeverity::Success);
    }
    void MainWindow::OnLearn(IInspectable const&, RoutedEventArgs const&)
    {
        if (RunTool(L"ngd.exe", L"record \"" + NgDir() + L"\\ngpolicy.db\""))
            Notify(L"Learn started (recording baseline).", InfoBarSeverity::Success);
    }
    void MainWindow::OnStop(IInspectable const&, RoutedEventArgs const&)
    {
        if (RunTool(L"ngctl.exe", L"panic"))
            Notify(L"Stopped; filters removed (failing open).", InfoBarSeverity::Informational);
    }
    void MainWindow::OnPanic(IInspectable const&, RoutedEventArgs const&)
    {
        if (RunTool(L"ngctl.exe", L"panic"))
            Notify(L"PANIC - all NeuralGuard filters removed.", InfoBarSeverity::Warning);
    }
}
