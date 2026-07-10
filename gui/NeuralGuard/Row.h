#pragma once
#include "Row.g.h"

namespace winrt::NeuralGuard::implementation
{
    struct Row : RowT<Row>
    {
        Row(int64_t id, hstring const& c0, hstring const& c1, hstring const& c2,
            hstring const& c3, hstring const& c4)
            : m_id(id), m_c0(c0), m_c1(c1), m_c2(c2), m_c3(c3), m_c4(c4) {}

        int64_t Id() const { return m_id; }
        hstring C0() const { return m_c0; }
        hstring C1() const { return m_c1; }
        hstring C2() const { return m_c2; }
        hstring C3() const { return m_c3; }
        hstring C4() const { return m_c4; }

    private:
        int64_t m_id;
        hstring m_c0, m_c1, m_c2, m_c3, m_c4;
    };
}

namespace winrt::NeuralGuard::factory_implementation
{
    struct Row : RowT<Row, implementation::Row>
    {
    };
}
