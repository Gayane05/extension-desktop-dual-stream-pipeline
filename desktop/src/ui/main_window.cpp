// desktop/src/ui/main_window.cpp
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <tchar.h>

#include <cstdio>
#include <string>

#include "app/config.h"
#include "app/pipeline.h"
#include "app/transcript_model.h"
#include "stt/stt_engine.h"
#include "ui/save_transcript.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_usingWarp = false;

void createRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

bool createDevice(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    // Hardware first; WARP (software rasterizer) fallback keeps us running on
    // GPU-less machines (VMs, RDP).
    for (auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
    {
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2,
                                                    D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device,
                                                    &level, &g_context)))
        {
            g_usingWarp = (type == D3D_DRIVER_TYPE_WARP);
            createRenderTarget();
            return true;
        }
    }
    return false;
}

void destroyDevice()
{
    if (g_rtv)
    {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    if (g_swapChain)
    {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context)
    {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device)
    {
        g_device->Release();
        g_device = nullptr;
    }
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l))
    {
        return true;
    }
    switch (msg)
    {
        case WM_SIZE:
            if (g_device && w != SIZE_MINIMIZED)
            {
                if (g_rtv)
                {
                    g_rtv->Release();
                    g_rtv = nullptr;
                }
                g_swapChain->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
                createRenderTarget();
            }
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, w, l);
}
}  // namespace

namespace dsp {

int runUi(Pipeline& pipeline, TranscriptModel& model, ISttEngine& engine, const Config& cfg)
{
    WNDCLASSEXW wc = {sizeof(wc),
                      CS_CLASSDC,
                      wndProc,
                      0,
                      0,
                      ::GetModuleHandleW(nullptr),
                      nullptr,
                      nullptr,
                      nullptr,
                      nullptr,
                      L"DualStreamTranscriber",
                      nullptr};
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Dual-Stream Transcriber", WS_OVERLAPPEDWINDOW,
                                100, 100, 900, 640, nullptr, nullptr, wc.hInstance, nullptr);
    if (!createDevice(hwnd))
    {
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    const ImVec4 micColor(0.43f, 0.66f, 1.0f, 1.0f);  // blue - You
    const ImVec4 tabColor(1.0f, 0.72f, 0.42f, 1.0f);  // orange - Others
    const ImVec4 dimColor(0.6f, 0.6f, 0.6f, 1.0f);
    const ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
    bool autoscroll = true;
    std::string saveStatus;        // empty when nothing to report
    double saveStatusUntil = 0.0;  // ImGui::GetTime() deadline; cleared after
    bool done = false;
    // Status otherwise only pushes to the extension on hello/clientGone,
    // which leaves the popup showing stale (or initial "idle/idle") state
    // for the rest of a session. Push ~1x/second from the render loop too.
    double lastStatusPush = -1.0;
    while (!done)
    {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
            {
                done = true;
            }
        }
        if (done)
        {
            break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const double now = ImGui::GetTime();
        if (now - lastStatusPush >= 1.0)
        {
            pipeline.pushStatus();
            lastStatusPush = now;
        }

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

        // --- status bar (top) ---
        ImGui::Text("engine: %s (%s)%s | client: %s | port: %d", engine.name().c_str(),
                    engine.effectiveProvider().c_str(), g_usingWarp ? " | WARP render" : "",
                    pipeline.clientConnected() ? "connected" : "waiting...", cfg.port);
        ImGui::SameLine();
        ImGui::TextColored(dimColor, "| mic:%s tab:%s | dropped: %llu/%llu",
                           pipeline.streamState(StreamId::Mic).c_str(),
                           pipeline.streamState(StreamId::Tab).c_str(),
                           static_cast<unsigned long long>(pipeline.droppedChunks(StreamId::Mic)),
                           static_cast<unsigned long long>(pipeline.droppedChunks(StreamId::Tab)));
        if (ImGui::Button("Clear"))
        {
            model.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save transcript"))
        {
            std::string err;
            if (saveTranscriptFile("transcript.txt", model.toText(), err))
            {
                saveStatus = "saved transcript.txt";
            }
            else
            {
                saveStatus = "save failed: " + err;
            }
            saveStatusUntil = ImGui::GetTime() + 5.0;
        }
        if (!saveStatus.empty())
        {
            if (ImGui::GetTime() < saveStatusUntil)
            {
                ImGui::SameLine();
                const bool failed = saveStatus.rfind("save failed", 0) == 0;
                ImGui::TextColored(failed ? errColor : dimColor, "%s", saveStatus.c_str());
            }
            else
            {
                saveStatus.clear();
            }
        }
        ImGui::SameLine();
        ImGui::Checkbox("Autoscroll", &autoscroll);
        ImGui::Separator();

        // --- transcript ---
        ImGui::BeginChild("transcript", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& u : model.snapshot())
        {
            const bool mic = u.stream == StreamId::Mic;
            ImGui::TextColored(mic ? micColor : tabColor, mic ? "You:" : "Others:");
            ImGui::SameLine();
            if (u.isFinal)
            {
                ImGui::TextWrapped("%s", u.text.c_str());
            }
            else
            {
                // TextColored does not word-wrap; long sherpa interims (a whole
                // utterance until the endpoint fires) would overflow the window.
                ImGui::PushStyleColor(ImGuiCol_Text, dimColor);
                ImGui::TextWrapped("%s ...", u.text.c_str());
                ImGui::PopStyleColor();
            }
        }
        if (autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40)
        {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.08f, 0.08f, 0.10f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

}  // namespace dsp
