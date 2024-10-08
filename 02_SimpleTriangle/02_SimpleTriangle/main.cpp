#include <windows.h>

#include <vector>
#include <array>
#include <cassert>
#include <sstream>
#include <numeric>

#include "TriangleApp.h"

#pragma comment(lib, "vulkan-1.lib")

const int WindowWidth = 640, WindowHeight = 480;
const char* AppTitle = "SimpleTriangle";

int __stdcall wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, 0);
    auto window = glfwCreateWindow(WindowWidth, WindowHeight, AppTitle, nullptr, nullptr);

    // Vulkan 初期化
    TriangleApp theApp;
    theApp.initialize(window, AppTitle);

    while (glfwWindowShouldClose(window) == GLFW_FALSE)
    {
        glfwPollEvents();
        theApp.render();
    }

    // Vulkan 修了
    theApp.terminate();
    glfwTerminate();
    return 0;
}