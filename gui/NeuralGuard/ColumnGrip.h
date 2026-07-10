#pragma once
#include "ColumnGrip.g.h"

#include <winrt/Microsoft.UI.Input.h>

namespace winrt::NeuralGuard::implementation
{
    struct ColumnGrip : ColumnGripT<ColumnGrip>
    {
        ColumnGrip()
        {
            // Show the horizontal-resize cursor whenever the pointer is over the grip.
            ProtectedCursor(winrt::Microsoft::UI::Input::InputSystemCursor::Create(
                winrt::Microsoft::UI::Input::InputSystemCursorShape::SizeWestEast));
        }
    };
}

namespace winrt::NeuralGuard::factory_implementation
{
    struct ColumnGrip : ColumnGripT<ColumnGrip, implementation::ColumnGrip>
    {
    };
}
