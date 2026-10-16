#pragma once

#include <algorithm>
#include <cassert>
#include <vector>

namespace gf {

// Fixed-capacity FIFO: pushing onto a full buffer drops the oldest element. Index 0 is the oldest.
// Memory is allocated once per capacity change; push() never allocates.
template <typename T>
class RingBuffer {
public:
    RingBuffer() = default;
    explicit RingBuffer(int capacity) { setCapacity(capacity); }

    // Changes the capacity, keeping the newest elements that still fit.
    void setCapacity(int capacity)
    {
        capacity = std::max(0, capacity);
        if (capacity == int(m_data.size()))
            return;
        std::vector<T> next;
        next.reserve(size_t(capacity));
        const int keep = std::min(m_size, capacity);
        for (int i = m_size - keep; i < m_size; ++i)
            next.push_back(at(i));
        next.resize(size_t(capacity));
        m_data = std::move(next);
        m_start = 0;
        m_size = keep;
    }

    void push(const T& value)
    {
        if (m_data.empty())
            return;
        const int cap = capacity();
        if (m_size < cap) {
            m_data[size_t((m_start + m_size) % cap)] = value;
            ++m_size;
        } else {
            m_data[size_t(m_start)] = value;
            m_start = (m_start + 1) % cap;
        }
    }

    const T& at(int i) const
    {
        assert(i >= 0 && i < m_size);
        return m_data[size_t((m_start + i) % capacity())];
    }
    const T& last() const { return at(m_size - 1); }

    int size() const { return m_size; }
    int capacity() const { return int(m_data.size()); }
    bool isEmpty() const { return m_size == 0; }
    void clear() { m_start = 0; m_size = 0; }

private:
    std::vector<T> m_data;
    int m_start = 0;
    int m_size = 0;
};

} // namespace gf
