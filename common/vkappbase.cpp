#include "vkappbase.h"
#include <sstream>
#include <algorithm>
#include <array>

#define GetInstanceProcAddr(FuncName) \
    m_##FuncName = reinterpret_cast<PFN_##FuncName>(vkGetInstanceProcAddr(m_instance, #FuncName))

using namespace std;

static VkBool32 VKAPI_CALL DebugReportCallback(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objectTypes,
    uint64_t object,
    size_t location,
    int32_t messageCode,
    const char* pLayerPrefix,
    const char* pMessage,
    void* pUserData)
{
    VkBool32 ret = VK_FALSE;
    if (flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT ||
        flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT)
    {
        ret = VK_TRUE;
    }

    std::stringstream ss;
    if (pLayerPrefix)
    {
        ss << "[" << pLayerPrefix << "]";
    }
    ss << pMessage << std::endl;

    const char* report = ss.str().c_str();
    wchar_t wideReport[256];
    MultiByteToWideChar(CP_UTF8, 0, report, -1, wideReport, 256);
    OutputDebugString(wideReport);

    return ret;
}

void VulkanAppBase::checkResult(VkResult result)
{
    if (result != VK_SUCCESS)
    {
        DebugBreak();
    }
}

VulkanAppBase::VulkanAppBase()
    : m_presentMode(VK_PRESENT_MODE_FIFO_KHR)
    , m_imageIndex(0)
{
}

void VulkanAppBase::initialize(GLFWwindow* window, const char* appName)
{
    // Vulkan インスタンスの生成
    initializeInstance(appName);

    // 物理デバイスの選択
    selectPhysicalDevice();
    m_graphicsQueueIndex = searchGraphicsQueueIndex();

#ifdef _DEBUG
    // デバッグレポート関数のセット
    enableDebugReport();
#endif

    // 論理デバイスの生成
    createDevice();

    // コマンドプールの準備
    prepareCommandPool();

    // サーフェース生成
    glfwCreateWindowSurface(m_instance, window, nullptr, &m_surface);

    // サーフェースのフォーマット情報選択
    selectSurfaceFormat(VK_FORMAT_B8G8R8A8_UNORM);

    // サーフェースの能力値情報取得
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physDev, m_surface, &m_surfaceCaps);
    VkBool32 isSupport;
    vkGetPhysicalDeviceSurfaceSupportKHR(m_physDev, m_graphicsQueueIndex, m_surface, &isSupport);

    // スワップチェイン生成
    createSwapchain(window);

    // デプスバッファ生成
    createDepthBuffer();

    // スワップチェインイメージとデプスバッファへの ImageView を生成
    createViews();

    // レンダーパスの生成
    createRenderPass();

    // フレームバッファの生成
    createFramebuffer();

    // コマンドバッファの準備
    prepareCommandBuffers();

    // 描画フレーム同期用
    prepareSemaphores();

    prepare();
}

void VulkanAppBase::terminate()
{
    vkDeviceWaitIdle(m_device);

    cleanup();

    vkFreeCommandBuffers(m_device, m_commandPool, uint32_t(m_commands.size()), m_commands.data());
    m_commands.clear();

    vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    for (auto& v : m_framebuffers)
    {
        vkDestroyFramebuffer(m_device, v, nullptr);
    }
    m_swapchainImages.clear();
    vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);

    for (auto& v : m_fences)
    {
        vkDestroyFence(m_device, v, nullptr);
    }
    m_fences.clear();
    vkDestroySemaphore(m_device, m_presentCompletedSem, nullptr);
    vkDestroySemaphore(m_device, m_renderCompletedSem, nullptr);

    vkDestroyCommandPool(m_device, m_commandPool, nullptr);

    vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    vkDestroyDevice(m_device, nullptr);

#ifdef _DEBUG
    disableDebugReport();
#endif
    vkDestroyInstance(m_instance, nullptr);
}

void VulkanAppBase::initializeInstance(const char* appName)
{
    vector<const char*> extensions;
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = appName;
    appInfo.pEngineName = appName;
    appInfo.apiVersion = VK_API_VERSION_1_1;
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);

    // 拡張情報の取得
    vector<VkExtensionProperties> props;
    {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        props.resize(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());

        for (const auto& v : props)
        {
            extensions.push_back(v.extensionName);
        }
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.enabledExtensionCount = uint32_t(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.pApplicationInfo = &appInfo;
#ifdef _DEBUG
    // デバッグビルド時には検証レイヤーを有効化
    const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
    if (VK_HEADER_VERSION_COMPLETE < VK_MAKE_VERSION(1, 1, 106))
    {
        // "VK_LAYER_LUNARG_standard_validation" は廃止になっているが昔の Vulkan SDK では動くので対処しておく
        layers[0] = "VK_LAYER_LUNARG_standard_validation";
    }
    ci.enabledLayerCount = 1;
    ci.ppEnabledLayerNames = layers;
#endif

    // インスタンス生成
    auto result = vkCreateInstance(&ci, nullptr, &m_instance);
    checkResult(result);
}

void VulkanAppBase::selectPhysicalDevice()
{
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &devCount, nullptr);
    vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(m_instance, &devCount, physDevs.data());

    // 最初のデバイスを使用する
    m_physDev = physDevs[0];

    // メモリプロパティを所得しておく
    vkGetPhysicalDeviceMemoryProperties(m_physDev, &m_physMemProps);
}

uint32_t VulkanAppBase::searchGraphicsQueueIndex()
{
    uint32_t propCount;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physDev, &propCount, nullptr);
    vector<VkQueueFamilyProperties> props(propCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physDev, &propCount, props.data());

    uint32_t graphicsQueue = ~0u;
    for (uint32_t i = 0; i < propCount; ++i)
    {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            graphicsQueue = i;
            break;
        }
    }
    return graphicsQueue;
}

void VulkanAppBase::createDevice()
{
    const float defaultQueuePriority(1.0f);
    VkDeviceQueueCreateInfo devQueueCI {};
    devQueueCI.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    devQueueCI.queueFamilyIndex = m_graphicsQueueIndex;
    devQueueCI.queueCount = 1;
    devQueueCI.pQueuePriorities = &defaultQueuePriority;

    vector<VkExtensionProperties> devExtProps;
    {
        // 拡張情報の取得
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(m_physDev, nullptr, &count, nullptr);
        devExtProps.resize(count);
        vkEnumerateDeviceExtensionProperties(m_physDev, nullptr, &count, devExtProps.data());
    }

    vector<const char*> extensions;
    for (const auto& v : devExtProps)
    {
        extensions.push_back(v.extensionName);
    }
    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pQueueCreateInfos = &devQueueCI;
    ci.queueCreateInfoCount = 1;
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledExtensionCount = uint32_t(extensions.size());

    auto result = vkCreateDevice(m_physDev, &ci, nullptr, &m_device);
    checkResult(result);

    // デバイスキューの取得
    vkGetDeviceQueue(m_device, m_graphicsQueueIndex, 0, &m_deviceQueue);
}

void VulkanAppBase::prepareCommandPool()
{
    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = m_graphicsQueueIndex;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    auto result = vkCreateCommandPool(m_device, &ci, nullptr, &m_commandPool);
    checkResult(result);
}

void VulkanAppBase::selectSurfaceFormat(VkFormat format)
{
    uint32_t surfaceFormatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDev, m_surface, &surfaceFormatCount, nullptr);
    vector<VkSurfaceFormatKHR> formats(surfaceFormatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physDev, m_surface, &surfaceFormatCount, formats.data());

    // 検索して一致するフォーマットを見つける
    for (const auto& f : formats)
    {
        if (f.format == format)
        {
            m_surfaceFormat = f;
        }
    }
}

/// <summary>
/// スワップチェインを生成
/// </summary>
/// <param name="window">生成対象となる GLFWwindow?</param>
void VulkanAppBase::createSwapchain(GLFWwindow* window)
{
    // NOTE: (std::max)(...) という記述は、マクロでも「max」というものがあるようで、それと誤認してプリプロセッサが置換してしまうのを防ぐ目的のよう。
    //       つまり、本来なら std::max(2f, ...) と書けるところを、予防措置として (std::max)() としている。
    // ----
    // ここでは、生成した VkSurfaceCapabilitiesKHR オブジェクトの持っている Image Count か、最大値を 2u としてカウントを計算。
    //
    // NOTE: ここでの Image は VkImage か？
    auto imageCount = (std::max)(2u, m_surfaceCaps.minImageCount);
    auto extent = m_surfaceCaps.currentExtent;

    // 値が無効な場合はウィンドウサイズを使用する
    if (extent.width == ~0u)
    {
        int width, height;
        glfwGetWindowSize(window, &width, &height);
        extent.width = uint32_t(width);
        extent.height = uint32_t(height);
    }

    // 生成するスワップチェインの情報を作成。
    // 基本的には VkSurfaceFormatKHR と VkSurfaceCapabilitiesKHR が持っている情報を元に作成する。
    uint32_t queueFamilyIndices[] = { m_graphicsQueueIndex };
    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = m_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = m_surfaceFormat.format;
    ci.imageColorSpace = m_surfaceFormat.colorSpace;
    ci.imageExtent = extent;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = m_surfaceCaps.currentTransform;
    ci.imageArrayLayers = 1;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.queueFamilyIndexCount = 0;
    ci.presentMode = m_presentMode;
    ci.oldSwapchain = VK_NULL_HANDLE;
    ci.clipped = VK_TRUE;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    // 対象デバイス（m_devie）を使ってスワップチェインの作成
    auto result = vkCreateSwapchainKHR(m_device, &ci, nullptr, &m_swapchain);
    checkResult(result);
    m_swapchainExtent = extent;
}

/// <summary>
/// デプスバッファを生成
/// </summary>
void VulkanAppBase::createDepthBuffer()
{
    // デプスバッファのための生成情報
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;

    // バッファのタイプは 2D
    ci.imageType = VK_IMAGE_TYPE_2D;

    // フォーマットは SFloat 32
    ci.format = VK_FORMAT_D32_SFLOAT;
    ci.extent.width = m_swapchainExtent.width;
    ci.extent.height = m_swapchainExtent.height;
    ci.extent.depth = 1;

    // NOTE: DepthBuffer は Stencil 的なアタッチメントということ？
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.arrayLayers = 1;

    // 上記情報を元に VkImage を生成
    auto result = vkCreateImage(m_device, &ci, nullptr, &m_depthBuffer);
    checkResult(result);

    // NOTE: おそらく上記は「デプスバッファの枠」として VkImage を生成したのみで、
    //       デバイス上のメモリ確保は別に行う必要があると思われる。

    // 上記の VkImage のためのメモリ確保を行う
    VkMemoryRequirements reqs;

    // 対象デバイス（m_device）に対して、VkImage に必要なメモリサイズをリクエスト
    vkGetImageMemoryRequirements(m_device, m_depthBuffer, &reqs);

    // メモリ確報用情報を作成
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;

    // 取得したメモリサイズを設定
    ai.allocationSize = reqs.size;

    // NOTE: このインデックスは、どのメモリ位置でデータ（リソース）を割り当てるかを指定するもの。
    ai.memoryTypeIndex = getMemoryTypeIndex(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // 実際にメモリを確保する
    vkAllocateMemory(m_device, &ai, nullptr, &m_depthBufferMemory);

    // 確保したメモリを VkImage にバインドする
    // NOTE: OpenGL でもテクスチャ生成したあとにバインドしていたものの、さらに低レイヤーな操作？
    vkBindImageMemory(m_device, m_depthBuffer, m_depthBufferMemory, 0);
}

/// <summary>
/// スワップチェイン、デプスバッファの VkImage を生成する
/// </summary>
void VulkanAppBase::createViews()
{
    // VkSwapchainKHR が利用する VkImage を取得する
    uint32_t imageCount;
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, nullptr);
    m_swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &imageCount, m_swapchainImages.data());
    m_swapchainViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i)
    {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = m_surfaceFormat.format;
        ci.components = {
            VK_COMPONENT_SWIZZLE_R,
            VK_COMPONENT_SWIZZLE_G,
            VK_COMPONENT_SWIZZLE_B,
            VK_COMPONENT_SWIZZLE_A,
        };
        ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1, };
        ci.image = m_swapchainImages[i];
        
        auto result = vkCreateImageView(m_device, &ci, nullptr, &m_swapchainViews[i]);
        checkResult(result);
    }

    // for depth buffer
    {
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = VK_FORMAT_D32_SFLOAT;
        ci.components = {
            VK_COMPONENT_SWIZZLE_R,
            VK_COMPONENT_SWIZZLE_G,
            VK_COMPONENT_SWIZZLE_B,
            VK_COMPONENT_SWIZZLE_A,
        };
        ci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1, };
        ci.image = m_depthBuffer;

        auto result = vkCreateImageView(m_device, &ci, nullptr, &m_depthBufferView);
        checkResult(result);
    }
}

/// <summary>
/// レンダーパスを生成
/// </summary>
void VulkanAppBase::createRenderPass()
{
    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;

    // Color アタッチメントと Depth アタッチメントの Description を生成する
    array<VkAttachmentDescription, 2> attachments;
    auto& colorTarget = attachments[0];
    auto& depthTarget = attachments[1];

    colorTarget = VkAttachmentDescription{};
    colorTarget.format = m_surfaceFormat.format;
    colorTarget.samples = VK_SAMPLE_COUNT_1_BIT;
    colorTarget.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorTarget.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorTarget.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorTarget.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorTarget.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorTarget.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    depthTarget = VkAttachmentDescription{};
    depthTarget.format = VK_FORMAT_D32_SFLOAT;
    depthTarget.samples = VK_SAMPLE_COUNT_1_BIT;
    depthTarget.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthTarget.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthTarget.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthTarget.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthTarget.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthTarget.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    // 上記アタッチメントの Description のリファレンスを生成
    // attachment フィールドは上記の配列の何番目の情報かを示していると思われる
    VkAttachmentReference colorReference{}, depthReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    depthReference.attachment = 1;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpassDesc{};
    subpassDesc.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDesc.colorAttachmentCount = 1;
    subpassDesc.pColorAttachments = &colorReference;
    subpassDesc.pDepthStencilAttachment = &depthReference;

    ci.attachmentCount = uint32_t(attachments.size());
    ci.pAttachments = attachments.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &subpassDesc;

    auto result = vkCreateRenderPass(m_device, &ci, nullptr, &m_renderPass);
    checkResult(result);
}

/// <summary>
/// フレームバッファを生成する
/// </summary>
void VulkanAppBase::createFramebuffer()
{
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = m_renderPass;
    ci.width = m_swapchainExtent.width;
    ci.height = m_swapchainExtent.height;
    ci.layers = 1;
    m_framebuffers.clear();
    for (auto& v : m_swapchainViews)
    {
        // 各スワップチェイン内の VkImageView それぞれにフレームバッファを作成する
        array<VkImageView, 2> attachments;
        ci.attachmentCount = uint32_t(attachments.size());
        ci.pAttachments = attachments.data();

        // アタッチメントはスワップチェイン内の VkImageView がカラーアタッチメント、
        // Depth buffer のものがデプスバッファアタッチメント？
        attachments[0] = v;
        attachments[1] = m_depthBufferView;

        VkFramebuffer framebuffer;
        auto result = vkCreateFramebuffer(m_device, &ci, nullptr, &framebuffer);
        checkResult(result);
        m_framebuffers.push_back(framebuffer);
    }
}

/// <summary>
/// コマンドバッファの準備（描画コマンド）
/// </summary>
void VulkanAppBase::prepareCommandBuffers()
{
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = m_commandPool;
    ai.commandBufferCount = uint32_t(m_swapchainViews.size());
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    m_commands.resize(ai.commandBufferCount);

    auto result = vkAllocateCommandBuffers(m_device, &ai, m_commands.data());
    checkResult(result);

    // コマンドバッファのフェンスも同数用意する
    // フェンスは CPU-GPU 間の同期に利用する
    m_fences.resize(ai.commandBufferCount);
    VkFenceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& v : m_fences)
    {
        result = vkCreateFence(m_device, &ci, nullptr, &v);
        checkResult(result);
    }
}

/// <summary>
/// セマフォの生成
/// </summary>
void VulkanAppBase::prepareSemaphores()
{
    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(m_device, &ci, nullptr, &m_renderCompletedSem);
    vkCreateSemaphore(m_device, &ci, nullptr, &m_presentCompletedSem);
}

uint32_t VulkanAppBase::getMemoryTypeIndex(uint32_t requestBits, VkMemoryPropertyFlags requestProps) const
{
    uint32_t result = ~0u;
    for (uint32_t i = 0; i < m_physMemProps.memoryTypeCount; ++i)
    {
        if (requestBits & 1)
        {
            const auto& types = m_physMemProps.memoryTypes[i];
            if ((types.propertyFlags & requestProps) == requestProps)
            {
                result = i;
                break;
            }
        }
        requestBits >>= 1;
    }

    return result;
}

/// <summary>
/// デバッグレポートを有効化
/// </summary>
void VulkanAppBase::enableDebugReport()
{
    // GetInstanceProcAddr の定義は以下
    // #define GetInstanceProcAddr(FuncName) \
    //    m_##FuncName = reinterpret_cast<PFN_##FuncName>(vkGetInstanceProcAddr(m_instance, #FuncName))
    // 実際に展開されると以下のような形になる
    // 
    // m_vkCreateDebugReportCallbackEXT = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(m_instance, "vkCreateDebugReportCallbackEXT"));
    // 
    // Memo: ##FuncName はトークン結合演算子で、前の文字と結合してトークン（式・文など）に変換される（e.g. m_hoge と、m_ とひとつになる）。一方、#FuncName は「文字列」に変換される。
    GetInstanceProcAddr(vkCreateDebugReportCallbackEXT);
    GetInstanceProcAddr(vkDebugReportMessageEXT);
    GetInstanceProcAddr(vkDestroyDebugReportCallbackEXT);

    VkDebugReportFlagsEXT flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;

    VkDebugReportCallbackCreateInfoEXT drcCI{};
    drcCI.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    drcCI.flags = flags;
    drcCI.pfnCallback = &DebugReportCallback;
    m_vkCreateDebugReportCallbackEXT(m_instance, &drcCI, nullptr, &m_debugReport);
}

/// <summary>
/// デバッグレポートを無効化
/// </summary>
void VulkanAppBase::disableDebugReport()
{
    if (m_vkDestroyDebugReportCallbackEXT)
    {
        m_vkDestroyDebugReportCallbackEXT(m_instance, m_debugReport, nullptr);
    }
}

void VulkanAppBase::render()
{
    uint32_t nextImageIndex = 0;
    vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, m_presentCompletedSem, VK_NULL_HANDLE, &nextImageIndex);
    auto commandFence = m_fences[nextImageIndex];
    vkWaitForFences(m_device, 1, &commandFence, VK_TRUE, UINT64_MAX);

    // クリア値
    array<VkClearValue, 2> clearValue = {
        {
            {0.5f, 0.25f, 0.25f, 0.0f},  // for Color
            {1.0f, 0} // for Depth
        }
    };

    VkRenderPassBeginInfo renderPassBI{};
    renderPassBI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBI.renderPass = m_renderPass;
    renderPassBI.framebuffer = m_framebuffers[nextImageIndex];
    renderPassBI.renderArea.offset = VkOffset2D { 0, 0 };
    renderPassBI.renderArea.extent = m_swapchainExtent;
    renderPassBI.pClearValues = clearValue.data();
    renderPassBI.clearValueCount = uint32_t(clearValue.size());

    // コマンドバッファ・レンダーパス開始
    VkCommandBufferBeginInfo commandBI{};
    commandBI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    auto& command = m_commands[nextImageIndex];
    vkBeginCommandBuffer(command, &commandBI);
    vkCmdBeginRenderPass(command, &renderPassBI, VK_SUBPASS_CONTENTS_INLINE);

    m_imageIndex = nextImageIndex;
    makeCommand(command);

    // コマンド・レンダーパス修了
    vkCmdEndRenderPass(command);
    vkEndCommandBuffer(command);

    // コマンドを実行（送信）
    VkSubmitInfo submitInfo{};
    VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command;
    submitInfo.pWaitDstStageMask = &waitStageMask;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &m_presentCompletedSem;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &m_renderCompletedSem;
    vkResetFences(m_device, 1, &commandFence);
    vkQueueSubmit(m_deviceQueue, 1, &submitInfo, commandFence);

    // Present 処理
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain;
    presentInfo.pImageIndices = &nextImageIndex;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_renderCompletedSem;
    vkQueuePresentKHR(m_deviceQueue, &presentInfo);
}
