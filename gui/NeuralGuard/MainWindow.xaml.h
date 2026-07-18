#pragma once

#include "MainWindow.g.h"
#include "ColumnGrip.h"      // XAML-activated custom control; XamlTypeInfo needs the full type
#include "InsightsView.xaml.h"   // <local:InsightsView> hosted in MainWindow.xaml; same reason

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
        void OnRowDoubleTapped(winrt::Windows::Foundation::IInspectable const&,
                               winrt::Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnAppDetailBack(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
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
        void OnThemeChanged(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnMlThresholdChanged(winrt::Microsoft::UI::Xaml::Controls::NumberBox const&,
                                  winrt::Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void OnClearFlags(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnExportFeedback(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCheckUpdate(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnInstallUpdate(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        // Background check on the same cadence as OnTick (see tickCount_): once
        // ~10s after launch, then once a day for as long as the tray stays up.
        void CheckForUpdateInBackground();

        // The tray icon lives in this process now (see Tray.h). Closing the window
        // hides to tray rather than exiting - the tray is the app's real lifetime.
        void StartTray();
        void ShowFromTray();
        void ExitApp();
        // Ask the running service to switch mode. False = service not reachable,
        // so the caller can fall back to a foreground worker.
        bool SetMode(const char* mode, winrt::hstring const& okMsg);

    private:
        void DemoteApp(winrt::hstring const& appPath, int port, int proto);   // Phase 4d: manual distrust
        void RetrustApp(winrt::hstring const& appPath, int port, int proto);  // remove a demote flag
        void RemoveFlag(int64_t id);                               // delete one ml_flags row
        void SetInboundAllowed(int64_t id, bool allowed);          // permit/revoke a blocked inbound service
        void ClearMlFlags();                                       // delete all ml_flags
        HWND WindowHandle();
        void LoadSettings();
        void ApplyTheme(std::string const& theme);   // 'dark' | 'light' | 'system' -> root RequestedTheme
        void SyncThemeDependents();                  // things ThemeResource can't reach (SemBrush, caption buttons)
        void ApplyCaptionColors(bool light);
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
        void NavTo(winrt::hstring const& tag);   // select a sidebar item by tag (jump-links)
        // Drill into a Per-app row's destination breakdown. `row` is the tapped
        // apps row; its C3 is the app label and C0/C1/C2 the events/blocked/dests
        // totals, reused in the detail header so they don't need re-querying.
        void OpenAppDetail(winrt::NeuralGuard::Row const& row);
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
        // app-detail view state: the app label being drilled into, plus the
        // events/blocked/dests totals grabbed from the clicked row for the header.
        winrt::hstring detailApp_, detailEvents_, detailBlocked_, detailDests_;
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

        // Live refreshes once a second; replacing ItemsSource every tick (every
        // other view's approach) flickers, since a new ItemsSource is entirely new
        // content to WinUI even when most rows didn't change. This collection
        // persists across ticks and is mutated in place (see RefreshCurrent) -
        // liveIds_ mirrors its contents by row id so the next tick can diff
        // against it. liveItemsValid_ is cleared on every tab switch (ShowView),
        // forcing one full rebuild before incremental updates resume.
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> liveItems_{ nullptr };
        std::vector<int64_t> liveIds_;
        bool liveItemsValid_{ false };

        // OnTick fires once a second; this counts those ticks so the periodic
        // update check can ride the existing timer instead of needing its own.
        int64_t tickCount_{ 0 };

        bool themeHooked_{ false };   // ActualThemeChanged wired once (see ApplyTheme)
        bool viewReady_{ false };     // first ShowView() done; before that there's nothing to refresh
    };
}

namespace winrt::NeuralGuard::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
