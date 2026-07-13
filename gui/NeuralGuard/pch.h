#pragma once
#include <windows.h>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot (Stop terminates ngd workers)
#include <shellapi.h>   // ShellExecuteExW (elevated tool launch)
#include <commdlg.h>    // GetOpenFileNameW / GetSaveFileNameW (rules export/import)
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")   // service control manager (Settings tab)
#pragma comment(lib, "Comdlg32.lib")   // common file dialogs (rules export/import)
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// Undefine GetCurrentTime macro to prevent
// conflict with Storyboard::GetCurrentTime
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.UI.h>
#include <wil/cppwinrt_helpers.h>

#include <string>
#include <vector>

// XAML-activated local types; XamlTypeInfo needs the full implementation types.
#include "SemBrush.h"
