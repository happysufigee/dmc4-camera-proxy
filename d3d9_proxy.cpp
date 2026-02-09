/**
 * DMC4 Camera Proxy for D3D9
 *
 * This proxy DLL intercepts D3D9 calls, extracts camera matrices from
 * vertex shader constants, and provides them via SetTransform().
 *
 * Build with Visual Studio Developer Command Prompt:
 *   cl /LD /EHsc d3d9_proxy.cpp /link /DEF:d3d9.def /OUT:d3d9.dll
 *
 * Setup:
 * 1. Place this compiled d3d9.dll in the game folder
 * 2. Configure matrix extraction registers in camera_proxy.ini
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include <vector>

#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx9.h"
#include "imgui/backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

bool LooksLikeMatrix(const float* data);

#pragma comment(lib, "user32.lib")

// Configuration
struct ProxyConfig {
    int viewMatrixRegister = 4;
    int projMatrixRegister = 8;
    int worldMatrixRegister = 0;
    bool enableLogging = true;
    float minFOV = 0.1f;
    float maxFOV = 2.5f;

    // Diagnostic mode - log ALL shader constant updates
    bool logAllConstants = false;
    bool autoDetectMatrices = false;
};

static ProxyConfig g_config;
static HMODULE g_hD3D9 = nullptr;
static FILE* g_logFile = nullptr;
static int g_frameCount = 0;

struct CameraMatrices {
    D3DMATRIX view;
    D3DMATRIX projection;
    D3DMATRIX world;
    D3DMATRIX mvp;
    bool hasView;
    bool hasProjection;
    bool hasWorld;
    bool hasMVP;
};

static CameraMatrices g_cameraMatrices = {};
static bool g_imguiInitialized = false;
static HWND g_imguiHwnd = nullptr;
static bool g_showImGui = false;
static bool g_toggleWasDown = false;
static bool g_pauseRendering = false;
static bool g_pauseToggleWasDown = false;
static WNDPROC g_imguiPrevWndProc = nullptr;
static bool g_showConstantsView = false;
static bool g_showConstantsAsMatrices = true;
static bool g_filterDetectedMatrices = false;
static bool g_showFpsStats = true;
static bool g_showTransposedMatrices = false;

static constexpr int kMaxConstantRegisters = 256;
static int g_selectedRegister = -1;
static uintptr_t g_activeShaderKey = 0;
static uintptr_t g_selectedShaderKey = 0;

struct ShaderConstantState {
    float constants[kMaxConstantRegisters][4] = {};
    bool valid[kMaxConstantRegisters] = {};
    bool snapshotReady = false;
};

static std::unordered_map<uintptr_t, ShaderConstantState> g_shaderConstants = {};
static std::vector<uintptr_t> g_shaderOrder = {};

static constexpr int kFrameTimeHistory = 120;
static float g_frameTimeHistory[kFrameTimeHistory] = {};
static int g_frameTimeIndex = 0;
static int g_frameTimeCount = 0;
static float g_frameTimeMin = 0.0f;
static float g_frameTimeMax = 0.0f;
static double g_frameTimeSum = 0.0;
static unsigned long long g_frameTimeSamples = 0;
static LARGE_INTEGER g_perfFrequency = {};
static LARGE_INTEGER g_prevCounter = {};
static bool g_perfInitialized = false;

static LRESULT CALLBACK ImGuiWndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_imguiInitialized) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    }
    if (g_imguiPrevWndProc) {
        return CallWindowProc(g_imguiPrevWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void StoreViewMatrix(const D3DMATRIX& view) {
    g_cameraMatrices.view = view;
    g_cameraMatrices.hasView = true;
}

static void StoreProjectionMatrix(const D3DMATRIX& projection) {
    g_cameraMatrices.projection = projection;
    g_cameraMatrices.hasProjection = true;
}

static void StoreWorldMatrix(const D3DMATRIX& world) {
    g_cameraMatrices.world = world;
    g_cameraMatrices.hasWorld = true;
}

static void StoreMVPMatrix(const D3DMATRIX& mvp) {
    g_cameraMatrices.mvp = mvp;
    g_cameraMatrices.hasMVP = true;
}

static HMODULE LoadSystemD3D9() {
    char systemDir[MAX_PATH] = {};
    if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0) {
        return LoadLibraryA("d3d9.dll");
    }

    char path[MAX_PATH] = {};
    snprintf(path, MAX_PATH, "%s\\d3d9.dll", systemDir);
    HMODULE module = LoadLibraryA(path);
    if (!module) {
        module = LoadLibraryA("d3d9.dll");
    }
    return module;
}

static void InitializeImGui(IDirect3DDevice9* device, HWND hwnd) {
    if (g_imguiInitialized || !device || !hwnd) {
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(device);
    g_imguiInitialized = true;
    g_imguiHwnd = hwnd;
    g_imguiPrevWndProc =
        reinterpret_cast<WNDPROC>(SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                                                   reinterpret_cast<LONG_PTR>(ImGuiWndProcHook)));
}

static void ShutdownImGui() {
    if (!g_imguiInitialized) {
        return;
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imguiInitialized = false;
    if (g_imguiPrevWndProc && g_imguiHwnd) {
        SetWindowLongPtr(g_imguiHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_imguiPrevWndProc));
    }
    g_imguiPrevWndProc = nullptr;
    g_imguiHwnd = nullptr;
}

static void DrawMatrix(const char* label, const D3DMATRIX& mat, bool available) {
    if (!available) {
        ImGui::Text("%s: <unavailable>", label);
        return;
    }

    ImGui::Text("%s:", label);
    ImGui::Text("[%.3f %.3f %.3f %.3f]", mat._11, mat._12, mat._13, mat._14);
    ImGui::Text("[%.3f %.3f %.3f %.3f]", mat._21, mat._22, mat._23, mat._24);
    ImGui::Text("[%.3f %.3f %.3f %.3f]", mat._31, mat._32, mat._33, mat._34);
    ImGui::Text("[%.3f %.3f %.3f %.3f]", mat._41, mat._42, mat._43, mat._44);
}

static D3DMATRIX TransposeMatrix(const D3DMATRIX& mat) {
    D3DMATRIX out = {};
    out._11 = mat._11; out._12 = mat._21; out._13 = mat._31; out._14 = mat._41;
    out._21 = mat._12; out._22 = mat._22; out._23 = mat._32; out._24 = mat._42;
    out._31 = mat._13; out._32 = mat._23; out._33 = mat._33; out._34 = mat._43;
    out._41 = mat._14; out._42 = mat._24; out._43 = mat._34; out._44 = mat._44;
    return out;
}

static void DrawMatrixWithTranspose(const char* label, const D3DMATRIX& mat, bool available,
                                    bool transpose) {
    if (!transpose) {
        DrawMatrix(label, mat, available);
        return;
    }
    D3DMATRIX transposed = TransposeMatrix(mat);
    DrawMatrix(label, transposed, available);
}

static ShaderConstantState* GetShaderState(uintptr_t shaderKey, bool createIfMissing) {
    if (shaderKey == 0 && !createIfMissing) {
        return nullptr;
    }
    auto it = g_shaderConstants.find(shaderKey);
    if (it != g_shaderConstants.end()) {
        return &it->second;
    }
    if (!createIfMissing) {
        return nullptr;
    }
    g_shaderOrder.push_back(shaderKey);
    auto inserted = g_shaderConstants.emplace(shaderKey, ShaderConstantState{});
    return &inserted.first->second;
}

static bool TryBuildMatrixFromSnapshot(const ShaderConstantState& state, int baseRegister,
                                       D3DMATRIX* outMatrix) {
    if (!state.snapshotReady || baseRegister < 0 ||
        baseRegister + 3 >= kMaxConstantRegisters) {
        return false;
    }

    for (int i = 0; i < 4; i++) {
        if (!state.valid[baseRegister + i]) {
            return false;
        }
    }

    memset(outMatrix, 0, sizeof(D3DMATRIX));
    outMatrix->_11 = state.constants[baseRegister + 0][0];
    outMatrix->_12 = state.constants[baseRegister + 0][1];
    outMatrix->_13 = state.constants[baseRegister + 0][2];
    outMatrix->_14 = state.constants[baseRegister + 0][3];

    outMatrix->_21 = state.constants[baseRegister + 1][0];
    outMatrix->_22 = state.constants[baseRegister + 1][1];
    outMatrix->_23 = state.constants[baseRegister + 1][2];
    outMatrix->_24 = state.constants[baseRegister + 1][3];

    outMatrix->_31 = state.constants[baseRegister + 2][0];
    outMatrix->_32 = state.constants[baseRegister + 2][1];
    outMatrix->_33 = state.constants[baseRegister + 2][2];
    outMatrix->_34 = state.constants[baseRegister + 2][3];

    outMatrix->_41 = state.constants[baseRegister + 3][0];
    outMatrix->_42 = state.constants[baseRegister + 3][1];
    outMatrix->_43 = state.constants[baseRegister + 3][2];
    outMatrix->_44 = state.constants[baseRegister + 3][3];
    return true;
}

static bool TryBuildMatrixSnapshotInfo(const ShaderConstantState& state, int baseRegister,
                                       D3DMATRIX* outMatrix, bool* looksLike) {
    if (!TryBuildMatrixFromSnapshot(state, baseRegister, outMatrix)) {
        if (looksLike) {
            *looksLike = false;
        }
        return false;
    }
    if (looksLike) {
        *looksLike = LooksLikeMatrix(reinterpret_cast<const float*>(outMatrix));
    }
    return true;
}

static void UpdateConstantSnapshot() {
    for (auto& entry : g_shaderConstants) {
        entry.second.snapshotReady = true;
    }
}

static void UpdateImGuiToggle() {
    bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool mDown = (GetAsyncKeyState('M') & 0x8000) != 0;
    bool togglePressed = altDown && mDown;
    if (togglePressed && !g_toggleWasDown) {
        g_showImGui = !g_showImGui;
    }
    g_toggleWasDown = togglePressed;
}

static void UpdatePauseToggle() {
    bool pauseDown = (GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
    if (pauseDown && !g_pauseToggleWasDown) {
        g_pauseRendering = !g_pauseRendering;
    }
    g_pauseToggleWasDown = pauseDown;
}

static void UpdateFrameTimeStats() {
    if (!g_perfInitialized) {
        QueryPerformanceFrequency(&g_perfFrequency);
        QueryPerformanceCounter(&g_prevCounter);
        g_perfInitialized = true;
        return;
    }

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    double delta = static_cast<double>(now.QuadPart - g_prevCounter.QuadPart) /
                   static_cast<double>(g_perfFrequency.QuadPart);
    g_prevCounter = now;

    float ms = static_cast<float>(delta * 1000.0);
    g_frameTimeHistory[g_frameTimeIndex] = ms;
    g_frameTimeIndex = (g_frameTimeIndex + 1) % kFrameTimeHistory;
    if (g_frameTimeCount < kFrameTimeHistory) {
        g_frameTimeCount++;
    }

    if (g_frameTimeSamples == 0) {
        g_frameTimeMin = ms;
        g_frameTimeMax = ms;
    } else {
        if (ms < g_frameTimeMin) g_frameTimeMin = ms;
        if (ms > g_frameTimeMax) g_frameTimeMax = ms;
    }
    g_frameTimeSum += ms;
    g_frameTimeSamples++;
}

static void RenderImGuiOverlay() {
    if (!g_imguiInitialized || !g_showImGui) {
        return;
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("Camera Matrices", nullptr,
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Toggle menu: Alt+M | Pause rendering: Pause");
    ImGui::Checkbox("Show FPS stats", &g_showFpsStats);
    ImGui::Checkbox("Show transposed matrices", &g_showTransposedMatrices);
    if (g_showFpsStats && g_frameTimeSamples > 0) {
        float minMs = g_frameTimeHistory[0];
        float maxMs = g_frameTimeHistory[0];
        double sumMs = 0.0;
        for (int i = 0; i < g_frameTimeCount; i++) {
            float ms = g_frameTimeHistory[i];
            if (ms < minMs) minMs = ms;
            if (ms > maxMs) maxMs = ms;
            sumMs += ms;
        }
        float avgMs = g_frameTimeCount > 0 ? static_cast<float>(sumMs / g_frameTimeCount) : 0.0f;
        float avgFps = avgMs > 0.0f ? 1000.0f / avgMs : 0.0f;
        float minFps = maxMs > 0.0f ? 1000.0f / maxMs : 0.0f;
        float maxFps = minMs > 0.0f ? 1000.0f / minMs : 0.0f;
        ImGui::Text("FPS avg/min/max: %.1f / %.1f / %.1f", avgFps, minFps, maxFps);
        ImGui::Text("Frame time ms avg/min/max: %.2f / %.2f / %.2f",
                    avgMs, minMs, maxMs);
        ImGui::PlotLines("Frame time (ms)", g_frameTimeHistory, g_frameTimeCount, g_frameTimeIndex,
                         nullptr, 0.0f, maxMs > 0.0f ? maxMs : 33.0f,
                         ImVec2(0, 80));
    }

    ImGui::Separator();
    ImGui::Checkbox("Show all constant registers", &g_showConstantsView);
    ImGui::Separator();

    if (!g_showConstantsView) {
        if (ImGui::CollapsingHeader("Camera Matrices", ImGuiTreeNodeFlags_DefaultOpen)) {
            DrawMatrixWithTranspose("World", g_cameraMatrices.world, g_cameraMatrices.hasWorld,
                                    g_showTransposedMatrices);
            ImGui::Separator();
            DrawMatrixWithTranspose("View", g_cameraMatrices.view, g_cameraMatrices.hasView,
                                    g_showTransposedMatrices);
            ImGui::Separator();
            DrawMatrixWithTranspose("Projection", g_cameraMatrices.projection,
                                    g_cameraMatrices.hasProjection, g_showTransposedMatrices);
            ImGui::Separator();
            DrawMatrixWithTranspose("MVP (c0-c3)", g_cameraMatrices.mvp, g_cameraMatrices.hasMVP,
                                    g_showTransposedMatrices);
        }
    } else {
        ImGui::Text("Per-shader snapshots update every frame.");
        if (g_selectedShaderKey == 0) {
            if (g_activeShaderKey != 0) {
                g_selectedShaderKey = g_activeShaderKey;
            } else if (!g_shaderOrder.empty()) {
                g_selectedShaderKey = g_shaderOrder.front();
            }
        }
        if (!g_shaderOrder.empty()) {
            char preview[64];
            snprintf(preview, sizeof(preview), "0x%p%s", reinterpret_cast<void*>(g_selectedShaderKey),
                     g_selectedShaderKey == g_activeShaderKey ? " (active)" : "");
            if (ImGui::BeginCombo("Shader", preview)) {
                for (uintptr_t key : g_shaderOrder) {
                    char itemLabel[64];
                    snprintf(itemLabel, sizeof(itemLabel), "0x%p%s", reinterpret_cast<void*>(key),
                             key == g_activeShaderKey ? " (active)" : "");
                    bool selected = (key == g_selectedShaderKey);
                    if (ImGui::Selectable(itemLabel, selected)) {
                        g_selectedShaderKey = key;
                        g_selectedRegister = -1;
                    }
                    if (selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::Text("<no shader constants captured yet>");
        }
        ImGui::Checkbox("Group by 4-register matrices", &g_showConstantsAsMatrices);
        ImGui::SameLine();
        ImGui::Checkbox("Only show detected matrices", &g_filterDetectedMatrices);
        if (g_selectedRegister >= 0) {
            ImGui::Text("Selected register: c%d", g_selectedRegister);
        }
        ImGui::BeginChild("ConstantsScroll", ImVec2(0, 340), true);
        ShaderConstantState* state = GetShaderState(g_selectedShaderKey, false);
        if (state && state->snapshotReady) {
            if (g_showConstantsAsMatrices) {
                for (int base = 0; base < kMaxConstantRegisters; base += 4) {
                    D3DMATRIX mat = {};
                    bool looksLike = false;
                    bool hasMatrix = TryBuildMatrixSnapshotInfo(*state, base, &mat, &looksLike);

                    if (g_filterDetectedMatrices) {
                        if (!hasMatrix || !looksLike) {
                            continue;
                        }
                    } else {
                        bool anyValid = false;
                        for (int reg = base; reg < base + 4; reg++) {
                            if (state->valid[reg]) {
                                anyValid = true;
                                break;
                            }
                        }
                        if (!anyValid) {
                            continue;
                        }
                    }

                    char label[64];
                    snprintf(label, sizeof(label), "c%d-c%d%s", base, base + 3,
                             looksLike ? " (matrix)" : "");
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                               ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (g_selectedRegister == base) {
                        flags |= ImGuiTreeNodeFlags_Selected;
                    }
                    bool open = ImGui::TreeNodeEx(label, flags);
                    if (ImGui::IsItemClicked()) {
                        g_selectedRegister = base;
                    }
                    if (open) {
                        D3DMATRIX displayMat = mat;
                        if (g_showTransposedMatrices && hasMatrix) {
                            displayMat = TransposeMatrix(mat);
                        }
                        for (int reg = base; reg < base + 4; reg++) {
                            char rowLabel[128];
                            if (state->valid[reg]) {
                                if (g_showTransposedMatrices && hasMatrix) {
                                    int row = reg - base;
                                    const float* data = reinterpret_cast<const float*>(&displayMat) + row * 4;
                                    snprintf(rowLabel, sizeof(rowLabel), "r%d: [%.3f %.3f %.3f %.3f]",
                                             row, data[0], data[1], data[2], data[3]);
                                } else {
                                    const float* data = state->constants[reg];
                                    snprintf(rowLabel, sizeof(rowLabel), "c%d: [%.3f %.3f %.3f %.3f]",
                                             reg, data[0], data[1], data[2], data[3]);
                                }
                            } else {
                                snprintf(rowLabel, sizeof(rowLabel), "c%d: <unset>", reg);
                            }
                            bool selected = (g_selectedRegister == reg);
                            if (ImGui::Selectable(rowLabel, selected)) {
                                g_selectedRegister = reg;
                            }
                        }
                        ImGui::TreePop();
                    }
                }
            } else {
                for (int reg = 0; reg < kMaxConstantRegisters; reg++) {
                    if (!state->valid[reg]) {
                        continue;
                    }
                    const float* data = state->constants[reg];
                    char rowLabel[128];
                    snprintf(rowLabel, sizeof(rowLabel), "c%d: [%.3f %.3f %.3f %.3f]",
                             reg, data[0], data[1], data[2], data[3]);
                    if (ImGui::Selectable(rowLabel, g_selectedRegister == reg)) {
                        g_selectedRegister = reg;
                    }
                }
            }
        } else {
            ImGui::Text("<no constants captured yet>");
        }
        ImGui::EndChild();
    }

    ImGui::End();

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

// Function pointer types
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT (WINAPI* Direct3DCreate9Ex_t)(UINT SDKVersion, IDirect3D9Ex**);
typedef int (WINAPI* D3DPERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int (WINAPI* D3DPERF_EndEvent_t)(void);
typedef DWORD (WINAPI* D3DPERF_GetStatus_t)(void);
typedef BOOL (WINAPI* D3DPERF_QueryRepeatFrame_t)(void);
typedef void (WINAPI* D3DPERF_SetMarker_t)(D3DCOLOR, LPCWSTR);
typedef void (WINAPI* D3DPERF_SetOptions_t)(DWORD);
typedef void (WINAPI* D3DPERF_SetRegion_t)(D3DCOLOR, LPCWSTR);

static Direct3DCreate9_t g_origDirect3DCreate9 = nullptr;
static Direct3DCreate9Ex_t g_origDirect3DCreate9Ex = nullptr;
static D3DPERF_BeginEvent_t g_origD3DPERF_BeginEvent = nullptr;
static D3DPERF_EndEvent_t g_origD3DPERF_EndEvent = nullptr;
static D3DPERF_GetStatus_t g_origD3DPERF_GetStatus = nullptr;
static D3DPERF_QueryRepeatFrame_t g_origD3DPERF_QueryRepeatFrame = nullptr;
static D3DPERF_SetMarker_t g_origD3DPERF_SetMarker = nullptr;
static D3DPERF_SetOptions_t g_origD3DPERF_SetOptions = nullptr;
static D3DPERF_SetRegion_t g_origD3DPERF_SetRegion = nullptr;

// Logging helper
void LogMsg(const char* fmt, ...) {
    if (!g_config.enableLogging || !g_logFile) return;

    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
    va_end(args);
}

// Check if matrix values are valid
bool LooksLikeMatrix(const float* data) {
    float sum = 0;
    for (int i = 0; i < 16; i++) {
        if (!std::isfinite(data[i])) return false;
        sum += fabsf(data[i]);
    }
    if (sum < 0.001f || sum > 10000.0f) return false;
    return true;
}

// Extract FOV from projection matrix
float ExtractFOV(const D3DMATRIX& proj) {
    if (fabsf(proj._22) < 0.001f) return 0;
    return 2.0f * atanf(1.0f / proj._22);
}

// Check if matrix looks like projection
bool LooksLikeProjection(const D3DMATRIX& m) {
    // Check for typical projection structure
    if (fabsf(m._12) > 0.01f || fabsf(m._13) > 0.01f || fabsf(m._14) > 0.01f) return false;
    if (fabsf(m._21) > 0.01f || fabsf(m._23) > 0.01f || fabsf(m._24) > 0.01f) return false;
    if (fabsf(m._31) > 0.01f || fabsf(m._32) > 0.01f) return false;
    if (fabsf(m._11) < 0.01f || fabsf(m._22) < 0.01f) return false;

    float fov = ExtractFOV(m);
    if (fov < g_config.minFOV || fov > g_config.maxFOV) return false;

    return true;
}

// Create a standard perspective projection matrix
void CreateProjectionMatrix(D3DMATRIX* out, float fovY, float aspect, float zNear, float zFar) {
    float yScale = 1.0f / tanf(fovY / 2.0f);
    float xScale = yScale / aspect;
    memset(out, 0, sizeof(D3DMATRIX));
    out->_11 = xScale;
    out->_22 = yScale;
    out->_33 = zFar / (zFar - zNear);
    out->_34 = 1.0f;
    out->_43 = -zNear * zFar / (zFar - zNear);
}

// Create an identity matrix
void CreateIdentityMatrix(D3DMATRIX* out) {
    memset(out, 0, sizeof(D3DMATRIX));
    out->_11 = out->_22 = out->_33 = out->_44 = 1.0f;
}

// Try to extract camera position from MVP-like matrix
void ExtractCameraFromMVP(const D3DMATRIX& mvp, D3DMATRIX* viewOut) {
    // The MVP matrix has rotation in the upper 3x3 and translation in column 4
    // We can try to extract a view matrix by normalizing the rotation part
    CreateIdentityMatrix(viewOut);

    // Extract rotation vectors (may be scaled by projection)
    float r0len = sqrtf(mvp._11*mvp._11 + mvp._12*mvp._12 + mvp._13*mvp._13);
    float r1len = sqrtf(mvp._21*mvp._21 + mvp._22*mvp._22 + mvp._23*mvp._23);
    float r2len = sqrtf(mvp._31*mvp._31 + mvp._32*mvp._32 + mvp._33*mvp._33);

    if (r0len > 0.001f && r1len > 0.001f && r2len > 0.001f) {
        // Normalize to get rotation
        viewOut->_11 = mvp._11 / r0len; viewOut->_12 = mvp._12 / r0len; viewOut->_13 = mvp._13 / r0len;
        viewOut->_21 = mvp._21 / r1len; viewOut->_22 = mvp._22 / r1len; viewOut->_23 = mvp._23 / r1len;
        viewOut->_31 = mvp._31 / r2len; viewOut->_32 = mvp._32 / r2len; viewOut->_33 = mvp._33 / r2len;

        // Translation (approximate - this is complex with combined matrices)
        viewOut->_41 = mvp._14 / r0len;
        viewOut->_42 = mvp._24 / r1len;
        viewOut->_43 = mvp._34 / r2len;
    }
}

// Check if matrix looks like view matrix (orthonormal rotation + translation)
bool LooksLikeView(const D3DMATRIX& m) {
    float row0len = sqrtf(m._11*m._11 + m._12*m._12 + m._13*m._13);
    float row1len = sqrtf(m._21*m._21 + m._22*m._22 + m._23*m._23);
    float row2len = sqrtf(m._31*m._31 + m._32*m._32 + m._33*m._33);

    if (fabsf(row0len - 1.0f) > 0.1f) return false;
    if (fabsf(row1len - 1.0f) > 0.1f) return false;
    if (fabsf(row2len - 1.0f) > 0.1f) return false;

    if (fabsf(m._14) > 0.01f || fabsf(m._24) > 0.01f || fabsf(m._34) > 0.01f) return false;
    if (fabsf(m._44 - 1.0f) > 0.01f) return false;

    return true;
}

// Forward declarations
class WrappedD3D9Device;
class WrappedD3D9;

/**
 * Wrapped IDirect3DDevice9 - intercepts SetVertexShaderConstantF
 */
class WrappedD3D9Device : public IDirect3DDevice9 {
private:
    IDirect3DDevice9* m_real;
    D3DMATRIX m_lastViewMatrix;
    D3DMATRIX m_lastProjMatrix;
    D3DMATRIX m_lastWorldMatrix;
    HWND m_hwnd = nullptr;
    IDirect3DVertexShader9* m_currentVertexShader = nullptr;
    bool m_hasView = false;
    bool m_hasProj = false;
    int m_constantLogThrottle = 0;

public:
    WrappedD3D9Device(IDirect3DDevice9* real) : m_real(real) {
        memset(&m_lastViewMatrix, 0, sizeof(D3DMATRIX));
        memset(&m_lastProjMatrix, 0, sizeof(D3DMATRIX));
        memset(&m_lastWorldMatrix, 0, sizeof(D3DMATRIX));
        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (SUCCEEDED(m_real->GetCreationParameters(&params))) {
            m_hwnd = params.hFocusWindow;
        }
        if (!m_hwnd) {
            m_hwnd = GetForegroundWindow();
        }
        LogMsg("WrappedD3D9Device created, wrapping device at %p", real);
    }

    ~WrappedD3D9Device() {
        LogMsg("WrappedD3D9Device destroyed");
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        HRESULT hr = m_real->QueryInterface(riid, ppvObj);
        // Don't wrap QueryInterface results - could cause issues
        return hr;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_real->AddRef();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) {
            ShutdownImGui();
            delete this;
        }
        return count;
    }

    // The key interception point
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(
        UINT StartRegister,
        const float* pConstantData,
        UINT Vector4fCount) override
    {
        uintptr_t shaderKey = reinterpret_cast<uintptr_t>(m_currentVertexShader);
        ShaderConstantState* state = GetShaderState(shaderKey, true);
        for (UINT i = 0; i < Vector4fCount; i++) {
            UINT reg = StartRegister + i;
            if (reg >= kMaxConstantRegisters) {
                break;
            }
            memcpy(state->constants[reg], pConstantData + i * 4, sizeof(state->constants[reg]));
            state->valid[reg] = true;
        }
        state->snapshotReady = true;

        // Diagnostic logging mode - log ALL constant updates
        if (g_config.logAllConstants && m_constantLogThrottle == 0) {
            if (Vector4fCount >= 4) {
                LogMsg("SetVertexShaderConstantF: c%d-%d (%d vectors)",
                       StartRegister, StartRegister + Vector4fCount - 1, Vector4fCount);

                // Log first 16 floats (one matrix worth)
                for (UINT i = 0; i < Vector4fCount && i < 4; i++) {
                    LogMsg("  c%d: [%.3f, %.3f, %.3f, %.3f]",
                           StartRegister + i,
                           pConstantData[i*4+0], pConstantData[i*4+1],
                           pConstantData[i*4+2], pConstantData[i*4+3]);
                }
            }
        }

        // DMC4-specific: c0-c3 contains combined MVP matrix
        // Extract what we can and synthesize valid View/Projection data
        if (StartRegister == 0 && Vector4fCount >= 4) {
            D3DMATRIX mvp;
            memcpy(&mvp, pConstantData, sizeof(D3DMATRIX));
            StoreMVPMatrix(mvp);

            bool allowMvpExtraction = g_config.autoDetectMatrices ||
                                      (g_config.viewMatrixRegister < 0 &&
                                       g_config.projMatrixRegister < 0);

            if (allowMvpExtraction) {
                // Check for valid floats (skip LooksLikeMatrix - MVP has large values)
                bool validFloats = true;
                for (int i = 0; i < 16 && validFloats; i++) {
                    if (!std::isfinite(pConstantData[i])) validFloats = false;
                }

                if (validFloats) {
                    // Extract approximate View from MVP
                    ExtractCameraFromMVP(mvp, &m_lastViewMatrix);

                    // Create standard projection (60 degree FOV, 16:9 aspect)
                    CreateProjectionMatrix(&m_lastProjMatrix, 1.047f, 16.0f/9.0f, 0.1f, 10000.0f);

                    // Set view transform for the runtime
                    m_real->SetTransform(D3DTS_VIEW, &m_lastViewMatrix);
                    StoreViewMatrix(m_lastViewMatrix);
                    StoreProjectionMatrix(m_lastProjMatrix);

                    if (!m_hasView) {
                        LogMsg("DMC4: Extracting camera from MVP at c0-c3");
                        LogMsg("  MVP row0: [%.3f, %.3f, %.3f, %.3f]", mvp._11, mvp._12, mvp._13, mvp._14);
                        LogMsg("  MVP row1: [%.3f, %.3f, %.3f, %.3f]", mvp._21, mvp._22, mvp._23, mvp._24);
                        LogMsg("  MVP row2: [%.3f, %.3f, %.3f, %.3f]", mvp._31, mvp._32, mvp._33, mvp._34);
                        LogMsg("  MVP row3: [%.3f, %.3f, %.3f, %.3f]", mvp._41, mvp._42, mvp._43, mvp._44);
                        m_hasView = true;
                        m_hasProj = true;
                    }
                }
            }
        }

        // Auto-detect mode - scan for matrices (fallback)
        if (g_config.autoDetectMatrices && Vector4fCount >= 4 && StartRegister != 0) {
            for (UINT offset = 0; offset + 4 <= Vector4fCount; offset++) {
                const float* matData = pConstantData + offset * 4;
                if (LooksLikeMatrix(matData)) {
                    D3DMATRIX mat;
                    memcpy(&mat, matData, sizeof(D3DMATRIX));

                    UINT reg = StartRegister + offset;
                    if (LooksLikeProjection(mat)) {
                        float fov = ExtractFOV(mat) * 180.0f / 3.14159f;
                        LogMsg("AUTO-DETECT: PROJECTION matrix at c%d (FOV: %.1f deg)", reg, fov);
                        memcpy(&m_lastProjMatrix, &mat, sizeof(D3DMATRIX));
                        m_hasProj = true;
                        StoreProjectionMatrix(m_lastProjMatrix);
                    }
                    else if (LooksLikeView(mat)) {
                        LogMsg("AUTO-DETECT: VIEW matrix at c%d", reg);
                        m_real->SetTransform(D3DTS_VIEW, &mat);
                        memcpy(&m_lastViewMatrix, &mat, sizeof(D3DMATRIX));
                        m_hasView = true;
                        StoreViewMatrix(m_lastViewMatrix);
                    }
                }
            }
        }

        // Configured register detection
        // Check for view matrix at configured register
        if (g_config.viewMatrixRegister >= 0 &&
            StartRegister <= (UINT)g_config.viewMatrixRegister &&
            StartRegister + Vector4fCount >= (UINT)g_config.viewMatrixRegister + 4)
        {
            int offset = (g_config.viewMatrixRegister - StartRegister) * 4;
            const float* matrixData = pConstantData + offset;

            if (LooksLikeMatrix(matrixData)) {
                D3DMATRIX mat;
                memcpy(&mat, matrixData, sizeof(D3DMATRIX));

                if (LooksLikeView(mat)) {
                    memcpy(&m_lastViewMatrix, &mat, sizeof(D3DMATRIX));
                    m_hasView = true;
                    m_real->SetTransform(D3DTS_VIEW, &m_lastViewMatrix);
                    StoreViewMatrix(m_lastViewMatrix);
                    LogMsg("Extracted VIEW matrix from c%d", g_config.viewMatrixRegister);
                }
            }
        }

        // Check for projection matrix at configured register
        if (g_config.projMatrixRegister >= 0 &&
            StartRegister <= (UINT)g_config.projMatrixRegister &&
            StartRegister + Vector4fCount >= (UINT)g_config.projMatrixRegister + 4)
        {
            int offset = (g_config.projMatrixRegister - StartRegister) * 4;
            const float* matrixData = pConstantData + offset;

            if (LooksLikeMatrix(matrixData)) {
                D3DMATRIX mat;
                memcpy(&mat, matrixData, sizeof(D3DMATRIX));

                if (LooksLikeProjection(mat)) {
                    memcpy(&m_lastProjMatrix, &mat, sizeof(D3DMATRIX));
                    m_hasProj = true;
                    StoreProjectionMatrix(m_lastProjMatrix);

                    float fov = ExtractFOV(mat) * 180.0f / 3.14159f;
                    LogMsg("Extracted PROJECTION matrix from c%d (FOV: %.1f deg)",
                           g_config.projMatrixRegister, fov);
                }
            }
        }

        // Check for world matrix at configured register
        if (g_config.worldMatrixRegister >= 0 &&
            StartRegister <= (UINT)g_config.worldMatrixRegister &&
            StartRegister + Vector4fCount >= (UINT)g_config.worldMatrixRegister + 4)
        {
            int offset = (g_config.worldMatrixRegister - StartRegister) * 4;
            const float* matrixData = pConstantData + offset;

            if (LooksLikeMatrix(matrixData)) {
                D3DMATRIX mat;
                memcpy(&mat, matrixData, sizeof(D3DMATRIX));
                memcpy(&m_lastWorldMatrix, &mat, sizeof(D3DMATRIX));
                StoreWorldMatrix(m_lastWorldMatrix);
            }
        }

        return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }

    // Present - good place to do per-frame logging throttle
    HRESULT STDMETHODCALLTYPE Present(const RECT* pSourceRect, const RECT* pDestRect,
                                       HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override {
        g_frameCount++;
        UpdateFrameTimeStats();
        // Throttle constant logging to every 60 frames
        if (g_config.logAllConstants) {
            m_constantLogThrottle = (m_constantLogThrottle + 1) % 60;
        }
        UpdateConstantSnapshot();

        // Log periodic status
        if (g_frameCount % 300 == 0) {
            LogMsg("Frame %d - hasView: %d, hasProj: %d", g_frameCount, m_hasView, m_hasProj);
        }

        if (!g_imguiInitialized) {
            InitializeImGui(m_real, m_hwnd);
        }
        UpdateImGuiToggle();
        UpdatePauseToggle();
        UpdateFreecamToggle();
        UpdateFreecam(m_real, m_hwnd);
        RenderImGuiOverlay();

        if (g_pauseRendering) {
            Sleep(1);
            return D3D_OK;
        }

        return m_real->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    // All other methods pass through
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return m_real->TestCooperativeLevel(); }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return m_real->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return m_real->EvictManagedResources(); }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override { return m_real->GetDirect3D(ppD3D9); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override { return m_real->GetDeviceCaps(pCaps); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override { return m_real->GetDisplayMode(iSwapChain, pMode); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) override { return m_real->GetCreationParameters(pParameters); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override { return m_real->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
    void STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override { m_real->SetCursorPosition(X, Y, Flags); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return m_real->ShowCursor(bShow); }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override { return m_real->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override { return m_real->GetSwapChain(iSwapChain, pSwapChain); }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return m_real->GetNumberOfSwapChains(); }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override {
        if (g_imguiInitialized) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }
        HRESULT hr = m_real->Reset(pPresentationParameters);
        if (SUCCEEDED(hr) && g_imguiInitialized) {
            ImGui_ImplDX9_CreateDeviceObjects();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override { return m_real->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override { return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override { return m_real->SetDialogBoxMode(bEnableDialogs); }
    void STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) override { m_real->SetGammaRamp(iSwapChain, Flags, pRamp); }
    void STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override { m_real->GetGammaRamp(iSwapChain, pRamp); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override { return m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override { return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override { return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override { return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override { return m_real->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, const POINT* pDestPoint) override { return m_real->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override { return m_real->UpdateTexture(pSourceTexture, pDestinationTexture); }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override { return m_real->GetRenderTargetData(pRenderTarget, pDestSurface); }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override { return m_real->GetFrontBufferData(iSwapChain, pDestSurface); }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) override { return m_real->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurface, const RECT* pRect, D3DCOLOR color) override { return m_real->ColorFill(pSurface, pRect, color); }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override { return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override { return m_real->SetRenderTarget(RenderTargetIndex, pRenderTarget); }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override { return m_real->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override { return m_real->SetDepthStencilSurface(pNewZStencil); }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override { return m_real->GetDepthStencilSurface(ppZStencilSurface); }
    HRESULT STDMETHODCALLTYPE BeginScene() override { return m_real->BeginScene(); }
    HRESULT STDMETHODCALLTYPE EndScene() override { return m_real->EndScene(); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override { return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override { return m_real->SetTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override { return m_real->GetTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override { return m_real->MultiplyTransform(State, pMatrix); }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* pViewport) override { return m_real->SetViewport(pViewport); }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override { return m_real->GetViewport(pViewport); }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* pMaterial) override { return m_real->SetMaterial(pMaterial); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override { return m_real->GetMaterial(pMaterial); }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, const D3DLIGHT9* pLight) override { return m_real->SetLight(Index, pLight); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9* pLight) override { return m_real->GetLight(Index, pLight); }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override { return m_real->LightEnable(Index, Enable); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL* pEnable) override { return m_real->GetLightEnable(Index, pEnable); }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, const float* pPlane) override { return m_real->SetClipPlane(Index, pPlane); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane) override { return m_real->GetClipPlane(Index, pPlane); }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override { return m_real->SetRenderState(State, Value); }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override { return m_real->GetRenderState(State, pValue); }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override { return m_real->CreateStateBlock(Type, ppSB); }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return m_real->BeginStateBlock(); }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override { return m_real->EndStateBlock(ppSB); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) override { return m_real->SetClipStatus(pClipStatus); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { return m_real->GetClipStatus(pClipStatus); }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override { return m_real->GetTexture(Stage, ppTexture); }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override { return m_real->SetTexture(Stage, pTexture); }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override { return m_real->GetTextureStageState(Stage, Type, pValue); }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { return m_real->SetTextureStageState(Stage, Type, Value); }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override { return m_real->GetSamplerState(Sampler, Type, pValue); }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override { return m_real->SetSamplerState(Sampler, Type, Value); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override { return m_real->ValidateDevice(pNumPasses); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) override { return m_real->SetPaletteEntries(PaletteNumber, pEntries); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override { return m_real->GetPaletteEntries(PaletteNumber, pEntries); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return m_real->SetCurrentTexturePalette(PaletteNumber); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* PaletteNumber) override { return m_real->GetCurrentTexturePalette(PaletteNumber); }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT* pRect) override { return m_real->SetScissorRect(pRect); }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override { return m_real->GetScissorRect(pRect); }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { return m_real->SetSoftwareVertexProcessing(bSoftware); }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return m_real->GetSoftwareVertexProcessing(); }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { return m_real->SetNPatchMode(nSegments); }
    float STDMETHODCALLTYPE GetNPatchMode() override { return m_real->GetNPatchMode(); }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override { return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override { return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override { return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override { return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { return m_real->SetVertexDeclaration(pDecl); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { return m_real->GetVertexDeclaration(ppDecl); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { return m_real->SetFVF(FVF); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { return m_real->GetFVF(pFVF); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) override { return m_real->CreateVertexShader(pFunction, ppShader); }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override {
        m_currentVertexShader = pShader;
        g_activeShaderKey = reinterpret_cast<uintptr_t>(pShader);
        GetShaderState(g_activeShaderKey, true);
        return m_real->SetVertexShader(pShader);
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override { return m_real->GetVertexShader(ppShader); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override { return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override { return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override { return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) override { return m_real->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override { return m_real->SetStreamSourceFreq(StreamNumber, Setting); }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) override { return m_real->GetStreamSourceFreq(StreamNumber, pSetting); }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override { return m_real->SetIndices(pIndexData); }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { return m_real->GetIndices(ppIndexData); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) override { return m_real->CreatePixelShader(pFunction, ppShader); }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override { return m_real->SetPixelShader(pShader); }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { return m_real->GetPixelShader(ppShader); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override { return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override { return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override { return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) override { return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) override { return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return m_real->DeletePatch(Handle); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override { return m_real->CreateQuery(Type, ppQuery); }
};

/**
 * Wrapped IDirect3D9 - intercepts CreateDevice to return wrapped devices
 */
class WrappedD3D9 : public IDirect3D9 {
private:
    IDirect3D9* m_real;

public:
    WrappedD3D9(IDirect3D9* real) : m_real(real) {
        LogMsg("WrappedD3D9 created, wrapping IDirect3D9 at %p", real);
    }

    ~WrappedD3D9() {
        LogMsg("WrappedD3D9 destroyed");
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        return m_real->QueryInterface(riid, ppvObj);
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_real->AddRef();
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IDirect3D9 methods
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override {
        return m_real->RegisterSoftwareDevice(pInitializeFunction);
    }

    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        return m_real->GetAdapterCount();
    }

    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override {
        return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
    }

    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override {
        return m_real->GetAdapterModeCount(Adapter, Format);
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override {
        return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode);
    }

    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override {
        return m_real->GetAdapterDisplayMode(Adapter, pMode);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override {
        return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override {
        return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
    }

    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override {
        return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
    }

    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override {
        return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override {
        return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps);
    }

    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override {
        return m_real->GetAdapterMonitor(Adapter);
    }

    // The key interception - wrap the device!
    HRESULT STDMETHODCALLTYPE CreateDevice(
        UINT Adapter,
        D3DDEVTYPE DeviceType,
        HWND hFocusWindow,
        DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        LogMsg("CreateDevice called - Adapter: %d, DeviceType: %d", Adapter, DeviceType);

        IDirect3DDevice9* realDevice = nullptr;
        HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                          pPresentationParameters, &realDevice);

        if (SUCCEEDED(hr) && realDevice) {
            LogMsg("CreateDevice succeeded, wrapping device");
            *ppReturnedDeviceInterface = new WrappedD3D9Device(realDevice);
        } else {
            LogMsg("CreateDevice failed with HRESULT: 0x%08X", hr);
            *ppReturnedDeviceInterface = nullptr;
        }

        return hr;
    }
};

/**
 * Wrapped IDirect3D9Ex - for games that use Direct3DCreate9Ex
 */
class WrappedD3D9Ex : public IDirect3D9Ex {
private:
    IDirect3D9Ex* m_real;

public:
    WrappedD3D9Ex(IDirect3D9Ex* real) : m_real(real) {
        LogMsg("WrappedD3D9Ex created, wrapping IDirect3D9Ex at %p", real);
    }

    ~WrappedD3D9Ex() {
        LogMsg("WrappedD3D9Ex destroyed");
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        return m_real->QueryInterface(riid, ppvObj);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) delete this;
        return count;
    }

    // IDirect3D9 methods
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override { return m_real->RegisterSoftwareDevice(pInitializeFunction); }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_real->GetAdapterCount(); }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override { return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return m_real->GetAdapterModeCount(Adapter, Format); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override { return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override { return m_real->GetAdapterDisplayMode(Adapter, pMode); }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels) override { return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override { return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps); }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return m_real->GetAdapterMonitor(Adapter); }

    HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        LogMsg("CreateDevice (via Ex) called");
        IDirect3DDevice9* realDevice = nullptr;
        HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, &realDevice);
        if (SUCCEEDED(hr) && realDevice) {
            *ppReturnedDeviceInterface = new WrappedD3D9Device(realDevice);
        } else {
            *ppReturnedDeviceInterface = nullptr;
        }
        return hr;
    }

    // IDirect3D9Ex methods
    UINT STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter) override { return m_real->GetAdapterModeCountEx(Adapter, pFilter); }
    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode) override { return m_real->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode); }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) override { return m_real->GetAdapterDisplayModeEx(Adapter, pMode, pRotation); }
    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID* pLUID) override { return m_real->GetAdapterLUID(Adapter, pLUID); }

    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode,
        IDirect3DDevice9Ex** ppReturnedDeviceInterface) override
    {
        LogMsg("CreateDeviceEx called");
        IDirect3DDevice9Ex* realDevice = nullptr;
        HRESULT hr = m_real->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                            pPresentationParameters, pFullscreenDisplayMode, &realDevice);
        if (SUCCEEDED(hr) && realDevice) {
            // Note: We wrap as IDirect3DDevice9, but the game expects IDirect3DDevice9Ex
            // This is a simplification - a full implementation would need a WrappedD3D9DeviceEx
            LogMsg("CreateDeviceEx succeeded, wrapping device (as base Device9)");
            *ppReturnedDeviceInterface = (IDirect3DDevice9Ex*)new WrappedD3D9Device(realDevice);
        } else {
            LogMsg("CreateDeviceEx failed: 0x%08X", hr);
            *ppReturnedDeviceInterface = nullptr;
        }
        return hr;
    }
};

// Load configuration from ini file
void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);

    char* lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "camera_proxy.ini");
    }

    g_config.viewMatrixRegister = GetPrivateProfileIntA("CameraProxy", "ViewMatrixRegister", 4, path);
    g_config.projMatrixRegister = GetPrivateProfileIntA("CameraProxy", "ProjMatrixRegister", 8, path);
    g_config.worldMatrixRegister = GetPrivateProfileIntA("CameraProxy", "WorldMatrixRegister", 0, path);
    g_config.enableLogging = GetPrivateProfileIntA("CameraProxy", "EnableLogging", 1, path) != 0;
    g_config.logAllConstants = GetPrivateProfileIntA("CameraProxy", "LogAllConstants", 0, path) != 0;
    g_config.autoDetectMatrices = GetPrivateProfileIntA("CameraProxy", "AutoDetectMatrices", 0, path) != 0;

    char buf[64];
    GetPrivateProfileStringA("CameraProxy", "MinFOV", "0.1", buf, sizeof(buf), path);
    g_config.minFOV = (float)atof(buf);
    GetPrivateProfileStringA("CameraProxy", "MaxFOV", "2.5", buf, sizeof(buf), path);
    g_config.maxFOV = (float)atof(buf);
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);

        LoadConfig();

        if (g_config.enableLogging) {
            g_logFile = fopen("camera_proxy.log", "w");
            LogMsg("=== DMC4 Camera Proxy for D3D9 ===");
            LogMsg("View matrix register: c%d-c%d", g_config.viewMatrixRegister, g_config.viewMatrixRegister + 3);
            LogMsg("Projection matrix register: c%d-c%d", g_config.projMatrixRegister, g_config.projMatrixRegister + 3);
            LogMsg("Auto-detect matrices: %s", g_config.autoDetectMatrices ? "ENABLED" : "disabled");
            LogMsg("Log all constants: %s", g_config.logAllConstants ? "ENABLED" : "disabled");
        }

        // Load the real system d3d9.dll
        g_hD3D9 = LoadSystemD3D9();

        if (g_hD3D9) {
            g_origDirect3DCreate9 = (Direct3DCreate9_t)GetProcAddress(g_hD3D9, "Direct3DCreate9");
            g_origDirect3DCreate9Ex = (Direct3DCreate9Ex_t)GetProcAddress(g_hD3D9, "Direct3DCreate9Ex");
            g_origD3DPERF_BeginEvent = (D3DPERF_BeginEvent_t)GetProcAddress(g_hD3D9, "D3DPERF_BeginEvent");
            g_origD3DPERF_EndEvent = (D3DPERF_EndEvent_t)GetProcAddress(g_hD3D9, "D3DPERF_EndEvent");
            g_origD3DPERF_GetStatus = (D3DPERF_GetStatus_t)GetProcAddress(g_hD3D9, "D3DPERF_GetStatus");
            g_origD3DPERF_QueryRepeatFrame = (D3DPERF_QueryRepeatFrame_t)GetProcAddress(g_hD3D9, "D3DPERF_QueryRepeatFrame");
            g_origD3DPERF_SetMarker = (D3DPERF_SetMarker_t)GetProcAddress(g_hD3D9, "D3DPERF_SetMarker");
            g_origD3DPERF_SetOptions = (D3DPERF_SetOptions_t)GetProcAddress(g_hD3D9, "D3DPERF_SetOptions");
            g_origD3DPERF_SetRegion = (D3DPERF_SetRegion_t)GetProcAddress(g_hD3D9, "D3DPERF_SetRegion");
            LogMsg("Loaded system d3d9.dll successfully");
            LogMsg("  Direct3DCreate9: %p", g_origDirect3DCreate9);
            LogMsg("  Direct3DCreate9Ex: %p", g_origDirect3DCreate9Ex);
        } else {
            LogMsg("ERROR: Failed to load system d3d9.dll!");
        }
    }
    else if (fdwReason == DLL_PROCESS_DETACH) {
        if (g_logFile) {
            LogMsg("=== Camera Proxy unloading ===");
            LogMsg("Total frames: %d", g_frameCount);
            fclose(g_logFile);
        }
        if (g_hD3D9) {
            FreeLibrary(g_hD3D9);
        }
    }
    return TRUE;
}

// Exported functions - these are what the game calls
// Named with Proxy_ prefix to avoid conflict with SDK declarations
// The .def file maps these to the real export names

extern "C" {
    __declspec(dllexport) const CameraMatrices* WINAPI Proxy_GetCameraMatrices() {
        return &g_cameraMatrices;
    }

    IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT SDKVersion) {
        LogMsg("Direct3DCreate9 called (SDK version: %d)", SDKVersion);

        if (!g_origDirect3DCreate9) {
            LogMsg("ERROR: g_origDirect3DCreate9 is null!");
            return nullptr;
        }

        IDirect3D9* realD3D9 = g_origDirect3DCreate9(SDKVersion);
        if (!realD3D9) {
            LogMsg("ERROR: Original Direct3DCreate9 returned null!");
            return nullptr;
        }

        LogMsg("Wrapping IDirect3D9");
        return new WrappedD3D9(realD3D9);
    }

    HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D) {
        LogMsg("Direct3DCreate9Ex called (SDK version: %d)", SDKVersion);

        if (!g_origDirect3DCreate9Ex) {
            LogMsg("ERROR: g_origDirect3DCreate9Ex is null!");
            return E_FAIL;
        }

        IDirect3D9Ex* realD3D9Ex = nullptr;
        HRESULT hr = g_origDirect3DCreate9Ex(SDKVersion, &realD3D9Ex);

        if (SUCCEEDED(hr) && realD3D9Ex) {
            LogMsg("Wrapping IDirect3D9Ex");
            *ppD3D = new WrappedD3D9Ex(realD3D9Ex);
        } else {
            LogMsg("ERROR: Original Direct3DCreate9Ex failed: 0x%08X", hr);
            *ppD3D = nullptr;
        }

        return hr;
    }

    // D3DPERF forwarding functions
    int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) {
        if (g_origD3DPERF_BeginEvent) return g_origD3DPERF_BeginEvent(col, name);
        return 0;
    }

    int WINAPI Proxy_D3DPERF_EndEvent(void) {
        if (g_origD3DPERF_EndEvent) return g_origD3DPERF_EndEvent();
        return 0;
    }

    DWORD WINAPI Proxy_D3DPERF_GetStatus(void) {
        if (g_origD3DPERF_GetStatus) return g_origD3DPERF_GetStatus();
        return 0;
    }

    BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame(void) {
        if (g_origD3DPERF_QueryRepeatFrame) return g_origD3DPERF_QueryRepeatFrame();
        return FALSE;
    }

    void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name) {
        if (g_origD3DPERF_SetMarker) g_origD3DPERF_SetMarker(col, name);
    }

    void WINAPI Proxy_D3DPERF_SetOptions(DWORD options) {
        if (g_origD3DPERF_SetOptions) g_origD3DPERF_SetOptions(options);
    }

    void WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name) {
        if (g_origD3DPERF_SetRegion) g_origD3DPERF_SetRegion(col, name);
    }
}

// Build helper: include ImGui sources for single-translation-unit builds.
// #include "imgui/imgui.cpp"
// #include "imgui/imgui_draw.cpp"
// #include "imgui/imgui_tables.cpp"
// #include "imgui/imgui_widgets.cpp"
// #include "imgui/backends/imgui_impl_dx9.cpp"
// #include "imgui/backends/imgui_impl_win32.cpp"
