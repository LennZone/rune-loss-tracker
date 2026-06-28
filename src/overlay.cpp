#include "overlay.h"
#include "tracker.h"
#include "config.h"
#if __has_include("res_assets.h")
#include "res_assets.h"
#define HAS_RES_ASSETS 1
#endif
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

// ── Logging ───────────────────────────────────────────────────────────────────

static std::string g_logPath;

static void OvlLog(const char* msg) {
    if (!g_config.log || g_logPath.empty()) return;
    std::ofstream f(g_logPath, std::ios::app);
    f << "[OVL] " << msg << "\n";
}

// ── D3D12 resources ───────────────────────────────────────────────────────────

static ID3D12Device*              g_pDevice       = nullptr;
static ID3D12CommandQueue*        g_gameQueue     = nullptr; // captured from game
static ID3D12DescriptorHeap*      g_pRtvHeap      = nullptr;
static ID3D12DescriptorHeap*      g_pSrvHeap      = nullptr;
static ID3D12GraphicsCommandList* g_pCmdList      = nullptr;
static ID3D12Fence*               g_pFence        = nullptr;
static HANDLE                     g_fenceEvent    = nullptr;
static UINT64                     g_fenceValue    = 0;
static UINT                       g_bufferCount   = 0;

static const UINT MAX_BUFFERS = 8;
struct Frame {
    ID3D12CommandAllocator*     Alloc  = nullptr;
    ID3D12Resource*             RT     = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE Handle = {};
};
static Frame g_frames[MAX_BUFFERS];

static ImFont* g_pFontLabel  = nullptr; // small — for "Deaths" / "Lost Runes"
static ImFont* g_pFontNumber = nullptr; // large — matches game rune counter size
static float   g_sc          = 1.0f;

static bool g_overlayRunning   = false;
static bool g_imguiInitialized = false;
static bool g_needPsoCreate    = false; // CreateDeviceObjects deferred to next frame

static ID3D12Resource*             g_pSoulsTex        = nullptr;
static D3D12_GPU_DESCRIPTOR_HANDLE g_soulsGpu          = {};
static ID3D12Resource*             g_pDeathTex        = nullptr;
static D3D12_GPU_DESCRIPTOR_HANDLE g_deathGpu          = {};
static ID3D12Resource*             g_pSoulsUploadRes  = nullptr;
static ID3D12Resource*             g_pDeathUploadRes  = nullptr;
static bool                        g_texUploaded      = false;

// Dedicated COPY queue for texture uploads. The DMA/copy engine runs independently
// of the shader engine so uploads don't compete with the game's first-launch
// shader compilation that saturates the graphics queue and causes TDR.
static ID3D12CommandQueue*        g_pUploadQueue   = nullptr;
static ID3D12CommandAllocator*    g_pUploadAlloc   = nullptr;
static ID3D12GraphicsCommandList* g_pUploadList    = nullptr;
static ID3D12Fence*               g_pUploadFence   = nullptr;
static UINT64                     g_uploadFenceVal = 0;
static bool                       g_needTexBarrier = false; // issue COMMON→PSR barriers before first draw

// Settle probe: before first render we signal g_gameQueue and check how fast it
// responds. If the signal is processed within 2 frames the queue is not heavily
// loaded by first-launch shader compilation and our render is safe.
static ID3D12Fence* g_pSettleFence   = nullptr;
static UINT64       g_settleVal      = 0;
static bool         g_settleProbed   = false;
static int          g_settleFrames   = 0;
static bool         g_queueSettled   = false;

// ── Hook types ────────────────────────────────────────────────────────────────

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain3*, UINT, UINT);
static PresentFn g_origPresent = nullptr;

typedef void(WINAPI* ExecuteCommandListsFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static ExecuteCommandListsFn g_origExecuteCommandLists = nullptr;

// ── Helpers ───────────────────────────────────────────────────────────────────

static DXGI_FORMAT StripSRGB(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
        default: return f;
    }
}

// ── ExecuteCommandLists hook — captures game's command queue ──────────────────

static void WINAPI HookedExecuteCommandLists(
    ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    // Save first non-null queue we see — that's the game's render queue
    if (!g_gameQueue && queue)
        g_gameQueue = queue;

    g_origExecuteCommandLists(queue, count, lists);
}

// ── DDS texture uploader (BC1/BC2/BC3, mip 0 only) ───────────────────────────
//
// Records copy + barrier commands onto an ALREADY-RECORDING command list.
// Returns the upload staging buffer, which the caller must keep alive until
// the GPU signals completion (release it after the next per-frame fence wait).
// On failure, returns nullptr and leaves *ppTex unchanged.
//
// No GPU wait is performed here — the copy happens when the caller submits the
// command list, eliminating the WaitForSingleObject-in-Present TDR risk.
// forCopyQueue=true  → texture created in COMMON state, no PSR barrier recorded.
//                       The COPY engine implicitly promotes COMMON→COPY_DEST.
//                       After ExecuteCommandLists the state decays back to COMMON.
//                       The caller must issue COMMON→PIXEL_SHADER_RESOURCE on the
//                       graphics queue after a cross-queue fence sync.
// forCopyQueue=false → texture created in COPY_DEST, PSR barrier included inline
//                       (legacy path, used only when COPY queue creation fails).
static ID3D12Resource* EnqueueTextureUpload(
    ID3D12GraphicsCommandList* cmdList,
    const uint8_t* data, size_t size,
    ID3D12Resource** ppTex,
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu,
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu,
    bool forCopyQueue = false)
{
    if (size < 128 || memcmp(data, "DDS ", 4) != 0) return nullptr;

    UINT height = *(UINT*)(data + 12);
    UINT width  = *(UINT*)(data + 16);
    char fourCC[5] = {};
    memcpy(fourCC, data + 84, 4);

    DXGI_FORMAT fmt   = DXGI_FORMAT_UNKNOWN;
    UINT blockBytes   = 0;
    if      (strncmp(fourCC, "DXT5", 4) == 0) { fmt = DXGI_FORMAT_BC3_UNORM; blockBytes = 16; }
    else if (strncmp(fourCC, "DXT3", 4) == 0) { fmt = DXGI_FORMAT_BC2_UNORM; blockBytes = 16; }
    else if (strncmp(fourCC, "DXT1", 4) == 0) { fmt = DXGI_FORMAT_BC1_UNORM; blockBytes =  8; }
    else return nullptr;

    const uint8_t* pixels    = data + 128;
    UINT           srcRowPitch = ((width + 3) / 4) * blockBytes;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = fmt;
    texDesc.SampleDesc       = {1, 0};

    D3D12_HEAP_PROPERTIES defaultHeap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_STATES initState = forCopyQueue
        ? D3D12_RESOURCE_STATE_COMMON       // COPY engine promotes COMMON→COPY_DEST implicitly
        : D3D12_RESOURCE_STATE_COPY_DEST;   // legacy: already in COPY_DEST for DIRECT queue
    if (FAILED(g_pDevice->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            initState, nullptr, IID_PPV_ARGS(ppTex))))
        return nullptr;

    UINT64 uploadSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    g_pDevice->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadSize);

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc       = {1, 0};
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap = { D3D12_HEAP_TYPE_UPLOAD };
    ID3D12Resource* pUpload = nullptr;
    if (FAILED(g_pDevice->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&pUpload)))) {
        (*ppTex)->Release(); *ppTex = nullptr; return nullptr;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE readRange = {};
    pUpload->Map(0, &readRange, (void**)&mapped);
    UINT numBlockRows = (height + 3) / 4;
    for (UINT r = 0; r < numBlockRows; r++)
        memcpy(mapped + r * footprint.Footprint.RowPitch, pixels + r * srcRowPitch, srcRowPitch);
    pUpload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = *ppTex;
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource       = pUpload;
    srcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    // For the COPY queue path, issue an explicit COMMON→COPY_DEST barrier before
    // copying so we don't rely on implicit state promotion (unreliable on some
    // drivers), then COPY_DEST→COMMON after to guarantee the texture is in COMMON
    // for the cross-queue handoff to the graphics queue.
    if (forCopyQueue) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource   = *ppTex;
        rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        cmdList->ResourceBarrier(1, &rb);
    }

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource   = *ppTex;
        rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        rb.Transition.StateAfter  = forCopyQueue
            ? D3D12_RESOURCE_STATE_COMMON               // explicit handoff state
            : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; // legacy DIRECT path
        cmdList->ResourceBarrier(1, &rb);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = fmt;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;
    g_pDevice->CreateShaderResourceView(*ppTex, &srvDesc, srvCpu);

    return pUpload;
}

// ── ImGui init (called once on first Present after game queue is known) ───────

static std::string GetDllDir() {
    HMODULE hm = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)GetDllDir, &hm);
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(hm, buf, MAX_PATH);
    char* slash = strrchr(buf, '\\');
    if (slash) *(slash + 1) = '\0';
    return std::string(buf);
}

static void LoadFonts(float sc) {
#ifdef HAS_RES_ASSETS
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false; // data is a static array in the binary
    g_pFontLabel  = io.Fonts->AddFontFromMemoryTTF((void*)res_font_ttf, (int)sizeof(res_font_ttf), 13.0f * sc, &cfg);
    cfg.MergeMode = false;
    g_pFontNumber = io.Fonts->AddFontFromMemoryTTF((void*)res_font_ttf, (int)sizeof(res_font_ttf), 27.0f * sc, &cfg);

    if (!g_pFontLabel || !g_pFontNumber) {
        OvlLog("Font load failed — using default");
        g_pFontLabel = g_pFontNumber = nullptr;
    } else {
        OvlLog("Font loaded OK");
    }
#else
    OvlLog("res_assets.h not present — using default ImGui font");
#endif
}

static bool InitImGui(IDXGISwapChain3* pSC) {
    OvlLog("InitImGui begin");

    if (!g_gameQueue) { OvlLog("Game queue not yet captured"); return false; }

    if (FAILED(pSC->GetDevice(__uuidof(ID3D12Device), (void**)&g_pDevice))) {
        OvlLog("GetDevice failed"); return false;
    }

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    pSC->GetDesc(&scDesc);
    g_bufferCount = (scDesc.BufferCount < MAX_BUFFERS) ? scDesc.BufferCount : MAX_BUFFERS;
    DXGI_FORMAT fmt = StripSRGB(scDesc.BufferDesc.Format);
    g_sc = (float)scDesc.BufferDesc.Height / 1080.0f;

    {
        char buf[64];
        snprintf(buf, sizeof(buf), "bufCount=%u fmt=%u sc=%.2f", g_bufferCount, (unsigned)fmt, g_sc);
        OvlLog(buf);
    }

    // RTV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        d.NumDescriptors = g_bufferCount;
        if (FAILED(g_pDevice->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_pRtvHeap)))) {
            OvlLog("RtvHeap failed"); return false;
        }
    }

    // SRV heap — slot 0: ImGui fonts, slot 1: lost-runes texture, slot 2: death-counter texture
    {
        D3D12_DESCRIPTOR_HEAP_DESC d = {};
        d.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 3;
        d.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_pDevice->CreateDescriptorHeap(&d, IID_PPV_ARGS(&g_pSrvHeap)))) {
            OvlLog("SrvHeap failed"); return false;
        }
    }

    // Per-frame allocators + RTVs
    UINT rtvStep = g_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_pRtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < g_bufferCount; ++i) {
        g_frames[i].Handle = rtvH;
        rtvH.ptr += rtvStep;

        if (FAILED(g_pDevice->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].Alloc)))) {
            OvlLog("CreateCommandAllocator failed"); return false;
        }

        if (FAILED(pSC->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].RT)))) {
            OvlLog("GetBuffer failed"); return false;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvd = {};
        rtvd.Format        = fmt;
        rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_pDevice->CreateRenderTargetView(g_frames[i].RT, &rtvd, g_frames[i].Handle);
    }

    // Command list (uses game queue's type: DIRECT)
    if (FAILED(g_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            g_frames[0].Alloc, nullptr, IID_PPV_ARGS(&g_pCmdList)))) {
        OvlLog("CreateCommandList failed"); return false;
    }
    g_pCmdList->Close();

    // Fence for GPU sync
    if (FAILED(g_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pFence)))) {
        OvlLog("CreateFence failed"); return false;
    }
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Settle probe fence — used to gauge how loaded g_gameQueue is before first render.
    if (FAILED(g_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pSettleFence)))) {
        OvlLog("SettleFence creation failed — will render without settle check");
        g_pSettleFence = nullptr;
        g_queueSettled = true; // skip settle check
    }

    // Dedicated COPY queue + command list for texture uploads.
    // The GPU's DMA engine processes COPY queues independently from the graphics
    // engine, so texture copies won't compete with first-launch shader compilation.
    {
        D3D12_COMMAND_QUEUE_DESC cqd = {};
        cqd.Type  = D3D12_COMMAND_LIST_TYPE_COPY;
        cqd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(g_pDevice->CreateCommandQueue(&cqd, IID_PPV_ARGS(&g_pUploadQueue)))) {
            OvlLog("UploadQueue creation failed — geometric fallback");
            g_pUploadQueue = nullptr;
        }
    }
    if (g_pUploadQueue) {
        bool uploadOk =
            SUCCEEDED(g_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_pUploadAlloc))) &&
            SUCCEEDED(g_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, g_pUploadAlloc, nullptr, IID_PPV_ARGS(&g_pUploadList))) &&
            SUCCEEDED(g_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_pUploadFence)));
        if (!uploadOk) {
            OvlLog("UploadQueue setup failed — geometric fallback");
            if (g_pUploadList)  { g_pUploadList->Release();  g_pUploadList  = nullptr; }
            if (g_pUploadAlloc) { g_pUploadAlloc->Release(); g_pUploadAlloc = nullptr; }
            if (g_pUploadFence) { g_pUploadFence->Release(); g_pUploadFence = nullptr; }
            g_pUploadQueue->Release(); g_pUploadQueue = nullptr;
        } else {
            g_pUploadList->Close();
            OvlLog("Upload copy queue ready");
        }
    }

    // Find game window
    HWND hwnd = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
            *(HWND*)lp = h;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&hwnd);

    if (!hwnd) { OvlLog("No window found"); return false; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    LoadFonts(g_sc);

    OvlLog("ImGui_ImplWin32_Init");
    ImGui_ImplWin32_Init(hwnd);

    OvlLog("ImGui_ImplDX12_Init");
    ImGui_ImplDX12_InitInfo dx12Info = {};
    dx12Info.Device            = g_pDevice;
    dx12Info.CommandQueue      = g_gameQueue;
    dx12Info.NumFramesInFlight = g_bufferCount;
    dx12Info.RTVFormat         = fmt;
    dx12Info.DSVFormat         = DXGI_FORMAT_UNKNOWN;
    dx12Info.SrvDescriptorHeap = g_pSrvHeap;
    dx12Info.LegacySingleSrvCpuDescriptor = g_pSrvHeap->GetCPUDescriptorHandleForHeapStart();
    dx12Info.LegacySingleSrvGpuDescriptor = g_pSrvHeap->GetGPUDescriptorHandleForHeapStart();
    ImGui_ImplDX12_Init(&dx12Info);
    OvlLog("ImGui_ImplDX12_Init done");

    g_needPsoCreate = true;
    OvlLog("InitImGui OK");
    return true;
}

// ── Wheel icon (approximation of vanilla rune medallion) ──────────────────────

static void DrawWheelIcon(ImDrawList* dl, ImVec2 c, float R, ImU32 gold, ImU32 dark) {
    const float PI2 = 6.28318530f;
    dl->AddCircleFilled(c, R, dark);

    // 8 petals: filled circles arranged in a ring
    float petalR    = R * 0.27f;
    float petalDist = R * 0.57f;
    for (int k = 0; k < 8; k++) {
        float a = k * (PI2 / 8.0f);
        ImVec2 pc = { c.x + cosf(a) * petalDist, c.y + sinf(a) * petalDist };
        dl->AddCircleFilled(pc, petalR, gold);
    }

    // Dark center cutout so petals form a ring, not a filled disc
    dl->AddCircleFilled(c, R * 0.33f, dark);

    // Small gold hub + dark center dot
    dl->AddCircleFilled(c, R * 0.19f, gold);
    dl->AddCircleFilled(c, R * 0.09f, dark);

    // Outer gold ring
    dl->AddCircle(c, R, gold, 40, g_sc * 1.3f);
}

// ── Draw panels ───────────────────────────────────────────────────────────────

static void DrawPanels() {
    if (!g_inGame) return;

    ImGuiIO& io = ImGui::GetIO();
    float sc = g_sc;

    // Panel dimensions — measured from vanilla rune counter at 1440p (269×50px)
    float W        = 202.f * sc;   // → 269px @ 1440p
    float H        =  38.f * sc;   // → 51px  @ 1440p  (vanilla: 50px)
    float GAP      =   3.f * sc;
    float MARGIN_X =  48.f * sc;   // right margin
    float MARGIN_Y =   9.25f * sc; // bottom margin  (1px down @ 1440p = -0.75 base)
    float RUNE_H   =  58.f * sc;   // vanilla offset from bottom (38 base + 20 → 26px up @ 1440p)

    float numberSz = 26.0f * sc;
    ImFont* fNumber = g_pFontNumber ? g_pFontNumber : ImGui::GetFont();

    // 16:9 letterbox anchor (ultrawide support)
    float gameW = io.DisplaySize.y * (16.0f / 9.0f);
    if (gameW > io.DisplaySize.x) gameW = io.DisplaySize.x;
    float gameRight = (io.DisplaySize.x + gameW) * 0.5f;

    float px = gameRight - W - MARGIN_X;
    float py = io.DisplaySize.y - MARGIN_Y - RUNE_H - H - GAP - H;

    const ImU32 TXT = IM_COL32(255, 255, 255, 255);
    // Geometric fallback colors
    const ImU32 BG  = IM_COL32( 12,  10,   8, 225);
    const ImU32 BOR = IM_COL32(178, 138,  58, 210);
    const ImU32 GLD = IM_COL32(178, 138,  58, 220);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    int64_t vals[2]          = { (int64_t)g_deathCount, g_totalLost };
    ID3D12Resource* texRes[2] = { g_pDeathTex,          g_pSoulsTex  };
    D3D12_GPU_DESCRIPTOR_HANDLE texGpu[2] = { g_deathGpu, g_soulsGpu };

    for (int i = 0; i < 2; ++i) {
        if (i == 0 && !g_config.showDeathOverlay) continue;
        float x = px, y = py + i * (H + GAP);
        float cy = y + H * 0.5f;

        if (texRes[i]) {
            // ImGui 1.92+: pass GPU descriptor handle as ImTextureID via ImTextureRef.
            ImTextureRef texRef((ImTextureID)texGpu[i].ptr);
            dl->AddImage(texRef, ImVec2(x, y), ImVec2(x + W, y + H));
        } else {
            // Geometric fallback (texture not loaded)
            float rounding = 3.f * sc;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x+W, y+H), BG,  rounding);
            dl->AddRect      (ImVec2(x, y), ImVec2(x+W, y+H), BOR, rounding, 0, 1.0f);
            float iconR = H * 0.37f;
            DrawWheelIcon(dl, ImVec2(x + H * 0.5f, cy), iconR, GLD, BG);
        }

        // Number right-aligned (matches vanilla rune counter layout)
        char buf[32]; snprintf(buf, sizeof(buf), "%lld", vals[i]);
        ImVec2 ts = fNumber->CalcTextSizeA(numberSz, FLT_MAX, 0.0f, buf);
        dl->AddText(fNumber, numberSz,
            ImVec2(x + W - ts.x - 15.25f * sc, cy - ts.y * 0.5f), TXT, buf);
    }
}

// ── Hooked Present ────────────────────────────────────────────────────────────

static HRESULT WINAPI HookedPresent(IDXGISwapChain3* pSC, UINT Sync, UINT Flags) {
    if (!g_overlayRunning)
        return g_origPresent(pSC, Sync, Flags);

    if (!g_imguiInitialized) {
        static UINT  inGameFrames  = 0;
        static DWORD inGameSinceMs = 0;

        if (!g_inGame) {
            inGameFrames  = 0;
            inGameSinceMs = 0;
            return g_origPresent(pSC, Sync, Flags);
        }

        // Record when g_inGame first became true this session.
        if (inGameSinceMs == 0) inGameSinceMs = GetTickCount();

        // Wait at least 10s wall-clock before touching D3D12 resources.
        // g_inGame turns true while the loading screen is still running (death
        // counter becomes readable before GPU asset streaming finishes).
        // 10s gives enough time for first-launch shader/asset streaming to settle.
        DWORD msInGame = GetTickCount() - inGameSinceMs;
        if (msInGame < 10000) return g_origPresent(pSC, Sync, Flags);

        // Additional 60-frame buffer after the clock wait clears.
        if (++inGameFrames < 60) return g_origPresent(pSC, Sync, Flags);

        {
            char buf[48];
            snprintf(buf, sizeof(buf), "InitImGui attempt (ms=%lu f=%u)", (unsigned long)msInGame, inGameFrames);
            OvlLog(buf);
        }
        g_imguiInitialized = InitImGui(pSC);
        OvlLog(g_imguiInitialized ? "InitImGui success" : "InitImGui FAILED");
        return g_origPresent(pSC, Sync, Flags);
    }

    {
        HRESULT dr = g_pDevice->GetDeviceRemovedReason();
        if (FAILED(dr)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Device removed (0x%08X) — overlay disabled", (unsigned)dr);
            OvlLog(buf);
            g_overlayRunning = false;
            return g_origPresent(pSC, Sync, Flags);
        }
    }

    UINT idx = pSC->GetCurrentBackBufferIndex();
    Frame& fc = g_frames[idx < g_bufferCount ? idx : 0];

    if (g_pFence->GetCompletedValue() < g_fenceValue) {
        OvlLog("Fence wait begin");
        g_pFence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
        OvlLog("Fence wait end");
    }
    if (g_pSoulsUploadRes) { g_pSoulsUploadRes->Release(); g_pSoulsUploadRes = nullptr; }
    if (g_pDeathUploadRes) { g_pDeathUploadRes->Release(); g_pDeathUploadRes = nullptr; }

    if (g_needPsoCreate) {
        g_needPsoCreate = false;
        OvlLog("CreateDeviceObjects begin");
        if (!ImGui_ImplDX12_CreateDeviceObjects()) {
            OvlLog("CreateDeviceObjects FAILED — overlay disabled");
            g_overlayRunning = false;
            return g_origPresent(pSC, Sync, Flags);
        }
        OvlLog("CreateDeviceObjects OK");
        // Return early — texture uploads happen next frame.
        // Separating PSO/font-upload (this frame) from panel-texture-upload+first-render
        // (next frame) avoids submitting too much GPU work in one frame during loading.
        return g_origPresent(pSC, Sync, Flags);
    }

    // ── Texture upload frame ──────────────────────────────────────────────────
    // Submit texture copies on the dedicated COPY queue so the DMA engine handles
    // them independently from the graphics engine (which may be saturated with
    // first-launch shader compilation on g_gameQueue). The render command list is
    // NOT touched here; rendering begins on the following frame.
    if (!g_texUploaded && g_inGame) {
        g_texUploaded = true;
        UINT srvStep = g_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = g_pSrvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = g_pSrvHeap->GetGPUDescriptorHandleForHeapStart();

        if (g_pUploadQueue && g_pUploadList && g_pUploadFence) {
            // COPY queue path: textures created in COMMON, uploaded via DMA engine.
            g_pUploadAlloc->Reset();
            g_pUploadList->Reset(g_pUploadAlloc, nullptr);
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = { cpuBase.ptr + srvStep };
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = { gpuBase.ptr + srvStep };
#ifdef HAS_RES_ASSETS
                g_pSoulsUploadRes = EnqueueTextureUpload(g_pUploadList, res_lost_runes_dds, sizeof(res_lost_runes_dds), &g_pSoulsTex, cpu, gpu, true);
                if (g_pSoulsTex) { g_soulsGpu = gpu; OvlLog("Lost-runes enqueued on copy queue"); }
                else OvlLog("Lost-runes enqueue failed");
#else
                OvlLog("res_assets.h not present — skipping lost-runes texture, using fallback icon");
#endif
            }
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = { cpuBase.ptr + 2 * srvStep };
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = { gpuBase.ptr + 2 * srvStep };
#ifdef HAS_RES_ASSETS
                g_pDeathUploadRes = EnqueueTextureUpload(g_pUploadList, res_death_counter_dds, sizeof(res_death_counter_dds), &g_pDeathTex, cpu, gpu, true);
                if (g_pDeathTex) { g_deathGpu = gpu; OvlLog("Death-counter enqueued on copy queue"); }
                else OvlLog("Death-counter enqueue failed");
#else
                OvlLog("res_assets.h not present — skipping death-counter texture, using fallback icon");
#endif
            }
            g_pUploadList->Close();
            g_pUploadQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pUploadList);
            g_pUploadQueue->Signal(g_pUploadFence, ++g_uploadFenceVal);
            g_needTexBarrier = true;
            OvlLog("Texture upload submitted on copy queue");
        } else {
            // Fallback: no copy queue — geometric fallback only, no textures.
            OvlLog("No copy queue — geometric fallback");
        }
        return g_origPresent(pSC, Sync, Flags);
    }

    // ── Settle check ──────────────────────────────────────────────────────────
    // Before first render on g_gameQueue, probe how loaded the queue is.
    // We submit a Signal and check how many frames later it is processed.
    // If done within 2 frames the queue is not saturated by shader compilation;
    // rendering at that point is safe.  On first launch, shader compilation
    // can keep the queue loaded for minutes — this check adapts automatically.
    if (!g_queueSettled && g_pSettleFence) {
        if (!g_settleProbed) {
            g_gameQueue->Signal(g_pSettleFence, ++g_settleVal);
            g_settleProbed = true;
            g_settleFrames = 0;
            return g_origPresent(pSC, Sync, Flags);
        }
        g_settleFrames++;
        bool done = g_pSettleFence->GetCompletedValue() >= g_settleVal;
        if (done) {
            g_queueSettled = true;
            char buf[64];
            snprintf(buf, sizeof(buf), "g_gameQueue settled in %d frame(s)", g_settleFrames);
            OvlLog(buf);
            // Fall through to render this frame.
        } else if (g_settleFrames >= 3) {
            // Probe expired — re-probe next frame.
            g_settleProbed = false;
            return g_origPresent(pSC, Sync, Flags);
        } else {
            return g_origPresent(pSC, Sync, Flags);
        }
    }

    // ── Render frame ──────────────────────────────────────────────────────────
    if (FAILED(fc.Alloc->Reset()) || FAILED(g_pCmdList->Reset(fc.Alloc, nullptr))) {
        OvlLog("CmdList reset failed — device likely removed, overlay disabled");
        g_overlayRunning = false;
        return g_origPresent(pSC, Sync, Flags);
    }

    // First render after texture upload: verify copy is done and issue
    // COMMON→PIXEL_SHADER_RESOURCE barriers.
    //
    // We use a CPU-side fence check (GetCompletedValue) rather than
    // g_gameQueue->Wait for the cross-queue sync.  The DMA copy of a few KB
    // finishes in microseconds; by the time the next Present fires (~16 ms
    // later) it is guaranteed done.  The COMMON→PSR barrier then handles
    // cache coherency within the graphics engine — no GPU-side Wait needed.
    // (GPU-side Wait between COPY and DIRECT queues has driver-specific
    // issues on some hardware and can itself cause device removal.)
    if (g_needTexBarrier) {
        bool uploadDone = !g_pUploadFence ||
                          g_pUploadFence->GetCompletedValue() >= g_uploadFenceVal;
        if (!uploadDone) {
            // DMA not signalled yet — skip textures this frame, retry next.
            OvlLog("Upload fence not ready — retrying next frame");
            // Leave g_needTexBarrier = true so we re-check next Present.
        } else {
            g_needTexBarrier = false;
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "Upload fence done (val=%llu)", (unsigned long long)g_uploadFenceVal);
                OvlLog(buf);
            }
            // Transition textures from COMMON (post-decay from COPY queue) to PSR.
            D3D12_RESOURCE_BARRIER psr[2] = {};
            UINT nbPsr = 0;
            if (g_pSoulsTex) {
                auto& b = psr[nbPsr++];
                b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = g_pSoulsTex;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            if (g_pDeathTex) {
                auto& b = psr[nbPsr++];
                b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = g_pDeathTex;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            if (nbPsr > 0) {
                g_pCmdList->ResourceBarrier(nbPsr, psr);
                OvlLog("COMMON->PSR barriers recorded");
            }
        }
    }

    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource   = fc.RT;
    rb.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_pCmdList->ResourceBarrier(1, &rb);
    OvlLog("RT PRESENT->RT ok");
    g_pCmdList->OMSetRenderTargets(1, &fc.Handle, FALSE, nullptr);
    g_pCmdList->SetDescriptorHeaps(1, &g_pSrvHeap);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // While g_needTexBarrier is still pending the textures are in COMMON state
    // and must not be sampled — temporarily hide them so DrawPanels falls back
    // to the geometric style for this frame.
    ID3D12Resource* savedSouls = nullptr;
    ID3D12Resource* savedDeath = nullptr;
    if (g_needTexBarrier) {
        savedSouls = g_pSoulsTex; g_pSoulsTex = nullptr;
        savedDeath = g_pDeathTex; g_pDeathTex = nullptr;
    }
    DrawPanels();
    if (g_needTexBarrier) { g_pSoulsTex = savedSouls; g_pDeathTex = savedDeath; }
    ImGui::Render();
    OvlLog("ImGui render done");
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pCmdList);
    OvlLog("ImGui RenderDrawData done");

    rb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g_pCmdList->ResourceBarrier(1, &rb);
    OvlLog("RT RT->PRESENT ok");

    HRESULT hrClose = g_pCmdList->Close();
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "CmdList Close hr=0x%08X", (unsigned)hrClose);
        OvlLog(buf);
    }
    if (FAILED(hrClose)) {
        g_overlayRunning = false;
        return g_origPresent(pSC, Sync, Flags);
    }

    g_gameQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pCmdList);
    OvlLog("ExecuteCmdLists ok");
    g_gameQueue->Signal(g_pFence, ++g_fenceValue);
    OvlLog("Signal ok");

    return g_origPresent(pSC, Sync, Flags);
}

// ── StartOverlay / StopOverlay ────────────────────────────────────────────────

void StartOverlay() {
    g_logPath = GetDllDir() + "RuneLossTracker.log";
    OvlLog("StartOverlay");
    MH_Initialize();

    // Create dummy D3D12 device + swap chain to read Present vtable,
    // and a dummy command queue to read ExecuteCommandLists vtable.
    WNDCLASSEXA wc  = {};
    wc.cbSize       = sizeof(wc);
    wc.style        = CS_CLASSDC;
    wc.lpfnWndProc  = DefWindowProcA;
    wc.hInstance    = GetModuleHandleA(nullptr);
    wc.lpszClassName= "RLT_TMP";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, "RLT_TMP", "", WS_OVERLAPPEDWINDOW,
                                0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);

    ID3D12Device*       pDev = nullptr;
    ID3D12CommandQueue* pQ   = nullptr;
    IDXGIFactory4*      pFac = nullptr;
    IDXGISwapChain*     pSC  = nullptr;

    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDev));
    if (pDev) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        pDev->CreateCommandQueue(&qd, IID_PPV_ARGS(&pQ));
    }
    if (pQ) CreateDXGIFactory1(IID_PPV_ARGS(&pFac));
    if (pFac) {
        DXGI_SWAP_CHAIN_DESC scd = {};
        scd.BufferCount       = 2;
        scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow      = hwnd;
        scd.SampleDesc.Count  = 1;
        scd.Windowed          = TRUE;
        scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        pFac->CreateSwapChain(pQ, &scd, &pSC);
    }

    bool hooksOk = false;
    if (pSC && pQ) {
        void** scVT = *(void***)pSC;
        void** qVT  = *(void***)pQ;

        // vtable[8]  = IDXGISwapChain::Present
        // vtable[10] = ID3D12CommandQueue::ExecuteCommandLists
        MH_STATUS s1 = MH_CreateHook(scVT[8],  (void*)HookedPresent,             (void**)&g_origPresent);
        MH_STATUS s2 = MH_CreateHook(qVT[10],  (void*)HookedExecuteCommandLists, (void**)&g_origExecuteCommandLists);

        char buf[128];
        snprintf(buf, sizeof(buf), "MH_CreateHook Present=%d ECL=%d", (int)s1, (int)s2);
        OvlLog(buf);

        if ((s1 == MH_OK || s1 == MH_ERROR_ALREADY_CREATED) &&
            (s2 == MH_OK || s2 == MH_ERROR_ALREADY_CREATED)) {
            MH_EnableHook(MH_ALL_HOOKS);
            g_overlayRunning = true;
            OvlLog("Hooks enabled, overlay running");
            hooksOk = true;
        }
    }

    if (!hooksOk) OvlLog("Hook setup failed");

    if (pSC)  pSC->Release();
    if (pFac) pFac->Release();
    if (pQ)   pQ->Release();
    if (pDev) pDev->Release();

    DestroyWindow(hwnd);
    UnregisterClassA("RLT_TMP", wc.hInstance);
}

void StopOverlay() {
    g_overlayRunning = false;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    if (g_imguiInitialized) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiInitialized = false;
    }
    if (g_pSoulsUploadRes) { g_pSoulsUploadRes->Release(); g_pSoulsUploadRes = nullptr; }
    if (g_pDeathUploadRes) { g_pDeathUploadRes->Release(); g_pDeathUploadRes = nullptr; }
    if (g_pSoulsTex) { g_pSoulsTex->Release(); g_pSoulsTex = nullptr; g_soulsGpu = {}; }
    if (g_pDeathTex) { g_pDeathTex->Release(); g_pDeathTex = nullptr; g_deathGpu = {}; }
    if (g_pUploadList)  { g_pUploadList->Release();  g_pUploadList  = nullptr; }
    if (g_pUploadAlloc) { g_pUploadAlloc->Release(); g_pUploadAlloc = nullptr; }
    if (g_pUploadFence) { g_pUploadFence->Release(); g_pUploadFence = nullptr; }
    if (g_pUploadQueue) { g_pUploadQueue->Release(); g_pUploadQueue = nullptr; }
    if (g_pSettleFence) { g_pSettleFence->Release(); g_pSettleFence = nullptr; }
    for (auto& f : g_frames) {
        if (f.Alloc) { f.Alloc->Release(); f.Alloc = nullptr; }
        if (f.RT)    { f.RT->Release();    f.RT    = nullptr; }
    }
    if (g_pCmdList) { g_pCmdList->Release(); g_pCmdList = nullptr; }
    if (g_pFence)   { g_pFence->Release();   g_pFence   = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_pRtvHeap) { g_pRtvHeap->Release(); g_pRtvHeap = nullptr; }
    if (g_pSrvHeap) { g_pSrvHeap->Release(); g_pSrvHeap = nullptr; }
    if (g_pDevice)  { g_pDevice->Release();  g_pDevice  = nullptr; }
}
