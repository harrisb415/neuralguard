#pragma once

#include "InsightsView.g.h"

#include <functional>
#include <string>

namespace winrt::NeuralGuard::implementation
{
    struct InsightsView : InsightsViewT<InsightsView>
    {
        InsightsView() { InitializeComponent(); }

        // Rebuild every card from the policy DB at `dbPath`. Called by MainWindow
        // (via winrt::get_self) each time the Insights tab is shown. Read-only.
        void Refresh(std::string const& dbPath);

        // MainWindow injects how a jump-link navigates (it owns the sidebar). The
        // control just calls this with a nav tag; it never touches MainWindow directly.
        void SetNavigate(std::function<void(winrt::hstring)> f) { navigate_ = std::move(f); }

        // Jump-link Click handlers (wired in InsightsView.xaml).
        void OnEditThresholds(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnViewFlags(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnViewFlows(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        std::function<void(winrt::hstring)> navigate_;
    };
}

namespace winrt::NeuralGuard::factory_implementation
{
    struct InsightsView : InsightsViewT<InsightsView, implementation::InsightsView>
    {
    };
}
