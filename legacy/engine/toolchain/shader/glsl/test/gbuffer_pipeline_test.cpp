// ============================================================================
//  gbuffer_pipeline_test.cpp  —  G2 headless pipeline proof (GLSL backend)
//
//  Proves the material-graph-GENERATED (via GLSL->shaderc) GBuffer fragment
//  shader is a real, Vulkan-pipeline-valid 3-MRT gbuffer shader: it builds a
//  graphics pipeline (passthrough vertex + generated frag, dynamic rendering,
//  3 color attachments matching the engine gbuffer layout) under the Khronos
//  validation layers.
//
//  This is the integration check spirv-val cannot give: the driver + validation
//  layers verify the frag's 3 outputs against 3 color attachments AND the
//  vertex->fragment interface (the generated frag reads the interpolated
//  vWorldNormal plus the fixed transition contract, so the passthrough vert
//  outputs locations 0/1/3/5 and 7/8/9). Raw Vulkan only (no render module).
//
//  Self-checking: 0 = PASS (or skip if no Vulkan device), 1 = FAIL.
// ============================================================================

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include "graph_test_helpers.hpp"

using namespace lux::rdesc;
namespace rdesc = lux::rdesc;

// Passthrough vertex shader that writes the optional graph interpolants plus
// the mandatory transition contract. Regenerate the checked-in word array from
// gbuffer_passthrough_fixture.vert with `glslc --target-env=vulkan1.3 -mfmt=c`.
static const uint32_t kPassthroughVert[] =
#include "gbuffer_passthrough_fixture.spv.inc"
;

static VkInstance g_instance = VK_NULL_HANDLE;
static VkDevice   g_device   = VK_NULL_HANDLE;
static bool       g_validation_error = false;

static VKAPI_ATTR VkBool32 VKAPI_CALL dbgCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        std::fprintf(stderr, "  [validation] %s\n", data->pMessage ? data->pMessage : "");
        g_validation_error = true;
    }
    return VK_FALSE;
}

static bool hasValidationLayer()
{
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> ls(n);
    vkEnumerateInstanceLayerProperties(&n, ls.data());
    for (const auto& l : ls)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            return true;
    return false;
}

static bool initVulkan()
{
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_3;

    const bool        vl       = hasValidationLayer();
    const char* const layers[] = { "VK_LAYER_KHRONOS_validation" };
    const char* const exts[]   = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    if (vl)
    {
        ici.enabledLayerCount       = 1;
        ici.ppEnabledLayerNames     = layers;
        ici.enabledExtensionCount   = 1;
        ici.ppEnabledExtensionNames = exts;
    }
    if (vkCreateInstance(&ici, nullptr, &g_instance) != VK_SUCCESS)
        return false;

    if (vl)
    {
        auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(g_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (fn)
        {
            VkDebugUtilsMessengerCreateInfoEXT ci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            ci.pfnUserCallback = dbgCallback;
            VkDebugUtilsMessengerEXT m{};
            fn(g_instance, &ci, nullptr, &m);
        }
    }

    uint32_t pc = 0;
    vkEnumeratePhysicalDevices(g_instance, &pc, nullptr);
    if (pc == 0)
        return false;
    std::vector<VkPhysicalDevice> pds(pc);
    vkEnumeratePhysicalDevices(g_instance, &pc, pds.data());
    VkPhysicalDevice pd = pds[0];

    uint32_t qfc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qfc);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qf.data());
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qfc; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }

    const float             pri = 1.0f;
    VkDeviceQueueCreateInfo q{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    q.queueFamilyIndex = qfi;
    q.queueCount       = 1;
    q.pQueuePriorities = &pri;

    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;  // pipeline uses dynamic rendering (renderPass = null)

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext                = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &q;
    return vkCreateDevice(pd, &dci, nullptr, &g_device) == VK_SUCCESS;
}

static VkShaderModule makeModule(const uint32_t* words, size_t byte_count)
{
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = byte_count;
    ci.pCode    = words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_device, &ci, nullptr, &m);
    return m;
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== material_graph_glsl gbuffer_pipeline_test (G2) ===\n");

    if (!initVulkan())
    {
        std::printf("No Vulkan device. Skipping.\n");
        return 0;
    }

    int  fails = 0;
    auto check = [&](bool c, const char* n)
    { std::printf(c ? "  [PASS] %s\n" : "  [FAIL] %s\n", n); if (!c) ++fails; };

    // 1. Constant material -> GBuffer SPIR-V (no descriptor/param data needed).
    MaterialGraph g;
    node_id cid = g.addNode(std::make_unique<ConstantNode>());
    {
        auto* cn = static_cast<ConstantNode*>(g.node(cid));
        cn->value_type = rdesc::EMatValueType::Vec3;
        cn->value[0] = 0.3f; cn->value[1] = 0.5f; cn->value[2] = 0.7f;
    }
    node_id oid = g.addNode(std::make_unique<OutputSurfaceNode>());
    g.connect(cid, 0, oid, static_cast<uint32_t>(rdesc::EMaterialAttribute::BaseColor));

    auto cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::GBuffer);
    check(static_cast<bool>(cs), "compileGraph(GBuffer)");
    if (!cs || cs->spirv.empty())
    { std::printf("no spirv generated: %s\n", cs ? "" : cs.error().c_str()); return 1; }
    std::vector<uint32_t> frag = std::move(cs->spirv);

    // 2. Shader modules (driver-level acceptance).
    VkShaderModule vs = makeModule(kPassthroughVert, sizeof(kPassthroughVert));
    VkShaderModule fs = makeModule(frag.data(), frag.size() * sizeof(uint32_t));
    check(vs != VK_NULL_HANDLE, "vkCreateShaderModule(passthrough vert)");
    check(fs != VK_NULL_HANDLE, "vkCreateShaderModule(generated gbuffer frag)");

    // 3. Empty pipeline layout (a constant material binds no descriptors / push constants).
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(g_device, &plci, nullptr, &layout);

    // 4. Graphics pipeline: dynamic rendering, 3 color attachments (gbuffer MRT).
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo   vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba[3]{};
    for (auto& a : cba)
        a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 3; cb.pAttachments = cba;

    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

    // Engine gbuffer MRT layout: AlbedoMetallic / NormalRoughness / EmissiveAo.
    const VkFormat colorFormats[3] = {
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM
    };
    VkPipelineRenderingCreateInfo prci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    prci.colorAttachmentCount    = 3;
    prci.pColorAttachmentFormats = colorFormats;

    VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.pNext               = &prci;
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState      = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;
    gpci.pColorBlendState    = &cb;
    gpci.pDynamicState       = &ds;
    gpci.layout              = layout;
    gpci.renderPass          = VK_NULL_HANDLE;  // dynamic rendering

    VkPipeline     pipe = VK_NULL_HANDLE;
    const VkResult pr   = vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipe);
    check(pr == VK_SUCCESS && pipe != VK_NULL_HANDLE,
          "vkCreateGraphicsPipelines(passthrough vert + generated gbuffer frag, 3 MRT)");
    check(!g_validation_error, "no Vulkan validation errors");

    if (pipe)   vkDestroyPipeline(g_device, pipe, nullptr);
    if (layout) vkDestroyPipelineLayout(g_device, layout, nullptr);
    if (fs)     vkDestroyShaderModule(g_device, fs, nullptr);
    if (vs)     vkDestroyShaderModule(g_device, vs, nullptr);

    std::printf("gbuffer_pipeline_test: %d failures\n", fails);
    return fails == 0 ? 0 : 1;
}
