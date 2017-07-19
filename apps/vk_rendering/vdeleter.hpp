#ifndef VDELETER_HPP
#define VDELETER_HPP

#include <functional>
#include <iostream>
#include <vulkan/vulkan.h>


template <typename T>
class VDeleter {
public:
    VDeleter() : VDeleter([](T _) {}) {}

    VDeleter(VDeleter&& other) {
      m_object = other.m_object;
      m_deleter = other.m_deleter;
      other.m_object = VK_NULL_HANDLE;
    }

    VDeleter& operator=(VDeleter&& other) {
      cleanup();
      m_object = other.m_object;
      other.m_object = VK_NULL_HANDLE;
      return *this;
    }

    VDeleter(std::function<void(T, VkAllocationCallbacks*)> deletef) {
        this->m_deleter = [=](T obj) { deletef(obj, nullptr); };
    }

    VDeleter(const VDeleter<VkInstance>& instance, std::function<void(VkInstance, T, VkAllocationCallbacks*)> deletef) {
        this->m_deleter = [&instance, deletef](T obj) { deletef(instance, obj, nullptr); };
    }

    VDeleter(const VDeleter<VkDevice>& device, std::function<void(VkDevice, T, VkAllocationCallbacks*)> deletef) {
        this->m_deleter = [&device, deletef](T obj) { deletef(device, obj, nullptr); };
    }

    ~VDeleter() {
        cleanup();
    }

    T* operator &() {
        cleanup();
        return &m_object;
    }

    operator T() const {
        return m_object;
    }

    T m_object{VK_NULL_HANDLE};
    std::function<void(T)> m_deleter;

    void cleanup() {
        if (m_object != VK_NULL_HANDLE) {
            m_deleter(m_object);
        }
        m_object = VK_NULL_HANDLE;
    }

    void reset(T h = VK_NULL_HANDLE)
    {
      using std::swap;
    }

    void
    swap(VDeleter& other) noexcept
    {
      using std::swap;
      swap(m_object, other.m_object);
      swap(m_deleter, other.m_deleter);
    }

    // disable copy from lvalue
    VDeleter(const VDeleter&) = delete;
    VDeleter& operator=(const VDeleter&) = delete;
};

#endif // VDELETER_HPP
