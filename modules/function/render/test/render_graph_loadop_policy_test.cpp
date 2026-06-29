// ============================================================================
//  render_graph_loadop_policy_test.cpp
//
//  Unit-style policy test for imported-resource first-write loadOp decisions.
// ============================================================================

#include <lux/engine/render/graph/RGLoadOpPolicy.hpp>

#include <vulkan/vulkan.h>

#include <iostream>

using namespace lux::render;

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char* name)
{
    if (cond)
    {
        std::cout << "  [PASS] " << name << "\n";
        ++g_pass;
    }
    else
    {
        std::cout << "  [FAIL] " << name << "\n";
        ++g_fail;
    }
}

int main()
{
    std::cout << "=== Render Graph LoadOp Policy Test ===\n";

    check(!shouldPreserveFirstWriteLoadOp(
              false, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
          "preserve=false -> first write must CLEAR");

    check(!shouldPreserveFirstWriteLoadOp(
              true, VK_IMAGE_LAYOUT_UNDEFINED),
          "preserve=true + initial=UNDEFINED -> first write must CLEAR");

    check(shouldPreserveFirstWriteLoadOp(
              true, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
          "preserve=true + initial=COLOR_ATTACHMENT_OPTIMAL -> first write LOAD");

    std::cout << "Summary: pass=" << g_pass << " fail=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}

