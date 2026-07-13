#pragma once
#include "Row.g.h"

namespace winrt::NeuralGuard::implementation
{
    struct Row : RowT<Row>
    {
        Row(int64_t id, hstring const& c0, hstring const& c1, hstring const& c2,
            hstring const& c3, hstring const& c4);

        int64_t Id() const { return m_id; }
        hstring C0() const { return m_c0; }
        hstring C1() const { return m_c1; }
        hstring C2() const { return m_c2; }
        hstring C3() const { return m_c3; }
        hstring C4() const { return m_c4; }

        winrt::Microsoft::UI::Xaml::Media::Brush C0Fg() const { return m_c0Fg; }
        winrt::Microsoft::UI::Xaml::Media::Brush C0Bg() const { return m_c0Bg; }
        winrt::Microsoft::UI::Xaml::Media::Brush C0Bd() const { return m_c0Bd; }
        winrt::Microsoft::UI::Xaml::Media::Brush C1Fg() const { return m_c1Fg; }
        winrt::Microsoft::UI::Xaml::Media::Brush C1Bg() const { return m_c1Bg; }
        winrt::Microsoft::UI::Xaml::Media::Brush C1Bd() const { return m_c1Bd; }
        winrt::Microsoft::UI::Xaml::Media::Brush C2Fg() const { return m_c2Fg; }
        winrt::Microsoft::UI::Xaml::Media::Brush C2Bg() const { return m_c2Bg; }
        winrt::Microsoft::UI::Xaml::Media::Brush C2Bd() const { return m_c2Bd; }
        winrt::Microsoft::UI::Xaml::Media::Brush C1BlockedFg() const { return m_c1Blocked; }
        winrt::Microsoft::UI::Xaml::Media::Brush C3AnomFg() const { return m_c3Anom; }
        winrt::Microsoft::UI::Xaml::Media::Brush C4MalFg() const { return m_c4Mal; }

    private:
        int64_t m_id;
        hstring m_c0, m_c1, m_c2, m_c3, m_c4;
        winrt::Microsoft::UI::Xaml::Media::Brush m_c0Fg{ nullptr }, m_c0Bg{ nullptr }, m_c0Bd{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Brush m_c1Fg{ nullptr }, m_c1Bg{ nullptr }, m_c1Bd{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Brush m_c2Fg{ nullptr }, m_c2Bg{ nullptr }, m_c2Bd{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Brush m_c1Blocked{ nullptr };
        winrt::Microsoft::UI::Xaml::Media::Brush m_c3Anom{ nullptr }, m_c4Mal{ nullptr };
    };
}

namespace winrt::NeuralGuard::factory_implementation
{
    struct Row : RowT<Row, implementation::Row>
    {
    };
}
