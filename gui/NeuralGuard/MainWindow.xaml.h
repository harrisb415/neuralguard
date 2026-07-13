#pragma once

#include "MainWindow.g.h"
#include "ColumnGrip.h"   // XAML-activated custom control; XamlTypeInfo needs the full type

namespace winrt::NeuralGuard::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void OnEnforce(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnLearn(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnStop(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnPanic(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnNavSelect(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnRowRightTapped(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs const&);
        void OnHeaderTap(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs const&);
        void OnGripPressed(winrt::Windows::Foundation::IInspectable const&,
                           winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnGripMoved(winrt::Windows::Foundation::IInspectable const&,
                         winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnGripReleased(winrt::Windows::Foundation::IInspectable const&,
                            winrt::Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnContainerChanging(winrt::Microsoft::UI::Xaml::Controls::ListViewBase const&,
                                 winrt::Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const&);
        void OnAutonomyChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnServiceInstall(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnServiceRemove(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSearchChanged(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const&,
                             winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const&);
        void OnExportRules(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnImportRules(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnFeatureToggle(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMlModeChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMlThresholdChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const&,
                                  winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void OnClearFlags(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnExportFeedback(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCheckUpdate(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnInstallUpdate(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        void DemoteApp(winrt::hstring const& appPath, int port, int proto);   // Phase 4d: manual distrust
        void RetrustApp(winrt::hstring const& appPath, int port, int proto);  // remove a demote flag
        void RemoveFlag(int64_t id);                               // delete one ml_flags row
        void ClearMlFlags();                                       // delete all ml_flags
        void BuildDigest();                                        // Phase 4f: fill DigestText
        HWND WindowHandle();
        void LoadSettings();
        void RefreshServiceStatus();
        int  ReadAutonomy();
        void WriteAutonomy(int level);
        std::string MetaGet(const char* key, const char* dflt);
        void MetaSet(const char* key, const char* val);
        void AddRuleFromEvent(int64_t eventId, bool block, bool useApp, int ttlSeconds);
        void DelRule(int64_t ruleId);
        void ApplyHeaderText();
        void SetCols(double a, double b, double c, double d, double e);  // per-view column widths (negative = star)
        winrt::Microsoft::UI::Xaml::Controls::TextBlock HdrBlock(int i);
        double GetW(int i);
        void SetW(int i, double px);
        void OnTick(winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&);
        void ShowView(winrt::hstring const& tag);
        void RefreshCurrent();
        void SetHeaders(winrt::hstring const& h0, winrt::hstring const& h1, winrt::hstring const& h2,
                        winrt::hstring const& h3, winrt::hstring const& h4);
        void UpdateMode();
        void StopDaemons();   // terminate ngd workers + reset meta('mode') so the status bar is honest
        bool RunTool(std::wstring const& exe, std::wstring const& args);
        void Log(winrt::hstring const& line);
        void Notify(winrt::hstring const& message,
                    winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);
        std::wstring NgDir();
        std::string DbPathU8();

        winrt::Microsoft::UI::Xaml::DispatcherTimer timer_{ nullptr };
        winrt::Microsoft::UI::Xaml::DispatcherTimer toastTimer_{ nullptr };  // auto-dismiss the toast
        winrt::hstring curView_{ L"live" };
        winrt::NeuralGuard::Row ctxRow_{ nullptr };   // row the context menu acts on
        winrt::NeuralGuard::ColWidths colW_{ nullptr };  // shared, bindable column widths
        winrt::hstring baseHdr_[5];                   // header text without the sort arrow
        int  sortCol_{ -1 };                          // sorted column, -1 = unsorted
        bool sortAsc_{ true };
        int  resizeCol_{ -1 };                        // column being drag-resized, -1 = none
        double dragStartX_{ 0 }, dragStartW_{ 0 };    // drag origin (relative to ContentRoot)
        bool loadingSettings_{ false };               // suppress the autonomy handler while syncing radios
        bool navSyncing_{ false };                    // guard while clearing the other sidebar list's selection
        bool menuOpen_{ false };                      // a row context menu is open - pause the live refresh
        winrt::hstring filter_;                       // case-insensitive filter for the current table
    };
}

namespace winrt::NeuralGuard::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
