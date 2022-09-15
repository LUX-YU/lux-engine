#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <iostream>

// d3d12
#include <DirectXMath.h> // For XMVector, XMFLOAT3, XMFLOAT4
#include <comdef.h>      // for _com_error
#include <d3d12.h>       // for D3D12 interface
#include <dxgi1_6.h>     // for DXGI
#include <wrl.h>         // for Microsoft::WRL::ComPtr

void DxTrace(const wchar_t *file, unsigned long line, HRESULT hr,
             const wchar_t *proc);

#define V_RETURN(op)                                                           \
  if (FAILED(hr = (op))) {                                                     \
    assert(0);                                                                 \
    DxTrace(__FILEW__, __LINE__, hr, L#op);                                    \
    return hr;                                                                 \
  }

#define V(op)                                                                  \
  if (FAILED(hr = (op))) {                                                     \
    assert(0);                                                                 \
    DxTrace(__FILEW__, __LINE__, hr, L#op);                                    \
  }

#define SAFE_ADDREF(obj) ((obj) ? (obj)->AddRef() : 0)
#define SAFE_RELEASE(obj) ((obj) ? (obj)->Release() : 0)

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static HRESULT InitDirect3D(HWND hwnd, int width, int height);
static HRESULT
GetHardwareAdpter(_In_ IDXGIFactory1 *pFactory,
                  _In_ BOOL bRequestHighPerformanceAdapter,
                  _In_ BOOL (*AdapterSelectionCallback)(IDXGIAdapter1 *),
                  _Out_ IDXGIAdapter1 **ppAdapter);
static HRESULT CreateCommandObjects();
static HRESULT CreateSwapChain(HWND hwnd, int width, int height,
                               IDXGIFactory4 *pDXGIFactory);
static HRESULT ResizeRenderedBuffers(int width, int height);
static HRESULT CreateRtvAndDsvDescriptorHeaps(int extraRtvCount,
                                              int extraDsvCount);
static VOID FlushCommandQueue();
static VOID FreeD3DResources();

static VOID RenderFrame();

static ComPtr<ID3D12Device> g_pd3dDevice;
static INT64 g_iFencePoint;
static ComPtr<ID3D12Fence> g_pd3dFence;
static HANDLE g_hFenceEvent;

static UINT g_uRtvDescriptorSize;
static UINT g_uDsvDescriptorSize;
static UINT g_uCbvSrvUavDescriptorSize;

// Command queue
static ComPtr<ID3D12CommandQueue> g_pd3dCommandQueue;
static ComPtr<ID3D12GraphicsCommandList> g_pd3dCommandList;
static ComPtr<ID3D12CommandAllocator> g_pd3dDirectCmdAlloc;

// Swap chain
static ComPtr<IDXGISwapChain> g_pSwapChain;
static constexpr DXGI_FORMAT g_BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static constexpr DXGI_FORMAT g_DepthStencilFormat =
    DXGI_FORMAT_D24_UNORM_S8_UINT;
static constexpr UINT g_iSwapChainBufferCount = 2;

static ComPtr<ID3D12DescriptorHeap> g_pRTVDescriptorHeap;
static ComPtr<ID3D12DescriptorHeap> g_pDSVDescriptorHeap;

static ComPtr<ID3D12Resource> g_pSwapChainBuffers[g_iSwapChainBufferCount];
static ComPtr<ID3D12Resource> g_pDepthStencilBuffer;

static UINT g_iCurrentFrameIndex = 0;

static D3D12_VIEWPORT g_ScreenViewport;
static D3D12_RECT g_ScreenScissorRect;

// Model objects

// GLFW callbacks
static void OnResizeFrame(GLFWwindow *window, int width, int height);

int main() {

  const int width = 800, height = 600;

  glfwInit();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  auto window =
      glfwCreateWindow(width, height, "glfw - DX12", nullptr, nullptr);
  if (!window) {
    std::cout << "Creat window error" << std::endl;
    return -1;
  }
  glfwSetFramebufferSizeCallback(window, OnResizeFrame);

  auto process_keystrokes_input = [](GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
      glfwSetWindowShouldClose(window, 1);
  };

  auto hMainWnd = glfwGetWin32Window(window);

  InitDirect3D(hMainWnd, 800, 600);

  // Resize window for the first time
  OnResizeFrame(window, width, height);

  while (!glfwWindowShouldClose(window)) {

    process_keystrokes_input(window);

    RenderFrame();

    glfwPollEvents();
  }

  FreeD3DResources();
  glfwTerminate();

  return 0;
}

VOID RenderFrame() {

  HRESULT hr;

  // Reuse the memory associated with command recording.
  // We can only reset when the associated command lists have finished execution
  // on the GPU.
  V(g_pd3dDirectCmdAlloc->Reset());
  // A command list can be reset after it has been added to the command queue
  // via ExecuteCommandList.
  // Reusing the command list reuses memory.
  V(g_pd3dCommandList->Reset(g_pd3dDirectCmdAlloc.Get(), nullptr));

  // Prepare for the next frame
  D3D12_RESOURCE_BARRIER rdBarrier = {
      D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      D3D12_RESOURCE_BARRIER_FLAG_NONE,
  };
  rdBarrier.Transition.pResource =
      g_pSwapChainBuffers[g_iCurrentFrameIndex].Get();
  rdBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  rdBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  g_pd3dCommandList->ResourceBarrier(1, &rdBarrier);

  // Render the next frame

  g_pd3dCommandList->RSSetViewports(1, &g_ScreenViewport);
  g_pd3dCommandList->RSSetScissorRects(1, &g_ScreenScissorRect);

  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
      g_pRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
  rtvHandle.ptr += g_iCurrentFrameIndex * g_uRtvDescriptorSize;
  D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
      g_pDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart();

  const float clrColor[] = {1.0f, .0f, .0f, 1.0f};

  g_pd3dCommandList->ClearRenderTargetView(rtvHandle, clrColor, 0, nullptr);
  g_pd3dCommandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH,
                                           1.0f, 0, 0, nullptr);

  g_pd3dCommandList->OMSetRenderTargets(1, &rtvHandle, TRUE, &dsvHandle);

  // State transition
  rdBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  rdBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  g_pd3dCommandList->ResourceBarrier(1, &rdBarrier);

  // Done recording commands.
  V(g_pd3dCommandList->Close());
  ID3D12CommandList *cmdLists[] = {g_pd3dCommandList.Get()};
  g_pd3dCommandQueue->ExecuteCommandLists(1, cmdLists);

  //
  g_pSwapChain->Present(0, 0);
  g_iCurrentFrameIndex = (g_iCurrentFrameIndex + 1) % g_iSwapChainBufferCount;

  FlushCommandQueue();
}

void OnResizeFrame(GLFWwindow *window, int width, int height) {
  ResizeRenderedBuffers(width, height);
}

HRESULT InitDirect3D(HWND hwnd, int width, int height) {

  HRESULT hr;
  UINT dxgiFactoryFlags = 0;
#ifdef _DEBUG
  ComPtr<ID3D12Debug> debugController;

  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
    debugController->EnableDebugLayer();
    dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
  }
#endif

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;

  V_RETURN(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

  auto AdapterSelctionPred = [](IDXGIAdapter1 *pAdapter) {
    DXGI_ADAPTER_DESC1 desc;
    pAdapter->GetDesc1(&desc);

    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
      // Basic Render Driver Adapter
      return FALSE;
    } else {
      // Check whether the adapter support dx12
      if (FAILED(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS((ID3D12Device **)0)))) {
        return FALSE;
      }
      return TRUE;
    }
  };

  // select a adapter
  V_RETURN(
      GetHardwareAdpter(factory.Get(), TRUE, AdapterSelctionPred, &adapter));
  // create device
  V_RETURN(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&g_pd3dDevice)));

  g_iFencePoint = 0;
  V_RETURN(g_pd3dDevice->CreateFence(g_iFencePoint, D3D12_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&g_pd3dFence)));

  g_uRtvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  g_uDsvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  g_uCbvSrvUavDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  V_RETURN(CreateCommandObjects());
  V_RETURN(CreateSwapChain(hwnd, width, height, factory.Get()));
  V_RETURN(CreateRtvAndDsvDescriptorHeaps(0, 0));

  return hr;
}

HRESULT CreateCommandObjects() {
  HRESULT hr;
  D3D12_COMMAND_QUEUE_DESC dqd = {};
  dqd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  dqd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  V_RETURN(g_pd3dDevice->CreateCommandQueue(&dqd,
                                            IID_PPV_ARGS(&g_pd3dCommandQueue)));

  V_RETURN(g_pd3dDevice->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_pd3dDirectCmdAlloc)));

  V_RETURN(g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           g_pd3dDirectCmdAlloc.Get(), nullptr,
                                           IID_PPV_ARGS(&g_pd3dCommandList)));

  g_pd3dCommandList->Close();

  return hr;
}

HRESULT CreateSwapChain(HWND hwnd, int width, int height,
                        IDXGIFactory4 *pDXGIFactory) {

  HRESULT hr;
  DXGI_SWAP_CHAIN_DESC dscd;

  // Release the previous Swap Chain so we can re-create new ones.
  g_pSwapChain = nullptr;

  dscd.BufferDesc.Width = width;
  dscd.BufferDesc.Height = height;
  dscd.BufferDesc.RefreshRate.Numerator = 60;
  dscd.BufferDesc.RefreshRate.Denominator = 1;
  dscd.BufferDesc.Format = g_BackBufferFormat;
  dscd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
  dscd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  dscd.SampleDesc.Count = 1;
  dscd.SampleDesc.Quality = 0;
  dscd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  dscd.BufferCount = g_iSwapChainBufferCount;
  dscd.OutputWindow = hwnd;
  dscd.Windowed = TRUE;
  dscd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  dscd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
               DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  V_RETURN(pDXGIFactory->CreateSwapChain(g_pd3dCommandQueue.Get(), &dscd,
                                         &g_pSwapChain));

  return hr;
}

HRESULT CreateRtvAndDsvDescriptorHeaps(int extraRtvCount, int extraDsvCount) {
  HRESULT hr;

  D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
  rtvHeapDesc.NumDescriptors = g_iSwapChainBufferCount + extraRtvCount;
  rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  rtvHeapDesc.NodeMask = 0;
  V_RETURN(g_pd3dDevice->CreateDescriptorHeap(
      &rtvHeapDesc, IID_PPV_ARGS(&g_pRTVDescriptorHeap)));

  D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
  dsvHeapDesc.NumDescriptors = 1 + extraDsvCount;
  dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  dsvHeapDesc.NodeMask = 0;
  V_RETURN(g_pd3dDevice->CreateDescriptorHeap(
      &dsvHeapDesc, IID_PPV_ARGS(&g_pDSVDescriptorHeap)));

  return hr;
}

VOID FlushCommandQueue() {

  HRESULT hr;
  g_iFencePoint += 1;

  V(g_pd3dCommandQueue->Signal(g_pd3dFence.Get(), g_iFencePoint));

  if (g_pd3dFence->GetCompletedValue() != g_iFencePoint) {
    if (!g_hFenceEvent)
      g_hFenceEvent = CreateEventEx(NULL, NULL, 0, EVENT_ALL_ACCESS);

    g_pd3dFence->SetEventOnCompletion(g_iFencePoint, g_hFenceEvent);

    WaitForSingleObject(g_hFenceEvent, INFINITE);
  }
}

VOID FreeD3DResources() {
  if (g_hFenceEvent)
    CloseHandle(g_hFenceEvent);
}

HRESULT ResizeRenderedBuffers(int width, int height) {

  HRESULT hr;
  int i;
  DXGI_SWAP_CHAIN_DESC scDesc;

  assert(g_pd3dDevice && "Device must be available!");
  assert(g_pd3dDirectCmdAlloc && "Command allocator must be available!");
  assert(g_pSwapChain && "Swap chain must be available!");

  // Flush before changing any resources.
  FlushCommandQueue();

  V_RETURN(g_pd3dCommandList->Reset(g_pd3dDirectCmdAlloc.Get(), nullptr));

  // Release the previous resources we will be recreating.
  for (i = 0; i < g_iSwapChainBufferCount; ++i)
    g_pSwapChainBuffers[i] = nullptr;
  g_pDepthStencilBuffer = nullptr;

  g_pSwapChain->GetDesc(&scDesc);

  // Resize the swap chain.
  V_RETURN(g_pSwapChain->ResizeBuffers(g_iSwapChainBufferCount, width, height,
                                       g_BackBufferFormat, scDesc.Flags));

  g_iCurrentFrameIndex = 0;

  // RTVs
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle = {
      g_pRTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart()};
  for (i = 0; i < g_iSwapChainBufferCount; ++i) {
    g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_pSwapChainBuffers[i]));

    g_pd3dDevice->CreateRenderTargetView(g_pSwapChainBuffers[i].Get(), nullptr,
                                         rtvHeapHandle);

    rtvHeapHandle.ptr += g_uRtvDescriptorSize;
  }

  // Create depth-stencil buffer and the view.
  D3D12_RESOURCE_DESC dsd;
  dsd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  dsd.Alignment = 0;
  dsd.Width = width;
  dsd.Height = height;
  dsd.DepthOrArraySize = 1;
  dsd.MipLevels = 1;

  dsd.Format = DXGI_FORMAT_R24G8_TYPELESS;
  dsd.SampleDesc.Count = 1;
  dsd.SampleDesc.Quality = 0;
  dsd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  dsd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

  D3D12_CLEAR_VALUE optValue;
  optValue.Format = g_DepthStencilFormat;
  optValue.DepthStencil.Depth = 1.0f;
  optValue.DepthStencil.Stencil = 0;

  D3D12_HEAP_PROPERTIES heapProps = {D3D12_HEAP_TYPE_DEFAULT,
                                     D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 0, 0};

  V_RETURN(g_pd3dDevice->CreateCommittedResource(
      &heapProps, D3D12_HEAP_FLAG_NONE, &dsd, D3D12_RESOURCE_STATE_COMMON,
      &optValue, IID_PPV_ARGS(&g_pDepthStencilBuffer)));

  // Create descriptor to mip level 0 of entire resource using the format of the
  // resource.
  D3D12_DEPTH_STENCIL_VIEW_DESC ddsvd;
  ddsvd.Flags = D3D12_DSV_FLAG_NONE;
  ddsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  ddsvd.Format = g_DepthStencilFormat;
  ddsvd.Texture2D.MipSlice = 0;
  g_pd3dDevice->CreateDepthStencilView(
      g_pDepthStencilBuffer.Get(), &ddsvd,
      g_pDSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

  // Transition the resource from its initial state to be used as a depth
  // buffer.
  D3D12_RESOURCE_BARRIER dsvBarrier = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                       D3D12_RESOURCE_BARRIER_FLAG_NONE};
  dsvBarrier.Transition.pResource = g_pDepthStencilBuffer.Get();
  dsvBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  dsvBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  dsvBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  g_pd3dCommandList->ResourceBarrier(1, &dsvBarrier);

  // Execute the resize commands.
  V_RETURN(g_pd3dCommandList->Close());
  ID3D12CommandList *cmdLists[] = {g_pd3dCommandList.Get()};
  g_pd3dCommandQueue->ExecuteCommandLists(1, cmdLists);

  // Wait until resize is complete.
  FlushCommandQueue();

  // Update the viewport transform to cover the client area.
  g_ScreenViewport.TopLeftX = 0.f;
  g_ScreenViewport.TopLeftY = 0.f;
  g_ScreenViewport.Width = 1.f * width;
  g_ScreenViewport.Height = 1.f * height;
  g_ScreenViewport.MinDepth = 0.f;
  g_ScreenViewport.MaxDepth = 1.f;

  g_ScreenScissorRect = {0, 0, (LONG)width, (LONG)height};

  return hr;
}

HRESULT
GetHardwareAdpter(_In_ IDXGIFactory1 *pFactory,
                  _In_ BOOL bRequestHighPerformanceAdapter,
                  _In_ BOOL (*AdapterSelectionCallback)(IDXGIAdapter1 *),
                  _Out_ IDXGIAdapter1 **ppAdapter) {
  ComPtr<IDXGIFactory6> pFactory6;
  ComPtr<IDXGIAdapter1> adapter;
  HRESULT hr = E_FAIL;

  *ppAdapter = nullptr;

  if (SUCCEEDED(pFactory->QueryInterface(IID_PPV_ARGS(&pFactory6)))) {
    for (int adapterIndex = 0;
         DXGI_ERROR_NOT_FOUND != pFactory6->EnumAdapterByGpuPreference(
                                     adapterIndex,
                                     bRequestHighPerformanceAdapter
                                         ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                         : DXGI_GPU_PREFERENCE_UNSPECIFIED,
                                     IID_PPV_ARGS(&adapter));
         ++adapterIndex) {

      if (AdapterSelectionCallback(adapter.Get())) {
        *ppAdapter = adapter.Get();
        adapter->AddRef();
        hr = S_OK;
        break;
      }
    }
  } else {
    for (int adapterIndex = 0; DXGI_ERROR_NOT_FOUND !=
                               pFactory->EnumAdapters1(adapterIndex, &adapter);
         ++adapterIndex) {
      if (AdapterSelectionCallback(adapter.Get())) {
        *ppAdapter = adapter.Get();
        adapter->AddRef();
        hr = S_OK;
        break;
      }
    }
  }

  return hr;
}

void DxTrace(const wchar_t *file, unsigned long line, HRESULT hr,
             const wchar_t *proc) {
  _com_error err(hr);
  std::cerr << "file:" << file << "line:" << line << ", " << proc
            << " Error: " << (const char *)err.Description() << std::endl;
}