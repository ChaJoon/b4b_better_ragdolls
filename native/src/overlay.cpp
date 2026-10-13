// `~` power-user overlay (#26): a translucent Dear ImGui window drawn over the game, with a tab per module (panel
// registry, overlay.h). It replaces the chat commands: every chat command has a control here.
//
// B4B renders with D3D12 (log: "DefaultRHIFromHardware: DX12"; vkd3d-proton under Proton). The render hooks are
// installed the first time the window is opened (nothing is touched before that): a throw-away D3D12 device, queue
// and swap chain on a hidden window give the vtables of IDXGISwapChain::Present / ResizeBuffers and
// ID3D12CommandQueue::ExecuteCommandLists, which are hooked with MinHook (every swap chain / queue of that
// implementation shares them). ExecuteCommandLists records the game's direct queue (a swap chain presents from the
// queue it was created with; UE has one direct queue).
//
// Threads: the ImGui frame is BUILT on the game thread (overlay_tick -> build_frame), so panels read UE state and call
// module code like a chat command handler does. ImGui::Render()'s draw lists are cloned into a snapshot; Present (UE's
// RHI thread) records the snapshot onto the current back buffer and submits it on the game's queue. `cs` guards ImGui
// state, but is NOT held while a panel's draw function runs (only inside each ov_* call): a panel action may make the
// game thread wait for the render threads (map travel, blocking loads), which must never wait for us.
//
// Input while open: the game window's procedure is subclassed; mouse (raw input deltas move a software cursor: the
// game hides and clips the real one), mouse buttons, wheel, key downs and characters go to ImGui and not to the game
// (key and button releases still reach it, so nothing stays held); hotkeys (cmds_hotkey_down) are off. `~` or Esc
// closes it.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_internal.h"   // ImGui::RegisterUserTexture (ov_texture)
extern "C" {
#include "log.h"
#include "cmds.h"
#include "MinHook.h"
}
#include "overlay.h"

// ImGui's DX12 backend compiles its two tiny shaders with D3DCompile: resolve it at run time instead of importing
// d3dcompiler_47.dll (the agent must load even where it is missing; Wine and Windows 10+ ship it).
extern "C" HRESULT WINAPI D3DCompile(LPCVOID src, SIZE_T len, LPCSTR name, const D3D_SHADER_MACRO *defs, ID3DInclude *inc,
                                     LPCSTR entry, LPCSTR target, UINT f1, UINT f2, ID3DBlob **code, ID3DBlob **errs) {
    typedef HRESULT(WINAPI * Fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT,
                                 UINT, ID3DBlob **, ID3DBlob **);
    static Fn fn;
    if (!fn) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        fn = m ? (Fn)(void *)GetProcAddress(m, "D3DCompile") : nullptr;
    }
    return fn ? fn(src, len, name, defs, inc, entry, target, f1, f2, code, errs) : E_NOTIMPL;
}

// ... and the backend makes a DXGI factory (tearing support query): same, from dxgi.dll at run time
extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void **out) {
    typedef HRESULT(WINAPI * Fn)(REFIID, void **);
    static Fn fn;
    if (!fn) {
        HMODULE m = LoadLibraryA("dxgi.dll");
        fn = m ? (Fn)(void *)GetProcAddress(m, "CreateDXGIFactory1") : nullptr;
    }
    return fn ? fn(riid, out) : E_NOTIMPL;
}

static int enabled = 1;          // ini overlay=0 turns it off
static int key = VK_OEM_3;       // ini overlay_key (default ~, the key left of 1 on a US layout)
static float ui_scale = 1.25f;   // ini overlay_scale
// Sizes and spacing (#44: ImGui's defaults are made for unscaled text and looked crowded): roomier than the defaults
// and scaled with the text size, so rows and sections keep their air at any overlay_scale. One place for every panel.
static float style_scale;
#define LOG_SPACING (2 * ui_scale)   // between the lines of the log under the tabs (rows of controls get ItemSpacing)
static void apply_style(float s) {
    ImGuiStyle &st = ImGui::GetStyle();
    st.FontScaleMain = s;
    st.WindowPadding = ImVec2(10 * s, 10 * s);
    st.FramePadding = ImVec2(6 * s, 4 * s);            // ImGui default 4, 3 (unscaled)
    st.ItemSpacing = ImVec2(8 * s, 6 * s);             // 8, 4: the gap between rows
    st.ItemInnerSpacing = ImVec2(6 * s, 4 * s);        // 4, 4
    st.CellPadding = ImVec2(6 * s, 4 * s);             // 4, 2: table rows
    st.SeparatorTextPadding = ImVec2(20 * s, 6 * s);   // 20, 3: section headings
    st.IndentSpacing = 21 * s;
    st.ScrollbarSize = 14 * s;
    st.GrabMinSize = 12 * s;
    st.WindowRounding = 6 * s;
    st.FrameRounding = 3 * s;
    st.GrabRounding = 3 * s;
    style_scale = s;
}
static volatile LONG open_;      // the window is shown
static int hooks;                // 0 not yet, 1 installed, -1 failed
static CRITICAL_SECTION cs;
static int cs_ready;
static void lock(int on) { if (cs_ready) { if (on) EnterCriticalSection(&cs); else LeaveCriticalSection(&cs); } }
struct Guard { Guard() { lock(1); } ~Guard() { lock(0); } };
#define LOCKED Guard guard_

// ---- D3D12 state (RHI thread) ----
typedef HRESULT(STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain3 *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *ResizeFn)(IDXGISwapChain3 *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE *EclFn)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
static PresentFn orig_present;
static ResizeFn orig_resize;
static EclFn orig_ecl;
static ID3D12CommandQueue *queue;   // the game's direct queue
static ID3D12Device *dev;
static IDXGISwapChain3 *chain;      // the swap chain we initialized for
static HWND hwnd;
static WNDPROC orig_wndproc;
#define MAXBUF 8
static UINT nbuf;
static ID3D12DescriptorHeap *rtv_heap, *srv_heap;
static UINT rtv_inc, srv_inc, srv_used;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv[MAXBUF];
static ID3D12Resource *backbuf[MAXBUF];
static ID3D12CommandAllocator *alloc[MAXBUF];
static UINT64 alloc_fence[MAXBUF];
static ID3D12GraphicsCommandList *cmdlist;
static ID3D12Fence *fence;
static UINT64 fence_val;
static HANDLE fence_ev;
static volatile LONG imgui_ready;
static int render_failed;
static float mouse_x = -1, mouse_y = -1, disp_w, disp_h;   // under cs
static ImDrawData snap;             // the last frame built on the game thread (cloned draw lists), under cs
static unsigned n_built, n_drawn;   // frames built (game thread) / rendered (RHI thread)

static void wait_gpu(UINT64 v) {
    if (v && fence->GetCompletedValue() < v) { fence->SetEventOnCompletion(v, fence_ev); WaitForSingleObject(fence_ev, 2000); }
}
static void release_buffers(void) {
    if (fence) wait_gpu(fence_val);
    for (UINT i = 0; i < MAXBUF; i++) if (backbuf[i]) { backbuf[i]->Release(); backbuf[i] = nullptr; }
}
static int get_buffers(IDXGISwapChain3 *sc) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < nbuf; i++) {
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&backbuf[i])))) return 0;
        rtv[i] = h; rtv[i].ptr += (SIZE_T)i * rtv_inc;
        dev->CreateRenderTargetView(backbuf[i], nullptr, rtv[i]);
    }
    return 1;
}
#define SRV_SLOTS 256   // font atlas pages + ov_texture pictures (at most OV_MAX_TEX), never freed
static void srv_alloc(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *c, D3D12_GPU_DESCRIPTOR_HANDLE *g) {
    UINT i = srv_used < SRV_SLOTS ? srv_used++ : SRV_SLOTS - 1;   // ov_texture keeps well below the end
    *c = srv_heap->GetCPUDescriptorHandleForHeapStart(); c->ptr += (SIZE_T)i * srv_inc;
    *g = srv_heap->GetGPUDescriptorHandleForHeapStart(); g->ptr += (UINT64)i * srv_inc;
}
static void srv_free(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

static int init_render(IDXGISwapChain3 *sc) {
    DXGI_SWAP_CHAIN_DESC d;
    if (FAILED(sc->GetDesc(&d)) || FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) return 0;
    nbuf = d.BufferCount < MAXBUF ? d.BufferCount : MAXBUF;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = MAXBUF;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) return 0;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = SRV_SLOTS; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap)))) return 0;
    rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < nbuf; i++)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i])))) return 0;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&cmdlist)))) return 0;
    cmdlist->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 0;
    fence_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!get_buffers(sc)) return 0;

    LOCKED;
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   // no imgui.ini in the game folder
    io.MouseDrawCursor = true;  // the game hides the OS cursor
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigErrorRecoveryEnableAssert = false;   // a panel's mistake shows a tooltip instead of stopping the game
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    apply_style(ui_scale);
    st.Colors[ImGuiCol_WindowBg].w = 0.80f;   // translucent
    st.Colors[ImGuiCol_ChildBg].w = 0.f;
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = dev; ii.CommandQueue = queue; ii.NumFramesInFlight = (int)nbuf;
    ii.RTVFormat = d.BufferDesc.Format; ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = srv_heap; ii.SrvDescriptorAllocFn = srv_alloc; ii.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&ii)) return 0;
    hwnd = d.OutputWindow;
    chain = sc;
    disp_w = (float)d.BufferDesc.Width; disp_h = (float)d.BufferDesc.Height;
    LOG("overlay: renderer ready (D3D12, %u buffers, format %d, window %p)", nbuf, (int)d.BufferDesc.Format, (void *)hwnd);
    return 1;
}

// RHI thread: draw the last snapshot onto the back buffer that is about to be presented
static void render(IDXGISwapChain3 *sc) {
    if (render_failed || !queue) return;
    if (!imgui_ready) {
        if (!init_render(sc)) { render_failed = 1; LOG("overlay: renderer init FAILED"); return; }
        InterlockedExchange(&imgui_ready, 1);
    }
    if (sc != chain) return;   // another swap chain (only ever one here)
    if (!backbuf[0] && !get_buffers(sc)) return;
    DXGI_SWAP_CHAIN_DESC d;
    sc->GetDesc(&d);
    UINT i = sc->GetCurrentBackBufferIndex() % nbuf;
    wait_gpu(alloc_fence[i]);   // outside the lock: the game thread never waits for the GPU through us
    LOCKED;
    disp_w = (float)d.BufferDesc.Width; disp_h = (float)d.BufferDesc.Height;
    if (!snap.Valid || !snap.CmdLists.Size) return;
    alloc[i]->Reset();
    cmdlist->Reset(alloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = backbuf[i];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmdlist->ResourceBarrier(1, &b);
    cmdlist->OMSetRenderTargets(1, &rtv[i], FALSE, nullptr);
    cmdlist->SetDescriptorHeaps(1, &srv_heap);
    ImGui_ImplDX12_RenderDrawData(&snap, cmdlist);   // also uploads font atlas changes (snap.Textures)
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdlist->ResourceBarrier(1, &b);
    cmdlist->Close();
    ID3D12CommandList *l = cmdlist;
    orig_ecl(queue, 1, &l);
    queue->Signal(fence, ++fence_val);
    alloc_fence[i] = fence_val;
    n_drawn++;
}

#ifndef B4B_RELEASE
static volatile LONG shot_state;   // dev `screenshot`: 0 idle, 1 requested (next Present copies), 2 copy/PNG in flight
static void shot_capture(IDXGISwapChain3 *sc);
#endif
static HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain3 *sc, UINT sync, UINT flags) {
    if (open_ && !(flags & DXGI_PRESENT_TEST)) render(sc);
#ifndef B4B_RELEASE
    if (shot_state == 1 && !(flags & DXGI_PRESENT_TEST)) shot_capture(sc);   // after the overlay: the frame as shown
#endif
    return orig_present(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain3 *sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (sc == chain) release_buffers();   // the swap chain's buffers must have no other references
    return orig_resize(sc, n, w, h, f, fl);
}
static void STDMETHODCALLTYPE ecl_hook(ID3D12CommandQueue *q, UINT n, ID3D12CommandList *const *lists) {
    if (!queue && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        queue = q;
        LOG("overlay: game's direct queue %p", (void *)q);
    }
    orig_ecl(q, n, lists);
}

// vtables from a throw-away device / queue / swap chain on a hidden window, then MinHook on the implementations
static int install_hooks(void) {
    HMODULE d3d12 = LoadLibraryA("d3d12.dll"), dxgi = LoadLibraryA("dxgi.dll");
    typedef HRESULT(WINAPI * CreateDev)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    typedef HRESULT(WINAPI * CreateFac)(REFIID, void **);
    CreateDev cd = d3d12 ? (CreateDev)(void *)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
    CreateFac cf = dxgi ? (CreateFac)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : nullptr;
    if (!cd || !cf) { LOG("overlay: no d3d12/dxgi"); return 0; }
    WNDCLASSEXA wc = {sizeof wc};
    wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "b4bcoop_overlay_probe";
    RegisterClassExA(&wc);
    HWND w = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    ID3D12Device *d = nullptr; ID3D12CommandQueue *q = nullptr; IDXGIFactory2 *fac = nullptr; IDXGISwapChain1 *sc1 = nullptr;
    int ok = 0;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 64; sd.Height = 64; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (w && SUCCEEDED(cd(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d))) &&
        SUCCEEDED(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && SUCCEEDED(cf(IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(fac->CreateSwapChainForHwnd(q, w, &sd, nullptr, nullptr, &sc1))) {
        void **vs = *(void ***)sc1, **vq = *(void ***)q;
        void *p = vs[8], *r = vs[13], *e = vq[10];   // IDXGISwapChain::Present, ResizeBuffers; ExecuteCommandLists
        ok = MH_CreateHook(p, (void *)present_hook, (void **)&orig_present) == MH_OK &&
             MH_CreateHook(r, (void *)resize_hook, (void **)&orig_resize) == MH_OK &&
             MH_CreateHook(e, (void *)ecl_hook, (void **)&orig_ecl) == MH_OK &&
             MH_EnableHook(p) == MH_OK && MH_EnableHook(r) == MH_OK && MH_EnableHook(e) == MH_OK;
        LOG("overlay: hooks %s (Present %p, ResizeBuffers %p, ExecuteCommandLists %p)", ok ? "installed" : "FAILED", p, r, e);
    } else LOG("overlay: probe device/swap chain failed");
    if (sc1) sc1->Release();
    if (fac) fac->Release();
    if (q) q->Release();
    if (d) d->Release();
    if (w) DestroyWindow(w);
    return ok;
}

#ifndef B4B_RELEASE
// ---- dev `screenshot <windows path>`: the presented frame (game + overlay) as a PNG ----
// Engine screenshots (`shot`) don't contain the overlay and come out black on some screens. Here Present (RHI thread)
// copies the back buffer that is about to be shown, after the overlay has drawn on it, into a readback buffer on the
// game's queue (one capture in flight); a worker thread waits for the copy, converts it to 8-bit RGB (8-bit, 10-bit and
// FP16 scRGB back buffers) and writes an uncompressed PNG (<path>.part, then renamed).
static ID3D12Device *shot_dev;
static ID3D12CommandAllocator *shot_alloc;
static ID3D12GraphicsCommandList *shot_list;
static ID3D12Fence *shot_fence;
static UINT64 shot_fence_val;
static HANDLE shot_ev;
static char shot_path[MAX_PATH], shot_result[260];
static struct { ID3D12Resource *rb; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT w, h; DXGI_FORMAT fmt; UINT64 v; } shot_job;
static unsigned shot_n;

static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_tab[1])
        for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
// PNG, 8-bit RGB, filter 0, zlib with stored (uncompressed) deflate blocks: a few dozen lines, no dependency
static int png_write(const char *path, const uint8_t *rgb, UINT w, UINT h) {
    size_t raw = (size_t)(w * 3 + 1) * h, nblk = (raw + 65534) / 65535, zlen = 2 + raw + nblk * 5 + 4;
    size_t total = 8 + 25 + 12 + zlen + 12;
    uint8_t *b = (uint8_t *)malloc(total), *q = b;
    if (!b) return 0;
    memcpy(q, "\x89PNG\r\n\x1a\n", 8); q += 8;
    be32(q, 13); memcpy(q + 4, "IHDR", 4); be32(q + 8, w); be32(q + 12, h);
    q[16] = 8; q[17] = 2; q[18] = q[19] = q[20] = 0;
    be32(q + 21, crc(0, q + 4, 17)); q += 25;
    be32(q, (uint32_t)zlen); memcpy(q + 4, "IDAT", 4);
    uint8_t *z = q + 8, *d = z + 2;
    z[0] = 0x78; z[1] = 0x01;
    uint32_t a1 = 1, a2 = 0;
    size_t left = raw, pos = 0, row = (size_t)w * 3 + 1;
    while (left) {
        size_t n = left < 65535 ? left : 65535;
        d[0] = left == n; d[1] = (uint8_t)n; d[2] = (uint8_t)(n >> 8); d[3] = (uint8_t)~n; d[4] = (uint8_t)(~n >> 8); d += 5;
        for (size_t i = 0; i < n; i++, pos++) {   // the raw stream: a filter byte (0) before each row
            size_t r = pos / row, c = pos % row;
            uint8_t v = c ? rgb[r * w * 3 + c - 1] : 0;
            *d++ = v;
            a1 = (a1 + v) % 65521; a2 = (a2 + a1) % 65521;
        }
        left -= n;
    }
    be32(d, a2 << 16 | a1); d += 4;
    be32(d, crc(0, q + 4, (size_t)(d - (q + 4)))); q = d + 4;
    be32(q, 0); memcpy(q + 4, "IEND", 4); be32(q + 8, crc(0, q + 4, 4)); q += 12;
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0;
    int ok = f != INVALID_HANDLE_VALUE && WriteFile(f, b, (DWORD)(q - b), &wr, nullptr) && wr == (DWORD)(q - b);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(b);
    return ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}
static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } u = s | e << 23 | (m & 1023) << 13; } }
    else if (e == 31) u = s | 0x7F800000u | m << 13;
    else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
static uint8_t lin_srgb(float v) {   // scRGB (linear, 1.0 = SDR white) -> sRGB byte
    if (!(v > 0)) return 0;
    if (v >= 1) return 255;
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
    return (uint8_t)(s * 255 + 0.5f);
}
static DWORD WINAPI shot_worker(void *) {
    int ok = 0;
    char why[120] = "";
    if (shot_fence->GetCompletedValue() < shot_job.v) { shot_fence->SetEventOnCompletion(shot_job.v, shot_ev); WaitForSingleObject(shot_ev, 5000); }
    uint8_t *m = nullptr, *rgb = nullptr;
    D3D12_RANGE rr = {0, (SIZE_T)shot_job.fp.Footprint.RowPitch * shot_job.h};
    if (shot_fence->GetCompletedValue() < shot_job.v) snprintf(why, sizeof why, "copy not done after 5 s");
    else if (FAILED(shot_job.rb->Map(0, &rr, (void **)&m))) snprintf(why, sizeof why, "Map failed");
    else if (!(rgb = (uint8_t *)malloc((size_t)shot_job.w * shot_job.h * 3))) snprintf(why, sizeof why, "out of memory");
    else {
        UINT w = shot_job.w, h = shot_job.h, pitch = shot_job.fp.Footprint.RowPitch;
        for (UINT y = 0; y < h; y++) {
            const uint8_t *s = m + (size_t)y * pitch;
            uint8_t *d = rgb + (size_t)y * w * 3;
            for (UINT x = 0; x < w; x++, d += 3) switch (shot_job.fmt) {
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
                d[0] = s[x * 4 + 2]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4]; break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                uint32_t v; memcpy(&v, s + x * 4, 4);
                d[0] = (uint8_t)((v & 1023) >> 2); d[1] = (uint8_t)((v >> 10 & 1023) >> 2); d[2] = (uint8_t)((v >> 20 & 1023) >> 2);
                break;
            }
            case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                uint16_t c[3]; memcpy(c, s + x * 8, 6);
                d[0] = lin_srgb(half_f(c[0])); d[1] = lin_srgb(half_f(c[1])); d[2] = lin_srgb(half_f(c[2]));
                break;
            }
            default: d[0] = s[x * 4]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 2];   // R8G8B8A8 (checked at capture)
            }
        }
        D3D12_RANGE none = {0, 0};
        shot_job.rb->Unmap(0, &none);
        m = nullptr;
        ok = png_write(shot_path, rgb, w, h);
        if (!ok) snprintf(why, sizeof why, "cannot write the file (error %lu)", GetLastError());
    }
    if (m) { D3D12_RANGE none = {0, 0}; shot_job.rb->Unmap(0, &none); }
    free(rgb);
    if (shot_fence->GetCompletedValue() >= shot_job.v) shot_job.rb->Release();   // else the GPU may still write it: leak
    if (ok) snprintf(shot_result, sizeof shot_result, "wrote %s (%ux%u, format %d)", shot_path, shot_job.w, shot_job.h, (int)shot_job.fmt);
    else snprintf(shot_result, sizeof shot_result, "FAILED %s: %s", shot_path, why);
    LOG("screenshot: %s", shot_result);
    shot_n++;
    InterlockedExchange(&shot_state, 0);
    return 0;
}
static int shot_fmt_ok(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: return 1;
    default: return 0;
    }
}
static void shot_fail(const char *why) {
    snprintf(shot_result, sizeof shot_result, "FAILED %s: %s", shot_path, why);
    LOG("screenshot: %s", shot_result);
    InterlockedExchange(&shot_state, 0);
}
// RHI thread, in Present: copy the current back buffer (state PRESENT) to a new readback buffer on the game's queue
static void shot_capture(IDXGISwapChain3 *sc) {
    if (!queue) return;   // not seen the game's queue yet: the next Present
    if (!shot_dev) {
        if (FAILED(sc->GetDevice(IID_PPV_ARGS(&shot_dev))) ||
            FAILED(shot_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&shot_alloc))) ||
            FAILED(shot_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, shot_alloc, nullptr, IID_PPV_ARGS(&shot_list))) ||
            FAILED(shot_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&shot_fence)))) {
            if (shot_dev) { shot_dev->Release(); shot_dev = nullptr; }
            return shot_fail("D3D12 setup failed");
        }
        shot_list->Close();
        shot_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    ID3D12Resource *bb = nullptr;
    if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)))) return shot_fail("GetBuffer failed");
    D3D12_RESOURCE_DESC rd = bb->GetDesc();
    if (!shot_fmt_ok(rd.Format)) {
        bb->Release();
        char w[64]; snprintf(w, sizeof w, "unsupported back buffer format %d", (int)rd.Format);
        return shot_fail(w);
    }
    UINT64 total = 0;
    shot_dev->GetCopyableFootprints(&rd, 0, 1, 0, &shot_job.fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    shot_job.rb = nullptr;
    if (FAILED(shot_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&shot_job.rb)))) { bb->Release(); return shot_fail("no readback buffer"); }
    shot_job.w = (UINT)rd.Width; shot_job.h = rd.Height; shot_job.fmt = rd.Format;
    shot_alloc->Reset();   // the previous capture's copy is done (the worker waited for it)
    shot_list->Reset(shot_alloc, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    shot_list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = shot_job.rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = shot_job.fp;
    src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    shot_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    shot_list->ResourceBarrier(1, &b);
    shot_list->Close();
    ID3D12CommandList *l = shot_list;
    orig_ecl(queue, 1, &l);
    queue->Signal(shot_fence, ++shot_fence_val);
    shot_job.v = shot_fence_val;
    bb->Release();
    InterlockedExchange(&shot_state, 2);
    HANDLE t = CreateThread(nullptr, 0, shot_worker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else shot_worker(nullptr);
}
#endif

// ---- window log (replies of ov_run, chat_local lines) ----
#define LOG_LINES 80
static char log_buf[LOG_LINES][200];
static int log_head, log_n;
static int log_scroll;   // new lines: scroll the log to the end
extern "C" void overlay_note(const char *text) {
    if (!cs_ready) return;
    LOCKED;
    for (const char *p = text; *p;) {
        size_t n = strcspn(p, "\n");
        if (n) {
            snprintf(log_buf[(log_head + log_n) % LOG_LINES], sizeof log_buf[0], "%.*s", (int)(n < 199 ? n : 199), p);
            if (log_n < LOG_LINES) log_n++; else log_head = (log_head + 1) % LOG_LINES;
        }
        p += n;
        if (*p) p++;
    }
    log_scroll = 1;
}
static void notef(const char *fmt, ...) {
    char b[400];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    overlay_note(b);
}

// ---- panel registry ----
struct Panel { char name[32]; int order; OverlayDrawFn draw; };
static Panel panels[24];
static int n_panels;
extern "C" void overlay_add_panel(const char *name, int order, OverlayDrawFn draw) {
    int i = 0;
    while (i < n_panels && strcmp(panels[i].name, name)) i++;
    if (!draw) {   // remove it (e.g. updates=0 while the game runs)
        if (i < n_panels) { memmove(&panels[i], &panels[i + 1], (n_panels - i - 1) * sizeof *panels); n_panels--; }
        return;
    }
    if (i == n_panels) {
        if (n_panels == 24) { LOG("overlay: too many panels, %s dropped", name); return; }
        n_panels++;
    }
    snprintf(panels[i].name, sizeof panels[i].name, "%s", name);
    panels[i].order = order; panels[i].draw = draw;
    for (int a = 1; a < n_panels; a++)   // keep them sorted by order (tiny insertion sort)
        for (int b = a; b > 0 && panels[b].order < panels[b - 1].order; b--) { Panel t = panels[b]; panels[b] = panels[b - 1]; panels[b - 1] = t; }
}
// Other names of a tab (dev `overlay tab <alias>`): that tab is shown and select(arg) runs (e.g. a mode inside it).
struct Alias { char name[32], panel[32]; void (*select)(int); int arg; };
static Alias aliases[16];
static int n_aliases;
extern "C" void overlay_add_alias(const char *alias, const char *panel, void (*select)(int), int arg) {
    if (n_aliases == 16) return;
    Alias *a = &aliases[n_aliases++];
    snprintf(a->name, sizeof a->name, "%s", alias);
    snprintf(a->panel, sizeof a->panel, "%s", panel);
    a->select = select; a->arg = arg;
}

// ---- test driving (dev `overlay press|set`): a control consumes a pending request that names its label ----
static char drive_label[128], drive_value[256], drive_result[200], cur_tab[32], select_tab[32];
static int drive_kind;          // 0 none, 1 press, 2 set, 3 locate (report the control's rectangle)
static unsigned drive_frames;   // frames the request has waited
static ULONGLONG drive_t0;
static int drive_done_item;     // the last control was set by a request: ov_edit_done() returns 1
static int label_match(const char *label, const char *want) {
    if (!_stricmp(label, want)) return 1;
    if (want[0] == '#' && want[1] == '#') {   // "##id": the id part alone (rows whose shown text varies)
        const char *id = strstr(label, "##");
        while (id && id[2] == '#') id++;
        return id && !_stricmp(id, want);
    }
    if (strstr(want, "##")) return 0;
    size_t n = strstr(label, "##") ? (size_t)(strstr(label, "##") - label) : strlen(label);
    return strlen(want) == n && !_strnicmp(label, want, n);
}
static struct { int disabled; const char *why; } dis[8];   // ov_begin_disabled stack
static int dis_n, dis_depth;    // stack size, levels that actually disable
static const char *drive_take(const char *label, int kind) {   // the value (or "") if this control is the target
    drive_done_item = 0;
    if (drive_kind == 3 && label_match(label, drive_label)) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        snprintf(drive_result, sizeof drive_result, "'%s' at %.0f,%.0f-%.0f,%.0f (centre %.0f %.0f)%s", label, a.x, a.y, b.x,
                 b.y, (a.x + b.x) / 2, (a.y + b.y) / 2, dis_depth ? ", disabled" : "");
        drive_kind = 0;
        return nullptr;
    }
    if (!drive_kind || dis_depth || !label_match(label, drive_label)) return nullptr;
    if (kind == 1 && drive_kind != 1) return nullptr;
    snprintf(drive_result, sizeof drive_result, "%s '%s'%s%s in %s", drive_kind == 1 ? "pressed" : "set", label,
             drive_kind == 2 ? " = " : "", drive_kind == 2 ? drive_value : "", cur_tab);
    LOG("overlay: test %s", drive_result);
    drive_kind = 0;
    drive_done_item = 1;
    return drive_value;
}

// ---- widgets (game thread, inside a panel) ----
static void after_item(void) {   // the reason a disabled control is disabled, on hover
    const char *why = dis_depth && dis_n ? dis[(dis_n > 8 ? 8 : dis_n) - 1].why : nullptr;
    if (why && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why);
}
extern "C" void ov_text(const char *fmt, ...) {
    LOCKED; va_list ap; va_start(ap, fmt); ImGui::TextV(fmt, ap); va_end(ap);
}
extern "C" void ov_text_dim(const char *fmt, ...) {
    LOCKED;
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    va_list ap; va_start(ap, fmt); ImGui::TextWrappedV(fmt, ap); va_end(ap);
    ImGui::PopStyleColor();
}
extern "C" void ov_text_warn(const char *fmt, ...) {
    LOCKED;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.78f, 0.35f, 1.f));
    va_list ap; va_start(ap, fmt); ImGui::TextWrappedV(fmt, ap); va_end(ap);
    ImGui::PopStyleColor();
}
extern "C" void ov_heading(const char *text) {   // a section: extra air above it (not at the top of a panel)
    LOCKED;
    if (ImGui::GetCursorPosY() > ImGui::GetFontSize()) ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.5f));
    ImGui::SeparatorText(text);
}
extern "C" int ov_button(const char *label) {
    LOCKED;
    int r = ImGui::Button(label);
    after_item();
    return r || drive_take(label, 1);
}
extern "C" int ov_button_confirm(const char *label, const char *confirm) {
    static ImGuiID armed; static double armed_at;
    LOCKED;
    ImGuiID id = ImGui::GetID(label);
    double now = ImGui::GetTime();
    int is_armed = armed == id && now - armed_at < 3.0;
    char b[160];
    snprintf(b, sizeof b, "%s###%s", is_armed ? confirm : label, label);
    if (is_armed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.25f, 0.2f, 1.f));
    int r = ImGui::Button(b);
    if (is_armed) ImGui::PopStyleColor();
    after_item();
    if (drive_take(label, 1)) { armed = 0; return 1; }
    if (!r) return 0;
    if (is_armed) { armed = 0; return 1; }
    armed = id; armed_at = now;
    return 0;
}
extern "C" int ov_checkbox(const char *label, int *v) {
    LOCKED;
    bool b = *v != 0;
    int r = ImGui::Checkbox(label, &b);
    after_item();
    if (r) *v = b;
    if (const char *d = drive_take(label, 2)) {   // set: 1/0/on/off; press: toggle
        *v = *d ? (atoi(d) != 0 || !_stricmp(d, "on") || !_stricmp(d, "true")) : !*v;
        r = 1;
    }
    return r;
}
extern "C" int ov_radio(const char *label, int active) {
    LOCKED;
    int r = ImGui::RadioButton(label, active != 0);
    after_item();
    return r || drive_take(label, 1);
}
extern "C" int ov_selectable(const char *label, int selected) {
    LOCKED;
    int r = ImGui::Selectable(label, selected != 0);
    after_item();
    return r || drive_take(label, 1);
}
extern "C" int ov_slider(const char *label, float *v, float lo, float hi, const char *fmt) {
    LOCKED;
    int r = ImGui::SliderFloat(label, v, lo, hi, fmt ? fmt : "%.0f", ImGuiSliderFlags_AlwaysClamp);
    after_item();
    if (const char *d = drive_take(label, 2)) { float f = (float)atof(d); *v = f < lo ? lo : f > hi ? hi : f; r = 1; }
    return r;
}
extern "C" int ov_slider_int(const char *label, int *v, int lo, int hi) {
    LOCKED;
    int r = ImGui::SliderInt(label, v, lo, hi, "%d", ImGuiSliderFlags_AlwaysClamp);
    after_item();
    if (const char *d = drive_take(label, 2)) { int f = atoi(d); *v = f < lo ? lo : f > hi ? hi : f; r = 1; }
    return r;
}
extern "C" int ov_input_text(const char *label, char *buf, int n, const char *hint) {
    LOCKED;
    int r = hint ? ImGui::InputTextWithHint(label, hint, buf, (size_t)n, ImGuiInputTextFlags_EnterReturnsTrue)
                 : ImGui::InputText(label, buf, (size_t)n, ImGuiInputTextFlags_EnterReturnsTrue);
    after_item();
    if (const char *d = drive_take(label, 2)) { snprintf(buf, (size_t)n, "%s", d); r = 1; }
    return r;
}
extern "C" int ov_input_int(const char *label, int *v, int lo, int hi) {
    LOCKED;
    int r = ImGui::InputInt(label, v);
    after_item();
    if (const char *d = drive_take(label, 2)) { *v = atoi(d); r = 1; }
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
    return r;
}
extern "C" int ov_combo(const char *label, int *cur, const char *const *items, int n) {
    LOCKED;
    int r = 0;
    if (*cur >= n) *cur = n - 1;
    if (*cur < 0) *cur = 0;
    if (ImGui::BeginCombo(label, n ? items[*cur] : "")) {
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(items[i], i == *cur)) { *cur = i; r = 1; }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    after_item();
    if (const char *d = drive_take(label, 2)) {
        for (int i = 0; i < n; i++) if (!_stricmp(items[i], d) || !_strnicmp(items[i], d, strlen(d))) { *cur = i; r = 1; break; }
    }
    return r;
}
extern "C" int ov_edit_done(void) { LOCKED; return ImGui::IsItemDeactivatedAfterEdit() || drive_done_item; }

// key capture: the window procedure fills `cap_vk` while `cap_id` waits (0 none; -2 cancelled)
static ImGuiID cap_id;
static volatile LONG cap_state;   // 0 idle, 1 waiting for a key, 2 got one
static int cap_vk;
extern "C" const char *ov_key_name(int vk) {
    static char b[8][16]; static int k;
    char *s = b[k++ & 7];
    if (!vk) return "none";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(s, 16, "%c", vk); return s; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(s, 16, "F%d", vk - VK_F1 + 1); return s; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(s, 16, "Num %d", vk - VK_NUMPAD0); return s; }
    switch (vk) {
    case VK_OEM_3: return "~"; case VK_TAB: return "Tab"; case VK_CAPITAL: return "Caps Lock"; case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete"; case VK_HOME: return "Home"; case VK_END: return "End"; case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down"; case VK_MBUTTON: return "Middle mouse"; case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5"; case VK_PAUSE: return "Pause"; case VK_OEM_MINUS: return "-"; case VK_OEM_PLUS: return "=";
    case VK_OEM_4: return "["; case VK_OEM_6: return "]"; case VK_OEM_5: return "\\"; case VK_OEM_1: return ";";
    case VK_OEM_7: return "'"; case VK_OEM_COMMA: return ","; case VK_OEM_PERIOD: return "."; case VK_OEM_2: return "/";
    }
    snprintf(s, 16, "0x%02X", vk);
    return s;
}
extern "C" int ov_key(const char *label, int *vk) {
    LOCKED;
    ImGuiID id = ImGui::GetID(label);
    int waiting = cap_id == id && cap_state == 1, r = 0;
    if (cap_id == id && cap_state == 2) {
        if (cap_vk != -2) { *vk = cap_vk; r = 1; }
        cap_id = 0; InterlockedExchange(&cap_state, 0);
    }
    char b[160], vis[64];
    size_t n = strstr(label, "##") ? (size_t)(strstr(label, "##") - label) : strlen(label);
    snprintf(vis, sizeof vis, "%.*s", (int)n, label);
    snprintf(b, sizeof b, "%s###%s", waiting ? "press a key (Esc cancels, Backspace = none)" : ov_key_name(*vk), label);
    if (ImGui::Button(b, ImVec2(ImGui::GetFontSize() * 7.f, 0)) && !waiting) { cap_id = id; InterlockedExchange(&cap_state, 1); }
    after_item();
    if (vis[0]) { ImGui::SameLine(); ImGui::TextUnformatted(vis); }
    if (const char *d = drive_take(label, 2)) { *vk = cmds_parse_key(d); r = 1; }
    return r;
}
// Pictures (shop thumbnails): an RGBA copy handed to ImGui's texture list; the DX12 backend uploads it on the render
// thread with the next frame. Kept for the whole session (never destroyed: no frame in flight can still use a freed
// one); OV_MAX_TEX of them at most, so the descriptor heap (SRV_SLOTS) never runs out.
static ImTextureData *user_tex[OV_MAX_TEX + 1];
static int n_user_tex;
extern "C" int ov_texture(const unsigned char *rgba, int w, int h) {
    LOCKED;
    if (!imgui_ready || !rgba || w <= 0 || h <= 0 || w > 512 || h > 512 || n_user_tex >= OV_MAX_TEX) return 0;
    ImTextureData *t = IM_NEW(ImTextureData)();
    t->Create(ImTextureFormat_RGBA32, w, h);
    memcpy(t->Pixels, rgba, (size_t)w * h * 4);
    ImGui::RegisterUserTexture(t);
    user_tex[++n_user_tex] = t;
    return n_user_tex;
}
extern "C" void ov_image(int tex, float w_em, float h_em) {
    LOCKED;
    ImVec2 sz(ImGui::GetFontSize() * w_em, ImGui::GetFontSize() * h_em);
    if (tex > 0 && tex <= n_user_tex) ImGui::Image(user_tex[tex]->GetTexRef(), sz);
    else ImGui::Dummy(sz);
}
extern "C" void ov_same_line(void) { LOCKED; ImGui::SameLine(); }
extern "C" void ov_separator(void) { LOCKED; ImGui::Separator(); }
extern "C" void ov_width(float em) { LOCKED; if (em > 0) ImGui::SetNextItemWidth(ImGui::GetFontSize() * em); }
extern "C" void ov_tooltip(const char *text) { LOCKED; if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text); }
extern "C" int ov_header(const char *label, int default_open) {
    LOCKED;
    ImGuiID id = ImGui::GetID(label);
    int open = ImGui::CollapsingHeader(label, default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    after_item();
    if (drive_take(label, 1)) ImGui::GetStateStorage()->SetInt(id, !open);   // dev press: opens/closes it next frame
    return open;
}
// A row of tabs inside a panel (a mode switch): *cur = the one shown; setting *cur selects it from the next frame.
extern "C" int ov_tabs(const char *id, const char *const *labels, int n, int *cur) {
    LOCKED;
    ImGuiStorage *st = ImGui::GetStateStorage();
    ImGuiID key = ImGui::GetID(id);
    int last = st->GetInt(key, -1), shown = -1, driven = -1, was = *cur;
    if (ImGui::BeginTabBar(id)) {
        for (int i = 0; i < n; i++) {
            bool sel = ImGui::BeginTabItem(labels[i], nullptr, i == *cur && i != last ? ImGuiTabItemFlags_SetSelected : 0);
            after_item();
            if (drive_take(labels[i], 1)) driven = i;
            if (sel) { shown = i; ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
    if (driven >= 0) *cur = driven;
    else if (last >= 0 && shown >= 0 && shown != last) *cur = shown;   // clicked (or our SetSelected arrived)
    if (last >= 0 && shown >= 0 && shown != last) ImGui::SetScrollY(0);   // another mode starts at its top
    st->SetInt(key, shown);
    return *cur != was;
}
extern "C" void ov_push_id(int id) { LOCKED; ImGui::PushID(id); }
extern "C" void ov_pop_id(void) { LOCKED; ImGui::PopID(); }
extern "C" int ov_table_begin(const char *id, int cols) {
    LOCKED;
    return ImGui::BeginTable(id, cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp);
}
extern "C" void ov_table_header(const char *const *names, int n) {
    LOCKED;
    for (int i = 0; i < n; i++) ImGui::TableSetupColumn(names[i]);
    ImGui::TableHeadersRow();
}
extern "C" void ov_table_next(void) { LOCKED; ImGui::TableNextColumn(); }
extern "C" void ov_table_end(void) { LOCKED; ImGui::EndTable(); }
extern "C" void ov_copy(const char *text) { LOCKED; ImGui::SetClipboardText(text); notef("copied: %s", text); }

extern "C" void ov_begin_disabled(int disabled, const char *why) {
    LOCKED;
    ImGui::BeginDisabled(disabled != 0);
    if (dis_n < 8) { dis[dis_n].disabled = disabled != 0; dis[dis_n].why = disabled ? why : dis_n ? dis[dis_n - 1].why : nullptr; }
    dis_n++;
    if (disabled) dis_depth++;
}
extern "C" void ov_end_disabled(void) {
    LOCKED;
    if (!dis_n) return;
    dis_n--;
    if (dis_n < 8 && dis[dis_n].disabled && dis_depth) dis_depth--;
    ImGui::EndDisabled();
}
extern "C" int ov_allowed(int perm, const char **why) {
    static const char *w;
    if (!why) why = &w;
    *why = nullptr;
    if (perm == CMD_ANYONE) return 1;
    if (perm == CMD_OWNSAVE) return cheats_own_save_ok(why);   // own save: host with cheats on, or the host said so
    if (admin_is_client()) { *why = "Host only: you are in someone else's session."; return 0; }
    if (perm == CMD_CHEAT && !cheats_enabled()) { *why = "Turn cheats on first (Cheats on, above)."; return 0; }
    return 1;
}
static const char *warned;   // the reason ov_begin_perm last printed in this panel (printed once)
extern "C" int ov_begin_perm(int perm) {
    const char *why;
    int ok = ov_allowed(perm, &why);
    if (!ok && why != warned) ov_text_warn("%s", why);
    warned = why;
    ov_begin_disabled(!ok, why);
    return ok;
}
extern "C" void ov_end_perm(void) { ov_end_disabled(); }

// ---- actions ----
extern "C" void ov_run(const char *fmt, ...) {
    static Out o;
    char line[400];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    notef("> /%s", line);
    LOG("overlay: /%s", line);
    out_reset(&o);
    admin_slash(line, &o);   // the chat command path: same permission check and handler as typing it
    if (o.len) overlay_note(o.buf);
}
extern "C" void ov_setting(const char *key, const char *val, int save) {
    if (!val && !cmds_ini_value(key)) save = 0;   // back to the default and not in the file: nothing to write
    int r = cmds_ini_apply(key, val, save);
    if (save) notef(r ? "could not write %s to b4bcoop.ini" : val ? "b4bcoop.ini: %s=%s" : "b4bcoop.ini: %s back to the default",
                    key, val ? val : "");
}
extern "C" void ov_setting_f(const char *key, float v, int save) {
    char b[32];
    snprintf(b, sizeof b, (v < 10 && v > -10 && v != (int)v) ? "%.2f" : "%.0f", v);
    ov_setting(key, b, save);
}
extern "C" void ov_setting_key(const char *k, int vk) {   // ini value for a key binding
    char b[16];
    if (!vk) snprintf(b, sizeof b, "off");
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(b, sizeof b, "%c", vk);
    else snprintf(b, sizeof b, "0x%02X", vk);
    ov_setting(k, b, 1);
}

// ---- built-in panels ----
static void panel_settings(void) {
    ov_heading("Window");
    float s = ui_scale;
    ov_width(12);
    if (ov_slider("Text size##scale", &s, 0.75f, 3.f, "%.2f")) ov_setting_f("overlay_scale", s, 0);
    if (ov_edit_done()) ov_setting_f("overlay_scale", s, 1);
    int k = key;
    if (ov_key("Open / close this window##overlay_key", &k)) ov_setting_key("overlay_key", k ? k : VK_OEM_3);
    ov_text_dim("Esc also closes it. overlay=0 in b4bcoop.ini turns the window off.");
    ov_heading("Keys");
    k = flashlight_hotkey();
    if (ov_key("Flashlight##flashlight_key", &k)) ov_setting_key("flashlight_key", k);
    k = thirdperson_hotkey();
    if (ov_key("Third person##thirdperson_key", &k)) ov_setting_key("thirdperson_key", k);
    k = bots_heal_hotkey();
    if (ov_key("Ask the bots to heal you##bot_heal_key", &k)) ov_setting_key("bot_heal_key", k);
    ov_text_dim("Changes here apply at once and are saved to b4bcoop.ini (only the settings you change). Editing "
                "b4bcoop.ini while the game runs updates this window too.");
    ov_text_dim("File: %s", cmds_config_path());
    online_settings_panel();
}
static void panel_help(void) {
    ov_text("b4bcoop %s (protocol %d)", coop_version(), coop_protocol());
    ov_text_dim("Every chat command has a control in these tabs, with the same rules: host-only controls are greyed out "
                "on a client (hover for why), cheats need Cheats on. Replies show in the log below. The chat commands "
                "still work (/help in chat).");
    ov_heading("Run a chat command");
    static char cmd[200];
    ov_width(18);
    int go = ov_input_text("##cmd", cmd, sizeof cmd, "e.g. players, thirdperson distance 200");
    ov_same_line();
    if ((ov_button("Run") || go) && cmd[0]) { ov_run("%s", cmd[0] == '/' ? cmd + 1 : cmd); cmd[0] = 0; }
    if (ov_button("/help")) ov_run("help");
    ov_same_line();
    if (ov_button("/cheats help")) ov_run("cheats help");
}

// ---- frame (game thread) ----
static void role_line(char *b, size_t n) {
    UObject *w = ue_world();
    char pkg[256] = "";
    if (w) ue_world_package(w, pkg, sizeof pkg);
    const char *map = strrchr(pkg, '/') ? strrchr(pkg, '/') + 1 : pkg;
    if (!w || !ue_local_pc()) snprintf(b, n, "loading");
    else if (admin_is_client()) snprintf(b, n, "in a friend's session, %s", map);
    else if (ue_is_listen_server(w)) snprintf(b, n, "hosting, %d friend(s) connected, %s", ue_num_clients(w), map);
    else snprintf(b, n, "playing alone, %s", map);
}

static void snapshot(void) {   // under cs, right after ImGui::Render()
    for (ImDrawList *l : snap.CmdLists) IM_DELETE(l);
    snap.Clear();
    ImDrawData *d = ImGui::GetDrawData();
    if (!d || !d->Valid) return;
    snap.Valid = true;
    snap.FrameCount = d->FrameCount;
    snap.DisplayPos = d->DisplayPos; snap.DisplaySize = d->DisplaySize; snap.FramebufferScale = d->FramebufferScale;
    snap.OwnerViewport = d->OwnerViewport;
    snap.Textures = d->Textures;
    for (ImDrawList *l : d->CmdLists) snap.CmdLists.push_back(l->CloneOutput());
    snap.TotalVtxCount = d->TotalVtxCount; snap.TotalIdxCount = d->TotalIdxCount;
}

static void build_frame(void) {
    static LARGE_INTEGER last, freq;
    char role[200];
    role_line(role, sizeof role);   // UE reads: outside the lock
    {
        LOCKED;
        if (!imgui_ready || disp_w < 1 || disp_h < 1) return;
        ImGuiIO &io = ImGui::GetIO();
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 1.f / 60;
        last = now;
        io.DeltaTime = dt <= 0 ? 1e-4f : dt > 0.2f ? 0.2f : dt;
        io.DisplaySize = ImVec2(disp_w, disp_h);
        if (mouse_x < 0) { mouse_x = disp_w / 2; mouse_y = disp_h / 2; io.AddMousePosEvent(mouse_x, mouse_y); }
        if (style_scale != ui_scale) apply_style(ui_scale);
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        float fs = ImGui::GetFontSize();
        ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(fs * 36, fs * 42 < disp_h - 60 ? fs * 42 : disp_h - 60), ImGuiCond_FirstUseEver);   // roomier rows (#44): taller
        ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 16, fs * 12), ImVec2(disp_w, disp_h));
        char title[80];
        snprintf(title, sizeof title, "b4bcoop  (%s or Esc closes)###b4bcoop", ov_key_name(key));
        ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::TextDisabled("b4bcoop %s: %s", coop_version(), role);
        ImGui::BeginTabBar("tabs", ImGuiTabBarFlags_FittingPolicyScroll);
    }
    for (int i = 0; i < n_panels; i++) {
        bool show;
        {
            LOCKED;
            int sel = select_tab[0] && !_stricmp(select_tab, panels[i].name);
            if (sel) select_tab[0] = 0;
            show = ImGui::BeginTabItem(panels[i].name, nullptr, sel ? ImGuiTabItemFlags_SetSelected : 0);
            if (show) {
                snprintf(cur_tab, sizeof cur_tab, "%s", panels[i].name);
                float log_h = (ImGui::GetTextLineHeight() + LOG_SPACING) * 6 + ImGui::GetStyle().ItemSpacing.y * 3;
                ImGui::BeginChild("panel", ImVec2(0, -log_h));
                ImGui::PushTextWrapPos(0);
            }
        }
        if (!show) continue;
        dis_n = dis_depth = 0;
        warned = nullptr;
        panels[i].draw();
        LOCKED;
        while (dis_n) { dis_n--; ImGui::EndDisabled(); }   // a panel that forgot ov_end_perm
        dis_depth = 0;
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    LOCKED;
    ImGui::EndTabBar();
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, LOG_SPACING));   // log lines stay dense
    for (int i = 0; i < log_n; i++) ImGui::TextUnformatted(log_buf[(log_head + i) % LOG_LINES]);
    if (log_scroll) { ImGui::SetScrollHereY(1.f); log_scroll = 0; }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::End();
    ImGui::Render();
    snapshot();
    n_built++;
    if (drive_kind && ++drive_frames > 10 && GetTickCount64() - drive_t0 > 3000) {   // nothing on the shown tab has that label
        snprintf(drive_result, sizeof drive_result, "no control '%s' on tab %s (or it is disabled)", drive_label, cur_tab);
        LOG("overlay: test %s", drive_result);
        drive_kind = 0;
    }
}

// ---- input (game thread: the window procedure) ----
static ImGuiKey vk_to_key(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow; case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End; case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace;
    case VK_RETURN: return ImGuiKey_Enter; case VK_SPACE: return ImGuiKey_Space; case VK_CONTROL: return ImGuiKey_LeftCtrl;
    case VK_SHIFT: return ImGuiKey_LeftShift; case VK_MENU: return ImGuiKey_LeftAlt; case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    default: return ImGuiKey_None;
    }
}
static void set_open(int on) {
    InterlockedExchange(&open_, on);
    {
        LOCKED;
        if (imgui_ready && on) {   // start the software cursor in the middle
            mouse_x = disp_w / 2; mouse_y = disp_h / 2;
            ImGui::GetIO().AddMousePosEvent(mouse_x, mouse_y);
        }
        if (!on) { for (ImDrawList *l : snap.CmdLists) IM_DELETE(l); snap.Clear(); cap_id = 0; cap_state = 0; }
    }
    LOG("overlay: %s", on ? "open" : "closed");
}
static void capture(int vk) {   // a key for ov_key
    cap_vk = vk == VK_ESCAPE ? -2 : (vk == VK_BACK || vk == VK_DELETE) ? 0 : vk;
    InterlockedExchange(&cap_state, 2);
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (!open_ || !imgui_ready) return CallWindowProcW(orig_wndproc, h, m, wp, lp);
    ImGuiIO &io = ImGui::GetIO();
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (cap_state == 1) {
            if (wp != VK_SHIFT && wp != VK_CONTROL && wp != VK_MENU) capture((int)wp);
            return 0;
        }
        if (wp == (WPARAM)key || wp == VK_ESCAPE) { set_open(0); return 0; }
        {
            LOCKED;
            if (ImGuiKey k = vk_to_key(wp)) io.AddKeyEvent(k, true);
            if (wp == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            if (wp == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, true);
        }
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP: {
        LOCKED;
        if (ImGuiKey k = vk_to_key(wp)) io.AddKeyEvent(k, false);
        if (wp == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (wp == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, false);
        break;   // releases reach the game too
    }
    case WM_CHAR:
        if (cap_state == 0 && wp != (WPARAM)'`' && wp != (WPARAM)'~' && wp >= 32) { LOCKED; io.AddInputCharacterUTF16((unsigned short)wp); }
        return 0;
    case WM_INPUT: {
        RAWINPUT ri; UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 && ri.header.dwType == RIM_TYPEMOUSE) {
            const RAWMOUSE &rm = ri.data.mouse;
            if (cap_state == 1) {   // a mouse button as a key binding (middle, 4, 5)
                if (rm.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) capture(VK_MBUTTON);
                if (rm.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) capture(VK_XBUTTON1);
                if (rm.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) capture(VK_XBUTTON2);
            }
            {
                LOCKED;
                if (!(rm.usFlags & MOUSE_MOVE_ABSOLUTE) && (rm.lLastX || rm.lLastY)) {
                    mouse_x += rm.lLastX; mouse_y += rm.lLastY;
                    if (mouse_x < 0) mouse_x = 0; if (mouse_y < 0) mouse_y = 0;
                    if (mouse_x > disp_w - 1) mouse_x = disp_w - 1; if (mouse_y > disp_h - 1) mouse_y = disp_h - 1;
                    io.AddMousePosEvent(mouse_x, mouse_y);
                }
                static const USHORT dn[] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_DOWN};
                static const USHORT up[] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP};
                for (int b = 0; b < 3; b++) {
                    if (rm.usButtonFlags & dn[b]) io.AddMouseButtonEvent(b, true);
                    if (rm.usButtonFlags & up[b]) io.AddMouseButtonEvent(b, false);
                }
                if (rm.usButtonFlags & RI_MOUSE_WHEEL) io.AddMouseWheelEvent(0, (float)(SHORT)rm.usButtonData / WHEEL_DELTA);
            }
            // button releases still go to the game (nothing stays held); everything else stops here
            if (rm.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP |
                                    RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP))
                return CallWindowProcW(orig_wndproc, h, m, wp, lp);
        }
        return DefWindowProcW(h, m, wp, lp);
    }
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_MOUSEWHEEL:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_XBUTTONDOWN:
        return 0;   // the software cursor follows raw input
    }
    return CallWindowProcW(orig_wndproc, h, m, wp, lp);
}

// ---- C interface ----
extern "C" int overlay_live(const char *k, const char *v) {
    if (!strcmp(k, "overlay")) enabled = v ? atoi(v) != 0 : 1;
    else if (!strcmp(k, "overlay_key")) { key = v ? cmds_parse_key(v) : VK_OEM_3; if (!key) key = VK_OEM_3; }
    else if (!strcmp(k, "overlay_scale")) { ui_scale = v ? (float)atof(v) : 1.25f; if (ui_scale < 0.5f) ui_scale = 0.5f; if (ui_scale > 4) ui_scale = 4; }
    else return 0;
    if (!enabled && open_) set_open(0);
    return 1;
}
static void ini_pair(const char *k, const char *v, void *) { overlay_live(k, v); }
extern "C" int overlay_init(void) {
    InitializeCriticalSection(&cs);
    cs_ready = 1;
    cmds_ini_each(ini_pair, nullptr);
    overlay_add_panel("Settings", 90, panel_settings);
    overlay_add_panel("Help", 100, panel_help);
    LOG("overlay: %s, key 0x%02x, scale %.2f (hooks on first open)", enabled ? "on" : "off (overlay=0)", key, ui_scale);
    return 0;
}
static void try_open(void) {
    if (!hooks) hooks = install_hooks() ? 1 : -1;
    if (hooks == 1) set_open(1);
}
// game thread, every tick: the open key (through the game's input, so typing in chat doesn't open it), first-open
// setup, the frame
extern "C" void overlay_tick(float) {
    static int was_down;
    if (!enabled) return;
    int down = key && !open_ && cmds_hotkey_down(key);
    if (down && !was_down && !open_) try_open();
    was_down = down;
    if (hooks != 1) return;
    if (!orig_wndproc && hwnd) {
        orig_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
        LOG("overlay: input hooked on window %p", (void *)hwnd);
    }
    if (open_ && imgui_ready) build_frame();
}
extern "C" int overlay_is_open(void) { return open_ != 0; }

#ifndef B4B_RELEASE
// dev: overlay [open|close|status|log|tab <name>|press <label>|set <label> <value>|locate <label>|mouse <x> <y>|wheel <n>]
// dev: screenshot <windows path> (the next presented frame, overlay included, as a PNG; installs the Present hook if
// the overlay was never opened) | screenshot (state and the last result)
static int screenshot_cmd(char *rest, Out *o) {
    while (rest && *rest == ' ') rest++;
    if (!rest || !*rest) {
        out_printf(o, "screenshot: hooks=%d state=%s taken=%u last: %s\n", hooks, shot_state == 1 ? "requested" : shot_state ? "writing" : "idle",
                   shot_n, shot_result[0] ? shot_result : "-");
        return 1;
    }
    if (!hooks) hooks = install_hooks() ? 1 : -1;
    if (hooks != 1) { out_printf(o, "error: no Present hook (see the log)\n"); return 1; }
    InterlockedCompareExchange(&shot_state, 0, 1);   // a request no Present picked up yet is replaced
    if (shot_state) { out_printf(o, "error: busy (%s)\n", shot_path); return 1; }
    snprintf(shot_path, sizeof shot_path, "%s", rest);
    shot_result[0] = 0;
    InterlockedExchange(&shot_state, 1);
    out_printf(o, "queued %s\n", shot_path);
    return 1;
}
extern "C" int overlay_cmd(const char *verb, char *rest, Out *o) {
    if (!strcmp(verb, "screenshot")) return screenshot_cmd(rest, o);
    if (strcmp(verb, "overlay")) return 0;
    char *a = rest ? strtok(rest, " ") : nullptr, *b = a ? strtok(nullptr, "") : nullptr;
    while (b && *b == ' ') b++;
    if (a && !strcmp(a, "open")) { if (!open_) try_open(); }
    else if (a && !strcmp(a, "close")) { if (open_) set_open(0); }
    else if (a && !strcmp(a, "tab") && b) {   // a tab's name or one of its aliases (overlay_add_alias)
        LOCKED;
        snprintf(select_tab, sizeof select_tab, "%s", b);
        int known = 0;
        for (int i = 0; i < n_panels; i++) known |= !_stricmp(panels[i].name, b);
        for (int i = 0; i < n_aliases && !known; i++)
            if (!_stricmp(aliases[i].name, b)) {
                snprintf(select_tab, sizeof select_tab, "%s", aliases[i].panel);
                if (aliases[i].select) aliases[i].select(aliases[i].arg);
                known = 1;
            }
        if (!known) { out_printf(o, "no tab '%s'\n", b); return 1; }
    }
    else if (a && !strcmp(a, "mouse") && b) {   // put the software cursor there (client pixels), for a real click
        LOCKED;
        float x, y;
        if (sscanf(b, "%f %f", &x, &y) == 2 && imgui_ready) { mouse_x = x; mouse_y = y; ImGui::GetIO().AddMousePosEvent(x, y); }
    } else if (a && !strcmp(a, "wheel") && b) {   // mouse wheel at the cursor (notches, + = up), e.g. scroll a tab
        LOCKED;
        if (imgui_ready) ImGui::GetIO().AddMouseWheelEvent(0, (float)atof(b));
    } else if (a && (!strcmp(a, "press") || !strcmp(a, "set") || !strcmp(a, "locate")) && b) {
        LOCKED;
        char lab[128] = "", *val = nullptr;
        if (*b == '"') { char *e = strchr(b + 1, '"'); snprintf(lab, sizeof lab, "%.*s", e ? (int)(e - b - 1) : (int)strlen(b + 1), b + 1); val = e ? e + 1 : nullptr; }
        else { char *sp = !strcmp(a, "set") ? strchr(b, ' ') : nullptr; snprintf(lab, sizeof lab, "%.*s", sp ? (int)(sp - b) : (int)strlen(b), b); val = sp; }
        while (val && *val == ' ') val++;
        snprintf(drive_label, sizeof drive_label, "%s", lab);
        snprintf(drive_value, sizeof drive_value, "%s", val ? val : "");
        drive_kind = !strcmp(a, "press") ? 1 : !strcmp(a, "set") ? 2 : 3; drive_frames = 0; drive_result[0] = 0;
        drive_t0 = GetTickCount64();
        out_printf(o, "queued %s '%s'%s%s\n", a, lab, val ? " = " : "", val ? val : "");
        return 1;
    } else if (a && !strcmp(a, "log")) {
        LOCKED;
        for (int i = 0; i < log_n; i++) out_printf(o, "%s\n", log_buf[(log_head + i) % LOG_LINES]);
        return 1;
    } else if (a && strcmp(a, "status")) { out_printf(o, "usage: overlay [open|close|status|log|tab <name>|press <label>|set <label> <value>]\n"); return 1; }
    LOCKED;
    out_printf(o, "overlay: enabled=%d open=%d hooks=%d renderer=%s frames built=%u drawn=%u tab=%s panels=%d key=%s scale=%.2f\n",
               enabled, (int)open_, hooks, render_failed ? "FAILED" : imgui_ready ? "ready" : "not yet", n_built, n_drawn,
               cur_tab[0] ? cur_tab : "-", n_panels, ov_key_name(key), ui_scale);
    out_printf(o, "last test: %s%s\n", drive_kind ? "pending " : "", drive_kind ? drive_label : drive_result[0] ? drive_result : "-");
    UObject *pc = ue_local_pc(), *pawn = pc ? (UObject *)ue_get_ptr(pc, "Pawn") : nullptr;   // input-leak checks: does the hero move?
    UFunction *f = pawn ? ue_find_function(U_CLASS(pawn), "K2_GetActorLocation") : nullptr;
    FField *rv = f ? ue_find_prop(f, "ReturnValue") : nullptr;
    if (rv && UFN_PARMSSIZE(f) <= 64) {
        uint8_t p[64] = {0};
        ue_process_event(pawn, f, p);
        const float *l = (const float *)(p + FP_OFFSET(rv));
        out_printf(o, "hero at %.0f %.0f %.0f; cursor %.0f %.0f of %.0fx%.0f\n", l[0], l[1], l[2], mouse_x, mouse_y, disp_w, disp_h);
    }
    return 1;
}
#endif
