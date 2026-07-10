#pragma once
#include "ColWidths.g.h"

namespace winrt::NeuralGuard::implementation
{
    struct ColWidths : ColWidthsT<ColWidths>
    {
        using GL = winrt::Microsoft::UI::Xaml::GridLength;
        using GUT = winrt::Microsoft::UI::Xaml::GridUnitType;

        ColWidths() = default;

        GL W0() { return m_w[0]; }
        void W0(GL const& v) { Set(0, v, L"W0"); }
        GL W1() { return m_w[1]; }
        void W1(GL const& v) { Set(1, v, L"W1"); }
        GL W2() { return m_w[2]; }
        void W2(GL const& v) { Set(2, v, L"W2"); }
        GL W3() { return m_w[3]; }
        void W3(GL const& v) { Set(3, v, L"W3"); }
        GL W4() { return m_w[4]; }
        void W4(GL const& v) { Set(4, v, L"W4"); }

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& h)
        {
            return m_pc.add(h);
        }
        void PropertyChanged(winrt::event_token const& t) { m_pc.remove(t); }

    private:
        void Set(int i, GL const& v, winrt::hstring const& name)
        {
            m_w[i] = v;
            m_pc(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ name });
        }

        GL m_w[5]{ GL{110, GUT::Pixel}, GL{110, GUT::Pixel}, GL{240, GUT::Pixel},
                   GL{320, GUT::Pixel}, GL{90, GUT::Pixel} };
        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_pc;
    };
}

namespace winrt::NeuralGuard::factory_implementation
{
    struct ColWidths : ColWidthsT<ColWidths, implementation::ColWidths>
    {
    };
}
