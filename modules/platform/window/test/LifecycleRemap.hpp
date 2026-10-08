#pragma once
#include "LifecycleBackend.hpp"

#define glfwInit lifecycle_test::initialize
#define glfwTerminate lifecycle_test::terminate
#define glfwVulkanSupported lifecycle_test::vulkanSupported
#define glfwCreateWindow lifecycle_test::createWindow
#define glfwDestroyWindow lifecycle_test::destroyWindow
#if defined(_WIN32)
#define SetWindowSubclass lifecycle_test::setSubclass
#define RemoveWindowSubclass lifecycle_test::removeSubclass
#endif
