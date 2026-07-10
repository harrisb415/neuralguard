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
    // Append a progress/error marker so startup failures are diagnosable without a debugger.
    static void Mark(std::string const& s)
    {
        wchar_t home[MAX_PATH]{};
        GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        std::wstring p = std::wstring(home) + L"\\NeuralGuard\\dashboard\\progress.txt";
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
            window.Activate();
            Mark("Activated");
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
