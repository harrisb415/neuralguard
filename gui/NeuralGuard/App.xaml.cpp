#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "ColWidths.h"

#include <cstdio>
#include <string>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::NeuralGuard::implementation
{
    // Append a progress/error marker so startup failures are diagnosable without a
    // debugger. Written next to the running exe rather than a fixed
    // %USERPROFILE%\NeuralGuard - the app can be installed per-machine now (see
    // installer/NeuralGuard.iss), and a hardcoded per-user path would silently
    // write to a folder that doesn't even exist for that install, breaking the
    // one diagnostic this app has for a startup that fails before any UI shows.
    static void Mark(std::string const& s)
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir(exePath);
        size_t slash = dir.find_last_of(L"\\/");
        dir = (slash == std::wstring::npos) ? L"." : dir.substr(0, slash);
        std::wstring p = dir + L"\\progress.txt";
        HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            SetFilePointer(h, 0, nullptr, FILE_END);
            std::string line = s + "\r\n";
            DWORD w = 0;
            WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
            CloseHandle(h);
        }
    }

    App::App()
    {
        Mark("App ctor");
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            Mark("UnhandledException: " + winrt::to_string(e.Message()));
        });
    }

    void App::OnLaunched([[maybe_unused]] LaunchActivatedEventArgs const& e)
    {
        Mark("OnLaunched enter");
        try
        {
            // Shared column widths, resolved by {StaticResource ColW} in the window.
            Resources().Insert(box_value(L"ColW"), winrt::make<NeuralGuard::implementation::ColWidths>());
            window = make<MainWindow>();
            Mark("MainWindow made");
            // Mica backdrop (dark, per the app RequestedTheme). Guarded on its own:
            // an unsupported/failed backdrop must not skip Activate().
            try
            {
                window.SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
                Mark("Mica set");
            }
            catch (...) { Mark("Mica unavailable"); }
            window.Activate();
            Mark("Activated");

            // `--tray` = started at login: come up as just a tray icon, no window.
            // Activate() has to run first (it's what realises the window and gets
            // the tray icon registered), so this hides it immediately after rather
            // than never showing it. Closing the window later hides it the same way.
            if (wcsstr(GetCommandLineW(), L"--tray"))
            {
                window.AppWindow().Hide();
                Mark("Hidden to tray");
            }
        }
        catch (winrt::hresult_error const& ex)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%08X", (uint32_t)ex.code());
            Mark(std::string("hresult_error ") + buf + ": " + winrt::to_string(ex.message()));
        }
        catch (std::exception const& ex)
        {
            Mark(std::string("std::exception: ") + ex.what());
        }
        catch (...)
        {
            Mark("unknown exception");
        }
    }
}
