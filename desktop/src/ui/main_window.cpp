// desktop/src/ui/main_window.cpp
//
// Win32 + Direct3D 11 + Dear ImGui shell for the desktop app's window. Reads
// state produced elsewhere (Pipeline's connection/stream status,
// TranscriptModel's snapshot) and renders it every frame; the only thing it
// writes back is user intent (Clear/Save button clicks, the pushStatus()
// heartbeat). Entered once from main() via runUi() and run until the window
// is closed. Not built/used in --headless mode.
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <shobjidl.h>
#include <tchar.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/config.h"
#include "app/pipeline.h"
#include "app/transcript_model.h"
#include "stt/stt_engine.h"
#include "ui/save_transcript.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace
{
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_usingWarp = false;
// True when the Segoe MDL2 Assets icon glyphs merged into the font atlas
// (see applyThemeAndFont); the Settings button falls back to a text label
// on systems without that font.
bool g_hasIconFont = false;
// Segoe MDL2 Assets "Settings" gear glyph U+E713, UTF-8 encoded.
constexpr const char* kIconSettings = "\xEE\x9C\x93";

// Main window client-area size, in pixels.
constexpr int kMainWindowWidth = 900;
constexpr int kMainWindowHeight = 640;
// Setup/Settings chooser window client-area size, in pixels.
constexpr int kSetupWindowWidth = 720;
constexpr int kSetupWindowHeight = 590;
// Point size for the main UI text font.
constexpr float kUiFontSize = 20.0f;
// Point size for the merged Segoe MDL2 icon glyphs.
constexpr float kIconFontSize = 18.0f;
// How often (seconds) the render loop pushes a status heartbeat to the
// extension.
constexpr double kStatusPushIntervalSec = 1.0;
// How long (seconds) the Save transcript status line stays visible.
constexpr double kSaveStatusDisplaySec = 5.0;
// Buffer size for the Deepgram API key input field.
constexpr size_t kApiKeyBufferSize = 256;

// D3D lifecycle: createDevice() (device+swapchain+RTV) pairs with
// destroyDevice() (called once at shutdown), while createRenderTarget()
// alone pairs with releasing just g_rtv on WM_SIZE (wndProc below) --
// resizing needs a fresh render target view but not a fresh device/swapchain.
void createRenderTarget()
{
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

bool createDevice(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC swapChainDesc{};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    // Hardware first; WARP (software rasterizer) fallback keeps us running on
    // GPU-less machines (VMs, RDP).
    for (auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
    {
        if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2,
                                                    D3D11_SDK_VERSION, &swapChainDesc, &g_swapChain,
                                                    &g_device, &level, &g_context)))
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

// Shows the standard Windows Save As dialog (IFileSaveDialog) modally over
// `owner` and returns the chosen path as UTF-8, or an empty string when the
// user cancels (or the dialog cannot be created). Runs its own message loop
// while open, so the ImGui frame underneath simply pauses -- transcription
// threads keep running.
std::string showSaveTranscriptDialog(HWND owner)
{
    std::wstring chosenPath;
    // S_OK or S_FALSE (already initialized) both require the matching
    // CoUninitialize; RPC_E_CHANGED_MODE (initialized with a different
    // threading model elsewhere) must not be paired with one.
    const HRESULT comInit =
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileSaveDialog* dialog = nullptr;
    if (SUCCEEDED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&dialog))))
    {
        const COMDLG_FILTERSPEC fileTypes[] = {{L"Text file (*.txt)", L"*.txt"},
                                               {L"All files (*.*)", L"*.*"}};
        dialog->SetFileTypes(2, fileTypes);
        dialog->SetDefaultExtension(L"txt");
        dialog->SetFileName(L"transcript.txt");
        dialog->SetTitle(L"Save transcript");
        // Show() returns ERROR_CANCELLED as an HRESULT when the user backs
        // out; that path simply leaves chosenPath empty.
        if (SUCCEEDED(dialog->Show(owner)))
        {
            IShellItem* resultItem = nullptr;
            if (SUCCEEDED(dialog->GetResult(&resultItem)))
            {
                PWSTR filesystemPath = nullptr;
                if (SUCCEEDED(resultItem->GetDisplayName(SIGDN_FILESYSPATH, &filesystemPath)))
                {
                    chosenPath = filesystemPath;
                    ::CoTaskMemFree(filesystemPath);
                }
                resultItem->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(comInit))
    {
        ::CoUninitialize();
    }
    if (chosenPath.empty())
    {
        return "";
    }
    const int utf8Len =
        ::WideCharToMultiByte(CP_UTF8, 0, chosenPath.c_str(), static_cast<int>(chosenPath.size()),
                              nullptr, 0, nullptr, nullptr);
    std::string utf8Path(utf8Len > 0 ? static_cast<size_t>(utf8Len) : 0, '\0');
    if (utf8Len > 0)
    {
        ::WideCharToMultiByte(CP_UTF8, 0, chosenPath.c_str(), static_cast<int>(chosenPath.size()),
                              utf8Path.data(), utf8Len, nullptr, nullptr);
    }
    return utf8Path;
}

// DestroyWindow triggers WM_DESTROY -> PostQuitMessage, and that WM_QUIT sits
// in the THREAD's message queue -- not the window's. Left there, it instantly
// terminates the next window's message loop (setup chooser -> main window
// handoff, and main -> chooser via the Settings button). Drain leftover
// messages after tearing a window down so each window starts with a clean
// queue.
void drainThreadMessages()
{
    MSG queuedMsg;
    while (::PeekMessageW(&queuedMsg, nullptr, 0, 0, PM_REMOVE))
    {
    }
}

// Shared by the main window and the setup chooser (runSetupUi) so both carry
// the same dark-slate/indigo theme and Segoe UI font. Must run after
// ImGui::CreateContext() and before the first frame.
void applyThemeAndFont()
{
    ImGui::StyleColorsLight();
    // Larger click targets: FramePadding sizes buttons/checkboxes (default is
    // a cramped 4x3); a touch more rounding keeps them looking intentional.
    ImGuiStyle& style = ImGui::GetStyle();
    style.FramePadding = ImVec2(15.0f, 7.0f);
    style.FrameRounding = 5.0f;
    style.ChildRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.ScrollbarRounding = 5.0f;
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    // Outlined "chip" buttons and framed panels are part of the paper look.
    style.FrameBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    // "Paper transcript" theme chosen by the user: a warm paper field
    // (#f5f2ea) with a brighter page panel (#fdfbf6), near-black ink text,
    // quiet outlined chip buttons, and two ink colors for the voices --
    // court-reporter energy. Transcript lane colors live in runUi():
    // You = blue ink #2b4a8c, Others = red ink #a8402c.
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.961f, 0.949f, 0.918f, 1.00f);  // Paper #f5f2ea.
    colors[ImGuiCol_ChildBg] = ImVec4(0.992f, 0.984f, 0.965f, 1.00f);   // Page #fdfbf6.
    colors[ImGuiCol_Text] = ImVec4(0.165f, 0.153f, 0.133f, 1.00f);      // Ink #2a2722.
    colors[ImGuiCol_Border] = ImVec4(0.788f, 0.761f, 0.698f, 1.00f);    // Chip edge #c9c2b2.
    colors[ImGuiCol_FrameBg] = ImVec4(0.918f, 0.898f, 0.847f, 1.00f);   // Field #eae5d8.
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.878f, 0.851f, 0.784f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.839f, 0.804f, 0.722f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.992f, 0.984f, 0.965f, 1.00f);  // Quiet chip.
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.941f, 0.922f, 0.867f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.890f, 0.863f, 0.788f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.169f, 0.290f, 0.549f, 1.00f);  // Blue ink #2b4a8c.
    colors[ImGuiCol_Separator] = ImVec4(0.867f, 0.839f, 0.776f, 1.00f);  // Rule #ddd6c6.
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.961f, 0.949f, 0.918f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.788f, 0.761f, 0.698f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.702f, 0.671f, 0.592f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.612f, 0.576f, 0.490f, 1.00f);
    // Replace ImGui's 13px bitmap default with a larger, softer system font.
    // Segoe UI Variable (Win11) first, classic Segoe UI as fallback; if
    // neither loads (non-standard Windows install), scale the bitmap default
    // rather than staying tiny.
    ImGuiIO& io = ImGui::GetIO();
    const char* windir = std::getenv("WINDIR");
    const std::string fontBase = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    ImFont* uiFont = nullptr;
    for (const char* fontFile : {"SegUIVar.ttf", "segoeui.ttf"})
    {
        const std::string path = fontBase + fontFile;
        uiFont = io.Fonts->AddFontFromFileTTF(path.c_str(), kUiFontSize);
        if (uiFont)
        {
            break;
        }
    }
    if (!uiFont)
    {
        io.FontGlobalScale = 1.5f;
    }
    // Merge the Windows-native icon font (Segoe MDL2 Assets) into the same
    // atlas so buttons can use standard system glyphs (e.g. the Settings
    // gear). Only the small private-use range we need is loaded. If the font
    // is missing, callers fall back to text labels via g_hasIconFont.
    g_hasIconFont = false;
    if (uiFont)
    {
        static const ImWchar iconRange[] = {0xE700, 0xE7FF, 0};
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.GlyphOffset = ImVec2(0.0f, 2.0f);  // Optical alignment with text baseline.
        const std::string mdl2 = fontBase + "segmdl2.ttf";
        g_hasIconFont =
            io.Fonts->AddFontFromFileTTF(mdl2.c_str(), kIconFontSize, &merge, iconRange) != nullptr;
    }
}

LRESULT WINAPI wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return true;
    }
    switch (msg)
    {
        case WM_SIZE:
            if (g_device && wParam != SIZE_MINIMIZED)
            {
                if (g_rtv)
                {
                    g_rtv->Release();
                    g_rtv = nullptr;
                }
                g_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN,
                                           0);
                createRenderTarget();
            }
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
}  // namespace

namespace dsp
{

int runUi(Pipeline& pipeline, TranscriptModel& model, ISttEngine& engine, const Config& cfg)
{
    WNDCLASSEXW windowClass = {sizeof(windowClass),         CS_CLASSDC, wndProc, 0,       0,
                               ::GetModuleHandleW(nullptr), nullptr,    nullptr, nullptr, nullptr,
                               L"DualStreamTranscriber",    nullptr};
    ::RegisterClassExW(&windowClass);
    HWND hwnd = ::CreateWindowW(windowClass.lpszClassName, L"Dual-Stream Transcriber",
                                WS_OVERLAPPEDWINDOW, 100, 100, kMainWindowWidth, kMainWindowHeight,
                                nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!createDevice(hwnd))
    {
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyThemeAndFont();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // The two voices are the two inks of the paper theme: You writes in
    // blue ink, Others in red ink, both on the bright page panel.
    const ImVec4 micColor(0.169f, 0.290f, 0.549f, 1.0f);  // Blue ink #2b4a8c - You.
    const ImVec4 tabColor(0.659f, 0.251f, 0.173f, 1.0f);  // Red ink #a8402c - Others.
    const ImVec4 dimColor(0.541f, 0.518f, 0.467f, 1.0f);  // Faded ink #8a8477.
    const ImVec4 errColor(0.761f, 0.169f, 0.169f, 1.0f);  // Alarm red, legible on paper.
    bool autoscroll = true;
    std::string saveStatus;        // Empty when nothing to report.
    double saveStatusUntil = 0.0;  // ImGui::GetTime() deadline; cleared after.
    bool done = false;
    int exitCode = 0;  // kRunUiRestartSetup when the Settings button was used.
    // Status otherwise only pushes to the extension on hello/clientGone,
    // which leaves the popup showing stale (or initial "idle/idle") state
    // for the rest of a session. Push ~1x/second from the render loop too.
    double lastStatusPush = -1.0;
    // Per-frame structure: pump the Win32 message queue (so the window stays
    // responsive/resizable), start a new ImGui frame, push a status
    // heartbeat at most 1x/second, lay out the widgets against
    // Pipeline/TranscriptModel state, then render. Runs until WM_QUIT.
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
        if (now - lastStatusPush >= kStatusPushIntervalSec)
        {
            pipeline.pushStatus();
            lastStatusPush = now;
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
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
            const std::string savePath = showSaveTranscriptDialog(hwnd);
            if (!savePath.empty())
            {
                // Snapshot the transcript AFTER the dialog closes so lines
                // transcribed while it was open still make the file.
                std::string err;
                if (saveTranscriptFile(savePath, model.toText(), err))
                {
                    saveStatus = "saved " + savePath;
                }
                else
                {
                    saveStatus = "save failed: " + err;
                }
                saveStatusUntil = ImGui::GetTime() + kSaveStatusDisplaySec;
            }
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
        ImGui::SameLine();
        // Reopens the mode chooser: this window closes, main() rebuilds the
        // engine with whatever the user picks there. Gear icon when the
        // system icon font is available, text label otherwise.
        if (ImGui::Button(g_hasIconFont ? kIconSettings : "Settings"))
        {
            exitCode = kRunUiRestartSetup;
            done = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Settings");
        }
        ImGui::Separator();

        // --- transcript ---
        ImGui::BeginChild("transcript", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& utterance : model.snapshot())
        {
            const bool mic = utterance.stream == StreamId::Mic;
            ImGui::TextColored(mic ? micColor : tabColor, mic ? "You:" : "Others:");
            ImGui::SameLine();
            if (utterance.isFinal)
            {
                ImGui::TextWrapped("%s", utterance.text.c_str());
            }
            else
            {
                // TextColored does not word-wrap; long sherpa interims (a whole
                // utterance until the endpoint fires) would overflow the window.
                ImGui::PushStyleColor(ImGuiCol_Text, dimColor);
                ImGui::TextWrapped("%s ...", utterance.text.c_str());
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
        const float clear[4] = {0.961f, 0.949f, 0.918f, 1.0f};  // Match WindowBg paper.
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
    ::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    drainThreadMessages();
    return exitCode;
}

// First-run / Settings mode chooser. Runs its own small window with the same
// theme, BEFORE any engine exists (so it can also be re-shown between engine
// restarts). Writes the choice into cfg.engine/cfg.provider and returns true;
// returns false when the user closes the window without choosing (main()
// treats that as "exit the app"). The caller persists the choice.
bool runSetupUi(Config& cfg)
{
    WNDCLASSEXW windowClass = {sizeof(windowClass),           CS_CLASSDC, wndProc, 0,       0,
                               ::GetModuleHandleW(nullptr),   nullptr,    nullptr, nullptr, nullptr,
                               L"DualStreamTranscriberSetup", nullptr};
    ::RegisterClassExW(&windowClass);
    HWND hwnd = ::CreateWindowW(
        windowClass.lpszClassName, L"Transcriber Setup", WS_OVERLAPPEDWINDOW, 200, 200,
        kSetupWindowWidth, kSetupWindowHeight, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (!createDevice(hwnd))
    {
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
        return false;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyThemeAndFont();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    const ImVec4 dimColor(0.541f, 0.518f, 0.467f, 1.0f);   // Faded ink #8a8477.
    const ImVec4 warnColor(0.671f, 0.404f, 0.129f, 1.0f);  // Amber ink, legible on paper.
    // Captions must wrap at the window edge (TextColored clips instead), so
    // every note below goes through this helper.
    const auto colorWrapped = [](const ImVec4& color, const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };
    // API key entry for Deepgram. Pre-filled from a previously saved key so
    // the field doubles as "view/replace" on later Settings visits. Kept
    // masked by default; the checkbox reveals it for verifying a paste.
    char keyBuf[kApiKeyBufferSize] = {};
    if (!cfg.deepgramKey.empty())
    {
        std::snprintf(keyBuf, sizeof(keyBuf), "%s", cfg.deepgramKey.c_str());
    }
    bool showKey = false;
    const bool envKeyPresent = std::getenv("DEEPGRAM_API_KEY") != nullptr;
    bool chosen = false;
    bool done = false;
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

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

        ImGui::Text("How should speech-to-text run?");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Local (sherpa-onnx)  -  GPU / CUDA", ImVec2(-1.0f, 0.0f)))
        {
            cfg.engine = "sherpa";
            cfg.provider = "cuda";
            chosen = true;
            done = true;
        }
        colorWrapped(dimColor,
                     "On-device transcription on your NVIDIA GPU. Falls back to CPU "
                     "automatically if CUDA is unavailable.");
        ImGui::Spacing();

        if (ImGui::Button("Local (sherpa-onnx)  -  CPU", ImVec2(-1.0f, 0.0f)))
        {
            cfg.engine = "sherpa";
            cfg.provider = "cpu";
            chosen = true;
            done = true;
        }
        colorWrapped(dimColor, "On-device transcription on the CPU. Works everywhere.");
        ImGui::Spacing();

        if (ImGui::Button("Local (Parakeet)  -  highest accuracy, live", ImVec2(-1.0f, 0.0f)))
        {
            cfg.engine = "parakeet";
            chosen = true;
            done = true;
        }
        colorWrapped(dimColor,
                     "NVIDIA Parakeet 0.6B on-device. Best local accuracy; the interim "
                     "line refreshes every ~1.2 s while you speak and the final replaces "
                     "it. Needs the Parakeet model downloaded (see README).");
        ImGui::Spacing();

        if (ImGui::Button("Deepgram  -  cloud", ImVec2(-1.0f, 0.0f)))
        {
            cfg.engine = "deepgram";
            chosen = true;
            done = true;
        }
        colorWrapped(dimColor,
                     "Streams audio to Deepgram's API. Best accuracy, punctuated "
                     "output; requires an API key (free at console.deepgram.com).");
        ImGui::SetNextItemWidth(-130.0f);
        ImGui::InputText("##dgkey", keyBuf, sizeof(keyBuf),
                         showKey ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        ImGui::Checkbox("show", &showKey);
        if (keyBuf[0] == '\0')
        {
            if (envKeyPresent)
            {
                colorWrapped(dimColor,
                             "Using the DEEPGRAM_API_KEY environment variable; paste a "
                             "key above only to override it.");
            }
            else
            {
                colorWrapped(warnColor,
                             "No API key found -- paste one above before choosing "
                             "Deepgram (or set DEEPGRAM_API_KEY).");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Checkbox("Ask every startup", &cfg.askOnStartup);
        ImGui::SameLine();
        colorWrapped(dimColor, cfg.askOnStartup ? "This page will open at every launch."
                                                : "This page opens only from the Settings button.");

        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.961f, 0.949f, 0.918f, 1.0f};  // Match WindowBg paper.
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    // Persist the key with whatever mode was chosen (typing a key and picking
    // a local engine still saves it for a later switch). Trim whitespace --
    // a trailing newline from a clipboard paste would corrupt the HTTP
    // Authorization header.
    if (chosen)
    {
        std::string key(keyBuf);
        const auto first = key.find_first_not_of(" \t\r\n");
        const auto last = key.find_last_not_of(" \t\r\n");
        cfg.deepgramKey = (first == std::string::npos) ? "" : key.substr(first, last - first + 1);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroyDevice();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    drainThreadMessages();
    return chosen;
}

}  // namespace dsp
