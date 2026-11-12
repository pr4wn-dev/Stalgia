#pragma once

#include <utility>

// Tiny COM pointer. MinGW has no WRL; keep the rest of the code free of ATL.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : m_ptr(p) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    void reset() {
        if (m_ptr) {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

    T** put() {
        reset();
        return &m_ptr;
    }

    void** put_void() { return reinterpret_cast<void**>(put()); }

    T* get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }
    operator T*() const { return m_ptr; }
    explicit operator bool() const { return m_ptr != nullptr; }

    T* detach() {
        T* p = m_ptr;
        m_ptr = nullptr;
        return p;
    }

private:
    T* m_ptr = nullptr;
};
