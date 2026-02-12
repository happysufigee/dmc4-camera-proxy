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
#include <string>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>

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
static void LogMsg(const char* fmt, ...);
static bool LooksLikeViewStrict(const D3DMATRIX& m);
static bool LooksLikeProjectionStrict(const D3DMATRIX& m);
float ExtractFOV(const D3DMATRIX& proj);
class WrappedD3D9Device;

#pragma comment(lib, "user32.lib")

// Configuration
struct ProxyConfig {
    int viewMatrixRegister = 4;
    int projMatrixRegister = 8;
    int worldMatrixRegister = 0;
    bool enableLogging = true;
    float minFOV = 0.1f;
    float maxFOV = 2.5f;
    int stabilityFrames = 5;
    float varianceThreshold = 0.0025f;
    int topCandidateCount = 5;
    bool logCandidateSummary = true;
    bool enableMemoryScanner = false;
    int memoryScannerIntervalSec = 0;
    int memoryScannerMaxResults = 25;
    char memoryScannerModule[MAX_PATH] = {};
    bool useRemixRuntime = true;
    char remixDllName[MAX_PATH] = "d3d9_remix.dll";
    bool emitFixedFunctionTransforms = true;

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

enum MatrixSlot {
    MatrixSlot_World = 0,
    MatrixSlot_View = 1,
    MatrixSlot_Projection = 2,
    MatrixSlot_MVP = 3,
    MatrixSlot_Count = 4
};

enum AutoDetectMode {
    AutoDetect_IndividualWVP = 0,
    AutoDetect_MVPOnly = 1
};


struct ManualMatrixBinding {
    bool enabled = false;
    uintptr_t shaderKey = 0;
    int baseRegister = -1;
    int rows = 4;
};

struct MatrixSourceInfo {
    bool valid = false;
    bool manual = false;
    uintptr_t shaderKey = 0;
    uint32_t shaderHash = 0;
    int baseRegister = -1;
    int rows = 4;
    bool transposed = false;
};

static CameraMatrices g_cameraMatrices = {};
static MatrixSourceInfo g_matrixSources[MatrixSlot_Count] = {};
static ManualMatrixBinding g_manualBindings[MatrixSlot_Count] = {};
static void UpdateMatrixSource(MatrixSlot slot,
                               uintptr_t shaderKey,
                               int baseRegister,
                               int rows,
                               bool transposed,
                               bool manual);

static bool g_imguiInitialized = false;
static HWND g_imguiHwnd = nullptr;
static bool g_showImGui = false;
static bool g_toggleWasDown = false;
static bool g_pauseRendering = false;
static bool g_isRenderingImGui = false;
static WNDPROC g_imguiPrevWndProc = nullptr;
static bool g_showConstantsAsMatrices = true;
static bool g_filterDetectedMatrices = false;
static bool g_showFpsStats = false;
static bool g_showTransposedMatrices = false;
static bool g_enableShaderEditing = false;
static bool g_requestManualEmit = false;
static char g_manualEmitStatus[192] = "";
static char g_matrixAssignStatus[256] = "";
static int g_manualAssignRows = 4;
static bool g_runtimeAutoDetectEnabled = false;
static bool g_autoApplyDetectedMatrices = true;
static int g_autoDetectMode = AutoDetect_IndividualWVP;
static int g_autoDetectMinFramesSeen = 12;
static int g_autoDetectMinConsecutiveFrames = 4;
static int g_autoDetectSamplingFrames = 180;
static int g_autoDetectSamplingStartFrame = -1;
static bool g_autoDetectSamplingActive = false;
static bool g_autoDetectSamplingPausedByUser = false;

static int g_iniViewMatrixRegister = 4;
static int g_iniProjMatrixRegister = 8;
static int g_iniWorldMatrixRegister = 0;

static constexpr int kMaxConstantRegisters = 256;
static int g_selectedRegister = -1;
static uintptr_t g_activeShaderKey = 0;
static uintptr_t g_selectedShaderKey = 0;

enum LayoutStrategyMode {
    Layout_Auto = 0,
    Layout_4x4 = 1,
    Layout_4x3 = 2,
    Layout_VP = 3,
    Layout_MVP = 4
};

enum OverrideScopeMode {
    Override_Sticky = 0,
    Override_OneFrame = 1,
    Override_NFrames = 2
};

struct HeuristicProfile {
    bool valid = false;
    int viewBase = -1;
    int projBase = -1;
    int layoutMode = Layout_Auto;
    bool transposed = false;
    bool inverseView = false;
};

static int g_layoutStrategyMode = Layout_Auto;
static bool g_probeTransposedLayouts = true;
static bool g_probeInverseView = true;
static bool g_autoPickCandidates = true;
static int g_overrideScopeMode = Override_Sticky;
static int g_overrideNFrames = 3;
static std::unordered_map<uintptr_t, uint32_t> g_shaderBytecodeHashes = {};
static std::unordered_map<uint32_t, HeuristicProfile> g_heuristicProfiles = {};

static bool TryGetShaderBytecodeHash(uintptr_t shaderKey, uint32_t* outHash) {
    if (!outHash || shaderKey == 0) {
        return false;
    }
    auto it = g_shaderBytecodeHashes.find(shaderKey);
    if (it == g_shaderBytecodeHashes.end() || it->second == 0) {
        return false;
    }
    *outHash = it->second;
    return true;
}

struct ShaderConstantState {
    float constants[kMaxConstantRegisters][4] = {};
    bool valid[kMaxConstantRegisters] = {};
    float overrideConstants[kMaxConstantRegisters][4] = {};
    bool overrideValid[kMaxConstantRegisters] = {};
    int overrideFramesRemaining[kMaxConstantRegisters] = {};
    bool snapshotReady = false;
    unsigned long long sampleCount = 0;
    double mean[kMaxConstantRegisters][4] = {};
    double m2[kMaxConstantRegisters][4] = {};
    int stableViewBase = -1;
    int stableViewCount = 0;
    int stableProjBase = -1;
    int stableProjCount = 0;
    unsigned long long lastChangeSerial = 0;
};

static ShaderConstantState* GetShaderState(uintptr_t shaderKey, bool createIfMissing);

static std::unordered_map<uintptr_t, ShaderConstantState> g_shaderConstants = {};
static std::vector<uintptr_t> g_shaderOrder = {};
static std::unordered_map<uintptr_t, bool> g_disabledShaders = {};
static unsigned long long g_constantChangeSerial = 0;
static HANDLE g_memoryScannerThread = nullptr;
static DWORD g_memoryScannerThreadId = 0;
static DWORD g_memoryScannerLastTick = 0;

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

static std::deque<std::string> g_logLines = {};
static std::vector<std::string> g_logSnapshot = {};
static std::vector<std::string> g_memoryScanResults = {};

struct MemoryScanHit {
    std::string label;
    D3DMATRIX matrix = {};
    MatrixSlot slot = MatrixSlot_View;
    uintptr_t address = 0;
    uint32_t hash = 0;
};

static std::vector<MemoryScanHit> g_memoryScanHits = {};
static std::mutex g_uiDataMutex;
static constexpr size_t kMaxUiLogLines = 600;
static bool g_logsLiveUpdate = false;
static bool g_logSnapshotDirty = true;


static LRESULT CALLBACK ImGuiWndProcHook(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_imguiInitialized) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
    }
    if (g_imguiPrevWndProc) {
        return CallWindowProc(g_imguiPrevWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void StoreViewMatrix(const D3DMATRIX& view,
                            uintptr_t shaderKey = 0,
                            int baseRegister = -1,
                            int rows = 4,
                            bool transposed = false,
                            bool manual = false) {
    g_cameraMatrices.view = view;
    g_cameraMatrices.hasView = true;
    UpdateMatrixSource(MatrixSlot_View, shaderKey, baseRegister, rows, transposed, manual);
}

static void StoreProjectionMatrix(const D3DMATRIX& projection,
                                  uintptr_t shaderKey = 0,
                                  int baseRegister = -1,
                                  int rows = 4,
                                  bool transposed = false,
                                  bool manual = false) {
    g_cameraMatrices.projection = projection;
    g_cameraMatrices.hasProjection = true;
    UpdateMatrixSource(MatrixSlot_Projection, shaderKey, baseRegister, rows, transposed, manual);
}

static void StoreWorldMatrix(const D3DMATRIX& world,
                             uintptr_t shaderKey = 0,
                             int baseRegister = -1,
                             int rows = 4,
                             bool transposed = false,
                             bool manual = false) {
    g_cameraMatrices.world = world;
    g_cameraMatrices.hasWorld = true;
    UpdateMatrixSource(MatrixSlot_World, shaderKey, baseRegister, rows, transposed, manual);
}

static void StoreMVPMatrix(const D3DMATRIX& mvp,
                           uintptr_t shaderKey = 0,
                           int baseRegister = -1,
                           int rows = 4,
                           bool transposed = false,
                           bool manual = false) {
    g_cameraMatrices.mvp = mvp;
    g_cameraMatrices.hasMVP = true;
    UpdateMatrixSource(MatrixSlot_MVP, shaderKey, baseRegister, rows, transposed, manual);
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

static HMODULE LoadTargetD3D9() {
    if (g_config.useRemixRuntime) {
        HMODULE remixModule = LoadLibraryA(g_config.remixDllName);
        if (remixModule) {
            LogMsg("Loaded Remix runtime: %s", g_config.remixDllName);
            return remixModule;
        }
        LogMsg("WARNING: Failed to load Remix runtime '%s', falling back to system d3d9.dll", g_config.remixDllName);
    }

    HMODULE systemModule = LoadSystemD3D9();
    if (systemModule) {
        LogMsg("Loaded system d3d9.dll");
    }
    return systemModule;
}

static void InitializeImGui(IDirect3DDevice9* device, HWND hwnd) {
    if (g_imguiInitialized || !device || !hwnd) {
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    // Replace default blue accents with a readable red palette.
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.30f, 0.30f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.88f, 0.28f, 0.28f, 1.00f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.40f, 0.40f, 1.00f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.38f, 0.16f, 0.16f, 0.72f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.64f, 0.22f, 0.22f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.26f, 0.26f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.45f, 0.17f, 0.17f, 0.70f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.68f, 0.24f, 0.24f, 0.88f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.28f, 0.28f, 0.95f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.58f, 0.20f, 0.20f, 0.58f);
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.78f, 0.28f, 0.28f, 0.80f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.92f, 0.34f, 0.34f, 1.00f);
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.70f, 0.24f, 0.24f, 0.35f);
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.90f, 0.32f, 0.32f, 0.78f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 0.38f, 0.38f, 0.95f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.28f, 0.12f, 0.12f, 0.90f);
    style.Colors[ImGuiCol_TabHovered] = ImVec4(0.60f, 0.22f, 0.22f, 0.88f);
    style.Colors[ImGuiCol_TabActive] = ImVec4(0.48f, 0.18f, 0.18f, 1.00f);
    style.Colors[ImGuiCol_TabUnfocused] = ImVec4(0.18f, 0.10f, 0.10f, 0.97f);
    style.Colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.33f, 0.14f, 0.14f, 1.00f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.55f, 0.20f, 0.20f, 0.80f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.45f, 0.18f, 0.18f, 0.80f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.45f, 0.16f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(0.96f, 0.34f, 0.34f, 1.00f);
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

static uint32_t HashBytesFNV1a(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t HashMatrix(const D3DMATRIX& mat) {
    return HashBytesFNV1a(reinterpret_cast<const uint8_t*>(&mat), sizeof(D3DMATRIX));
}

static uint32_t GetShaderHashForKey(uintptr_t shaderKey) {
    if (shaderKey == 0) {
        return 0;
    }
    uint32_t hash = 0;
    if (TryGetShaderBytecodeHash(shaderKey, &hash)) {
        return hash;
    }
    return HashBytesFNV1a(reinterpret_cast<const uint8_t*>(&shaderKey), sizeof(shaderKey));
}

static bool IsShaderDisabled(uintptr_t shaderKey) {
    auto it = g_disabledShaders.find(shaderKey);
    return it != g_disabledShaders.end() && it->second;
}

static void SetShaderDisabled(uintptr_t shaderKey, bool disabled) {
    if (shaderKey == 0) {
        return;
    }
    g_disabledShaders[shaderKey] = disabled;
}

static bool IsCurrentShaderDrawDisabled(IDirect3DVertexShader9* shader) {
    return IsShaderDisabled(reinterpret_cast<uintptr_t>(shader));
}

static float GetShaderFlashStrength(const ShaderConstantState* state) {
    if (!state || state->lastChangeSerial == 0 || g_constantChangeSerial < state->lastChangeSerial) {
        return 0.0f;
    }
    const unsigned long long age = g_constantChangeSerial - state->lastChangeSerial;
    if (age > 30ULL) {
        return 0.0f;
    }
    return 1.0f - (static_cast<float>(age) / 30.0f);
}

static void BuildShaderComboLabel(uintptr_t shaderKey, char* outLabel, size_t outSize) {
    if (!outLabel || outSize == 0) {
        return;
    }
    ShaderConstantState* state = GetShaderState(shaderKey, false);
    bool disabled = IsShaderDisabled(shaderKey);
    float flash = GetShaderFlashStrength(state);
    uint32_t displayHash = GetShaderHashForKey(shaderKey);
    snprintf(outLabel, outSize, "0x%p (hash 0x%08X)%s%s%s",
             reinterpret_cast<void*>(shaderKey),
             displayHash,
             shaderKey == g_activeShaderKey ? " (active)" : "",
             disabled ? " [DISABLED]" : "",
             flash > 0.0f ? " [changed]" : "");
}

static void RefreshLogSnapshot() {
    std::lock_guard<std::mutex> lock(g_uiDataMutex);
    g_logSnapshot.assign(g_logLines.begin(), g_logLines.end());
    g_logSnapshotDirty = false;
}

static void UpdateMatrixSource(MatrixSlot slot,
                               uintptr_t shaderKey,
                               int baseRegister,
                               int rows,
                               bool transposed,
                               bool manual) {
    MatrixSourceInfo info = {};
    info.valid = true;
    info.manual = manual;
    info.shaderKey = shaderKey;
    info.shaderHash = GetShaderHashForKey(shaderKey);
    info.baseRegister = baseRegister;
    info.rows = rows;
    info.transposed = transposed;
    g_matrixSources[slot] = info;
}

static void DrawMatrixSourceInfo(MatrixSlot slot, bool available) {
    if (!available) {
        return;
    }

    const MatrixSourceInfo& source = g_matrixSources[slot];
    if (!source.valid) {
        ImGui::Text("Source: <unknown>");
        return;
    }

    uint32_t shaderHash = 0;
    bool hasBytecodeHash = TryGetShaderBytecodeHash(source.shaderKey, &shaderHash);
    if (source.shaderKey == 0) {
        ImGui::Text("Source shader: <unknown>");
    } else {
        ImGui::Text("Source shader: 0x%p", reinterpret_cast<void*>(source.shaderKey));
        if (hasBytecodeHash) {
            ImGui::Text("Shader hash: 0x%08X", shaderHash);
        } else if (source.shaderHash != 0) {
            ImGui::Text("Shader hash: 0x%08X (fallback)", source.shaderHash);
        } else {
            ImGui::Text("Shader hash: <pending>");
        }
    }
    if (source.baseRegister >= 0) {
        int rows = source.rows > 0 ? source.rows : 4;
        ImGui::Text("Registers: c%d-c%d (%d rows)%s",
                    source.baseRegister,
                    source.baseRegister + (rows - 1),
                    rows,
                    source.transposed ? " [transposed]" : "");
    }
    ImGui::Text("Origin: %s", source.manual ? "manual constants selection" : "auto/config detection");
}



static bool CanAssignManualMatrix(MatrixSlot slot, char* reason, size_t reasonSize) {
    if (slot == MatrixSlot_View && g_iniViewMatrixRegister >= 0) {
        snprintf(reason, reasonSize,
                 "Manual VIEW assignment blocked: ViewMatrixRegister is configured in camera_proxy.ini.");
        return false;
    }
    if (slot == MatrixSlot_Projection && g_iniProjMatrixRegister >= 0) {
        snprintf(reason, reasonSize,
                 "Manual PROJECTION assignment blocked: ProjMatrixRegister is configured in camera_proxy.ini.");
        return false;
    }
    if (slot == MatrixSlot_World && g_iniWorldMatrixRegister >= 0) {
        snprintf(reason, reasonSize,
                 "Manual WORLD assignment blocked: WorldMatrixRegister is configured in camera_proxy.ini.");
        return false;
    }
    return true;
}

static const char* MatrixSlotLabel(MatrixSlot slot) {
    switch (slot) {
        case MatrixSlot_World: return "WORLD";
        case MatrixSlot_View: return "VIEW";
        case MatrixSlot_Projection: return "PROJECTION";
        case MatrixSlot_MVP: return "MVP";
        default: return "UNKNOWN";
    }
}

static void TryAssignManualMatrixFromSelection(MatrixSlot slot,
                                               uintptr_t shaderKey,
                                               int baseRegister,
                                               int rows,
                                               const D3DMATRIX& mat) {
    if (rows < 3 || rows > 4) {
        return;
    }

    char reason[256] = {};
    if (!CanAssignManualMatrix(slot, reason, sizeof(reason))) {
        snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus), "%s", reason);
        return;
    }

    g_manualBindings[slot].enabled = true;
    g_manualBindings[slot].shaderKey = shaderKey;
    g_manualBindings[slot].baseRegister = baseRegister;
    g_manualBindings[slot].rows = rows;

    if (slot == MatrixSlot_World) {
        StoreWorldMatrix(mat, shaderKey, baseRegister, rows, false, true);
    } else if (slot == MatrixSlot_View) {
        StoreViewMatrix(mat, shaderKey, baseRegister, rows, false, true);
    } else if (slot == MatrixSlot_Projection) {
        StoreProjectionMatrix(mat, shaderKey, baseRegister, rows, false, true);
    } else if (slot == MatrixSlot_MVP) {
        StoreMVPMatrix(mat, shaderKey, baseRegister, rows, false, true);
    }

    snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
             "Assigned %s from shader 0x%p registers c%d-c%d (%d rows).",
             MatrixSlotLabel(slot), reinterpret_cast<void*>(shaderKey), baseRegister,
             baseRegister + rows - 1, rows);
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


static uint32_t ComputeShaderBytecodeHash(IDirect3DVertexShader9* shader) {
    if (!shader) {
        return 0;
    }
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0) {
        return 0;
    }
    std::vector<uint8_t> data(size);
    if (FAILED(shader->GetFunction(data.data(), &size)) || size == 0) {
        return 0;
    }
    return HashBytesFNV1a(data.data(), size);
}

static void GetProfilesPath(char* outPath, size_t outSize) {
    if (!outPath || outSize == 0) {
        return;
    }
    GetModuleFileNameA(nullptr, outPath, static_cast<DWORD>(outSize));
    char* lastSlash = strrchr(outPath, '\\');
    if (lastSlash) {
        strcpy(lastSlash + 1, "camera_proxy_profiles.ini");
    }
}

static void SaveHeuristicProfile(uint32_t shaderHash, const HeuristicProfile& profile) {
    char path[MAX_PATH] = {};
    GetProfilesPath(path, MAX_PATH);
    char section[64] = {};
    snprintf(section, sizeof(section), "Shader_%08X", shaderHash);
    char value[64] = {};

    snprintf(value, sizeof(value), "%d", profile.valid ? 1 : 0);
    WritePrivateProfileStringA(section, "Valid", value, path);
    snprintf(value, sizeof(value), "%d", profile.viewBase);
    WritePrivateProfileStringA(section, "ViewBase", value, path);
    snprintf(value, sizeof(value), "%d", profile.projBase);
    WritePrivateProfileStringA(section, "ProjBase", value, path);
    snprintf(value, sizeof(value), "%d", profile.layoutMode);
    WritePrivateProfileStringA(section, "LayoutMode", value, path);
    snprintf(value, sizeof(value), "%d", profile.transposed ? 1 : 0);
    WritePrivateProfileStringA(section, "Transposed", value, path);
    snprintf(value, sizeof(value), "%d", profile.inverseView ? 1 : 0);
    WritePrivateProfileStringA(section, "InverseView", value, path);
}

static void LoadHeuristicProfiles() {
    g_heuristicProfiles.clear();
    char path[MAX_PATH] = {};
    GetProfilesPath(path, MAX_PATH);

    char sections[32768] = {};
    DWORD len = GetPrivateProfileSectionNamesA(sections, sizeof(sections), path);
    if (len == 0) {
        return;
    }

    const char* cur = sections;
    while (*cur) {
        uint32_t hash = 0;
        if (sscanf(cur, "Shader_%08X", &hash) == 1) {
            HeuristicProfile profile = {};
            profile.valid = GetPrivateProfileIntA(cur, "Valid", 0, path) != 0;
            profile.viewBase = GetPrivateProfileIntA(cur, "ViewBase", -1, path);
            profile.projBase = GetPrivateProfileIntA(cur, "ProjBase", -1, path);
            profile.layoutMode = GetPrivateProfileIntA(cur, "LayoutMode", Layout_Auto, path);
            profile.transposed = GetPrivateProfileIntA(cur, "Transposed", 0, path) != 0;
            profile.inverseView = GetPrivateProfileIntA(cur, "InverseView", 0, path) != 0;
            if (profile.valid) {
                g_heuristicProfiles[hash] = profile;
            }
        }
        cur += strlen(cur) + 1;
    }
}

static bool TryBuildMatrix4x3FromSnapshot(const ShaderConstantState& state, int baseRegister,
                                          bool transposed, D3DMATRIX* outMatrix) {
    if (!outMatrix || !state.snapshotReady || baseRegister < 0 || baseRegister + 2 >= kMaxConstantRegisters) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (!state.valid[baseRegister + i]) {
            return false;
        }
    }

    D3DMATRIX m = {};
    if (!transposed) {
        m._11 = state.constants[baseRegister + 0][0]; m._12 = state.constants[baseRegister + 0][1]; m._13 = state.constants[baseRegister + 0][2]; m._14 = state.constants[baseRegister + 0][3];
        m._21 = state.constants[baseRegister + 1][0]; m._22 = state.constants[baseRegister + 1][1]; m._23 = state.constants[baseRegister + 1][2]; m._24 = state.constants[baseRegister + 1][3];
        m._31 = state.constants[baseRegister + 2][0]; m._32 = state.constants[baseRegister + 2][1]; m._33 = state.constants[baseRegister + 2][2]; m._34 = state.constants[baseRegister + 2][3];
        m._41 = 0.0f; m._42 = 0.0f; m._43 = 0.0f; m._44 = 1.0f;
    } else {
        m._11 = state.constants[baseRegister + 0][0]; m._21 = state.constants[baseRegister + 0][1]; m._31 = state.constants[baseRegister + 0][2]; m._41 = state.constants[baseRegister + 0][3];
        m._12 = state.constants[baseRegister + 1][0]; m._22 = state.constants[baseRegister + 1][1]; m._32 = state.constants[baseRegister + 1][2]; m._42 = state.constants[baseRegister + 1][3];
        m._13 = state.constants[baseRegister + 2][0]; m._23 = state.constants[baseRegister + 2][1]; m._33 = state.constants[baseRegister + 2][2]; m._43 = state.constants[baseRegister + 2][3];
        m._14 = 0.0f; m._24 = 0.0f; m._34 = 0.0f; m._44 = 1.0f;
    }
    *outMatrix = m;
    return true;
}

static D3DMATRIX InvertSimpleRigidView(const D3DMATRIX& view) {
    D3DMATRIX out = {};
    out._11 = view._11; out._12 = view._21; out._13 = view._31;
    out._21 = view._12; out._22 = view._22; out._23 = view._32;
    out._31 = view._13; out._32 = view._23; out._33 = view._33;
    out._44 = 1.0f;
    out._41 = -(view._41 * out._11 + view._42 * out._21 + view._43 * out._31);
    out._42 = -(view._41 * out._12 + view._42 * out._22 + view._43 * out._32);
    out._43 = -(view._41 * out._13 + view._42 * out._23 + view._43 * out._33);
    return out;
}

static void ClearAllShaderOverrides() {
    for (auto& entry : g_shaderConstants) {
        ShaderConstantState& state = entry.second;
        memset(state.overrideConstants, 0, sizeof(state.overrideConstants));
        memset(state.overrideValid, 0, sizeof(state.overrideValid));
        memset(state.overrideFramesRemaining, 0, sizeof(state.overrideFramesRemaining));
    }
}

static void ClearShaderRegisterOverride(uintptr_t shaderKey, int reg) {
    if (reg < 0 || reg >= kMaxConstantRegisters) {
        return;
    }
    ShaderConstantState* state = GetShaderState(shaderKey, false);
    if (!state) {
        return;
    }
    memset(state->overrideConstants[reg], 0, sizeof(state->overrideConstants[reg]));
    state->overrideValid[reg] = false;
    state->overrideFramesRemaining[reg] = 0;
}

static bool BuildOverriddenConstants(ShaderConstantState& state,
                                     UINT startRegister,
                                     UINT vector4fCount,
                                     const float* sourceData,
                                     std::vector<float>& scratch) {
    if (!g_enableShaderEditing || !sourceData || vector4fCount == 0) {
        return false;
    }

    bool hasOverride = false;
    for (UINT i = 0; i < vector4fCount; i++) {
        UINT reg = startRegister + i;
        if (reg >= kMaxConstantRegisters) {
            break;
        }
        if (state.overrideValid[reg]) {
            hasOverride = true;
            break;
        }
    }
    if (!hasOverride) {
        return false;
    }

    scratch.assign(sourceData, sourceData + vector4fCount * 4);
    for (UINT i = 0; i < vector4fCount; i++) {
        UINT reg = startRegister + i;
        if (reg >= kMaxConstantRegisters) {
            break;
        }
        if (!state.overrideValid[reg]) {
            continue;
        }

        memcpy(&scratch[i * 4], state.overrideConstants[reg], sizeof(state.overrideConstants[reg]));

        if (state.overrideFramesRemaining[reg] > 0) {
            state.overrideFramesRemaining[reg]--;
            if (state.overrideFramesRemaining[reg] == 0) {
                state.overrideValid[reg] = false;
            }
        }
    }
    return true;
}

static void UpdateVariance(ShaderConstantState& state, int reg, const float* values) {
    state.sampleCount++;
    for (int i = 0; i < 4; i++) {
        double value = static_cast<double>(values[i]);
        double delta = value - state.mean[reg][i];
        state.mean[reg][i] += delta / static_cast<double>(state.sampleCount);
        double delta2 = value - state.mean[reg][i];
        state.m2[reg][i] += delta * delta2;
    }
}

static float GetVarianceMagnitude(const ShaderConstantState& state, int reg) {
    if (state.sampleCount < 2) {
        return 0.0f;
    }
    double sum = 0.0;
    for (int i = 0; i < 4; i++) {
        sum += state.m2[reg][i] / static_cast<double>(state.sampleCount - 1);
    }
    return static_cast<float>(sum / 4.0);
}


static bool TryBuildMatrixFromConstantUpdate(const float* constantData,
                                             UINT startRegister,
                                             UINT vector4fCount,
                                             int baseRegister,
                                             int rows,
                                             bool transposed,
                                             D3DMATRIX* outMatrix) {
    if (!constantData || !outMatrix || rows < 3 || rows > 4) {
        return false;
    }
    if (baseRegister < 0) {
        return false;
    }
    if (startRegister > static_cast<UINT>(baseRegister) ||
        startRegister + vector4fCount < static_cast<UINT>(baseRegister + rows)) {
        return false;
    }

    int offset = (baseRegister - static_cast<int>(startRegister)) * 4;
    const float* m = constantData + offset;
    D3DMATRIX out = {};

    if (!transposed) {
        out._11 = m[0]; out._12 = m[1]; out._13 = m[2]; out._14 = m[3];
        out._21 = m[4]; out._22 = m[5]; out._23 = m[6]; out._24 = m[7];
        out._31 = m[8]; out._32 = m[9]; out._33 = m[10]; out._34 = m[11];
        if (rows == 4) {
            out._41 = m[12]; out._42 = m[13]; out._43 = m[14]; out._44 = m[15];
        } else {
            out._41 = 0.0f; out._42 = 0.0f; out._43 = 0.0f; out._44 = 1.0f;
        }
    } else {
        out._11 = m[0]; out._21 = m[1]; out._31 = m[2]; out._41 = m[3];
        out._12 = m[4]; out._22 = m[5]; out._32 = m[6]; out._42 = m[7];
        out._13 = m[8]; out._23 = m[9]; out._33 = m[10]; out._43 = m[11];
        if (rows == 4) {
            out._14 = m[12]; out._24 = m[13]; out._34 = m[14]; out._44 = m[15];
        } else {
            out._14 = 0.0f; out._24 = 0.0f; out._34 = 0.0f; out._44 = 1.0f;
        }
    }

    *outMatrix = out;
    return true;
}

static bool TryBuildMatrixSnapshot(const ShaderConstantState& state,
                                  int baseRegister,
                                  int rows,
                                  bool transposed,
                                  D3DMATRIX* outMatrix) {
    if (!outMatrix || !state.snapshotReady || baseRegister < 0 || rows < 3 || rows > 4 ||
        baseRegister + rows - 1 >= kMaxConstantRegisters) {
        return false;
    }

    for (int i = 0; i < rows; i++) {
        if (!state.valid[baseRegister + i]) {
            return false;
        }
    }

    const float* m = &state.constants[baseRegister][0];
    D3DMATRIX out = {};

    if (!transposed) {
        out._11 = m[0]; out._12 = m[1]; out._13 = m[2]; out._14 = m[3];
        out._21 = m[4]; out._22 = m[5]; out._23 = m[6]; out._24 = m[7];
        out._31 = m[8]; out._32 = m[9]; out._33 = m[10]; out._34 = m[11];
        if (rows == 4) {
            out._41 = m[12]; out._42 = m[13]; out._43 = m[14]; out._44 = m[15];
        } else {
            out._41 = 0.0f; out._42 = 0.0f; out._43 = 0.0f; out._44 = 1.0f;
        }
    } else {
        out._11 = m[0]; out._21 = m[1]; out._31 = m[2]; out._41 = m[3];
        out._12 = m[4]; out._22 = m[5]; out._32 = m[6]; out._42 = m[7];
        out._13 = m[8]; out._23 = m[9]; out._33 = m[10]; out._43 = m[11];
        if (rows == 4) {
            out._14 = m[12]; out._24 = m[13]; out._34 = m[14]; out._44 = m[15];
        } else {
            out._14 = 0.0f; out._24 = 0.0f; out._34 = 0.0f; out._44 = 1.0f;
        }
    }

    *outMatrix = out;
    return true;
}

static bool TryBuildMatrixFromSnapshot(const ShaderConstantState& state, int baseRegister,
                                       D3DMATRIX* outMatrix) {
    return TryBuildMatrixSnapshot(state, baseRegister, 4, false, outMatrix);
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

struct CandidateScore {
    int base = -1;
    float score = 0.0f;
    float variance = 0.0f;
    int strategy = Layout_4x4;
    bool transposed = false;
    bool inverseView = false;
};

struct AutoCandidateKey {
    uintptr_t shaderKey = 0;
    int base = -1;
    int rows = 4;
    bool transposed = false;
    MatrixSlot slot = MatrixSlot_View;

    bool operator==(const AutoCandidateKey& other) const {
        return shaderKey == other.shaderKey &&
               base == other.base &&
               rows == other.rows &&
               transposed == other.transposed &&
               slot == other.slot;
    }
};

struct AutoCandidateKeyHasher {
    size_t operator()(const AutoCandidateKey& key) const {
        size_t h = static_cast<size_t>(key.shaderKey);
        h ^= static_cast<size_t>(key.base + 257) * 16777619u;
        h ^= static_cast<size_t>(key.rows + 31) * 1099511628211ull;
        h ^= static_cast<size_t>(key.transposed ? 0x9E3779B1u : 0x7F4A7C15u);
        h ^= static_cast<size_t>(key.slot) * 0x85EBCA6Bu;
        return h;
    }
};

struct AutoCandidateStats {
    AutoCandidateKey key = {};
    unsigned long long seenUpdates = 0;
    unsigned long long drawCalls = 0;
    unsigned long long framesWithDraw = 0;
    int framesSeen = 0;
    int bestConsecutiveFrames = 0;
    int consecutiveFrames = 0;
    int lastSeenFrame = -1000000;
    int lastDrawFrame = -1000000;
    float averageFov = 0.0f;
    int fovSamples = 0;
    float averageDelta = 0.0f;
    int deltaSamples = 0;
    int smoothTransitionCount = 0;
    D3DMATRIX lastMatrix = {};
    bool hasLastMatrix = false;
};

static std::unordered_map<AutoCandidateKey, AutoCandidateStats, AutoCandidateKeyHasher> g_autoCandidateStats = {};

static bool IsAutoDetectActive() {
    return g_config.autoDetectMatrices;
}

static void StartAutoDetectSampling() {
    g_autoCandidateStats.clear();
    g_autoDetectSamplingStartFrame = g_frameCount;
    g_autoDetectSamplingActive = true;
    g_autoDetectSamplingPausedByUser = false;
}

static void StopAutoDetectSampling() {
    g_autoDetectSamplingActive = false;
    g_autoDetectSamplingPausedByUser = true;
}

static void ResetAutoDetectToLegacyDefaults() {
    g_runtimeAutoDetectEnabled = false;
    g_autoApplyDetectedMatrices = false;
    g_autoDetectMode = AutoDetect_IndividualWVP;
    g_autoDetectMinFramesSeen = 12;
    g_autoDetectMinConsecutiveFrames = 4;
    g_autoDetectSamplingFrames = 180;
    g_layoutStrategyMode = Layout_Auto;
    g_probeTransposedLayouts = true;
    g_probeInverseView = true;
    g_autoPickCandidates = true;
    g_autoDetectSamplingStartFrame = -1;
    g_autoDetectSamplingPausedByUser = false;
    g_autoCandidateStats.clear();
    StopAutoDetectSampling();

    g_manualBindings[MatrixSlot_World].enabled = false;
    g_manualBindings[MatrixSlot_View].enabled = false;
    g_manualBindings[MatrixSlot_Projection].enabled = false;
    g_manualBindings[MatrixSlot_MVP].enabled = false;

    g_config.worldMatrixRegister = g_iniWorldMatrixRegister;
    g_config.viewMatrixRegister = g_iniViewMatrixRegister;
    g_config.projMatrixRegister = g_iniProjMatrixRegister;
}

static bool IsAutoDetectSamplingWindowOpen() {
    if (!g_autoDetectSamplingActive) {
        return false;
    }
    if (g_autoDetectSamplingFrames < 1) {
        g_autoDetectSamplingFrames = 1;
    }
    const int elapsed = g_frameCount - g_autoDetectSamplingStartFrame;
    if (elapsed >= g_autoDetectSamplingFrames) {
        g_autoDetectSamplingActive = false;
        return false;
    }
    return true;
}

static bool IsLikelyOrthographicProjection(const D3DMATRIX& m) {
    return fabsf(m._34) < 0.2f && fabsf(m._44 - 1.0f) < 0.2f;
}

static bool PassesPerspectiveHeuristics(const D3DMATRIX& m) {
    if (IsLikelyOrthographicProjection(m)) {
        return false;
    }
    if (fabsf(m._33) < 0.01f || fabsf(m._43) < 0.0001f) {
        return false;
    }
    float fov = ExtractFOV(m);
    if (!std::isfinite(fov)) {
        return false;
    }
    return fov >= g_config.minFOV && fov <= g_config.maxFOV;
}

static void ObserveAutoCandidate(const AutoCandidateKey& key, const D3DMATRIX& mat, float fovRadians = 0.0f) {
    AutoCandidateStats& stats = g_autoCandidateStats[key];
    stats.key = key;
    stats.seenUpdates++;
    if (stats.lastSeenFrame != g_frameCount) {
        stats.framesSeen++;
        if (stats.lastSeenFrame == g_frameCount - 1) {
            stats.consecutiveFrames++;
        } else {
            stats.consecutiveFrames = 1;
        }
        stats.bestConsecutiveFrames = (std::max)(stats.bestConsecutiveFrames, stats.consecutiveFrames);
        stats.lastSeenFrame = g_frameCount;
    }
    if (fovRadians > 0.0f && std::isfinite(fovRadians)) {
        stats.averageFov = (stats.averageFov * static_cast<float>(stats.fovSamples) + fovRadians) /
                           static_cast<float>(stats.fovSamples + 1);
        stats.fovSamples++;
    }
    if (stats.hasLastMatrix) {
        const float* prev = reinterpret_cast<const float*>(&stats.lastMatrix);
        const float* curr = reinterpret_cast<const float*>(&mat);
        float avgAbsDelta = 0.0f;
        for (int i = 0; i < 16; i++) {
            avgAbsDelta += fabsf(curr[i] - prev[i]);
        }
        avgAbsDelta /= 16.0f;
        stats.averageDelta = (stats.averageDelta * static_cast<float>(stats.deltaSamples) + avgAbsDelta) /
                             static_cast<float>(stats.deltaSamples + 1);
        stats.deltaSamples++;
        if (avgAbsDelta < 0.15f) {
            stats.smoothTransitionCount++;
        }
    }
    stats.lastMatrix = mat;
    stats.hasLastMatrix = true;
}

static void RegisterAutoCandidateDraw(const AutoCandidateKey& key) {
    auto it = g_autoCandidateStats.find(key);
    if (it == g_autoCandidateStats.end()) {
        return;
    }
    AutoCandidateStats& stats = it->second;
    stats.drawCalls++;
    if (stats.lastDrawFrame != g_frameCount) {
        stats.framesWithDraw++;
        stats.lastDrawFrame = g_frameCount;
    }
}

static bool PassesTemporalStability(const AutoCandidateStats& stats) {
    return stats.framesSeen >= g_autoDetectMinFramesSeen &&
           stats.bestConsecutiveFrames >= g_autoDetectMinConsecutiveFrames;
}

static float ComputeAutoCandidateScore(const AutoCandidateStats& stats) {
    if (!PassesTemporalStability(stats)) {
        return -1.0f;
    }
    const float smoothnessBonus = static_cast<float>(stats.smoothTransitionCount) * 0.5f;
    const float deltaPenalty = stats.deltaSamples > 0 ? stats.averageDelta * 2.0f : 0.0f;
    return static_cast<float>(stats.drawCalls) * 2.2f +
           static_cast<float>(stats.framesWithDraw) * 1.0f +
           static_cast<float>(stats.bestConsecutiveFrames) * 0.9f +
           static_cast<float>(stats.seenUpdates) * 0.04f +
           smoothnessBonus - deltaPenalty;
}

static bool FindBestAutoCandidate(MatrixSlot slot, AutoCandidateStats* outStats) {
    if (!outStats) {
        return false;
    }
    float bestScore = -1.0f;
    bool found = false;
    for (const auto& entry : g_autoCandidateStats) {
        const AutoCandidateStats& stats = entry.second;
        if (stats.key.slot != slot || !stats.hasLastMatrix) {
            continue;
        }
        float score = ComputeAutoCandidateScore(stats);
        if (score > bestScore) {
            bestScore = score;
            *outStats = stats;
            found = true;
        }
    }
    return found;
}

void CreateProjectionMatrix(D3DMATRIX* out, float fovY, float aspect, float zNear, float zFar);
void ExtractCameraFromMVP(const D3DMATRIX& mvp, D3DMATRIX* viewOut);

static bool ApplyAutoCandidateToLegacyDetection(const AutoCandidateStats& best) {
    if (!best.hasLastMatrix || best.key.base < 0) {
        return false;
    }

    switch (best.key.slot) {
        case MatrixSlot_World:
            g_config.worldMatrixRegister = best.key.base;
            g_manualBindings[MatrixSlot_World].enabled = false;
            StoreWorldMatrix(best.lastMatrix, best.key.shaderKey, best.key.base, best.key.rows,
                             best.key.transposed, false);
            break;
        case MatrixSlot_View:
            g_config.viewMatrixRegister = best.key.base;
            g_manualBindings[MatrixSlot_View].enabled = false;
            StoreViewMatrix(best.lastMatrix, best.key.shaderKey, best.key.base, best.key.rows,
                            best.key.transposed, false);
            break;
        case MatrixSlot_Projection:
            g_config.projMatrixRegister = best.key.base;
            g_manualBindings[MatrixSlot_Projection].enabled = false;
            StoreProjectionMatrix(best.lastMatrix, best.key.shaderKey, best.key.base, best.key.rows,
                                  best.key.transposed, false);
            break;
        case MatrixSlot_MVP: {
            StoreMVPMatrix(best.lastMatrix, best.key.shaderKey, best.key.base, best.key.rows,
                           best.key.transposed, false);
            D3DMATRIX extractedView = {};
            D3DMATRIX extractedProj = {};
            ExtractCameraFromMVP(best.lastMatrix, &extractedView);
            CreateProjectionMatrix(&extractedProj, 1.047f, 16.0f / 9.0f, 0.1f, 10000.0f);
            StoreViewMatrix(extractedView, best.key.shaderKey, best.key.base, best.key.rows,
                            best.key.transposed, false);
            StoreProjectionMatrix(extractedProj, best.key.shaderKey, best.key.base, best.key.rows,
                                  best.key.transposed, false);
            break;
        }
        default:
            return false;
    }
    return true;
}

static float ComputeProjectionLikeScore(const D3DMATRIX& m) {
    float score = 0.0f;
    if (fabsf(m._34 - 1.0f) < 0.1f) score += 0.7f;
    if (fabsf(m._44) < 0.1f) score += 0.5f;
    if (fabsf(m._11) > 0.01f && fabsf(m._22) > 0.01f) score += 0.4f;
    return score;
}

static void CollectTopCandidates(const ShaderConstantState& state,
                                 std::vector<CandidateScore>* outViewScores,
                                 std::vector<CandidateScore>* outProjScores) {
    if (!outViewScores || !outProjScores) {
        return;
    }
    outViewScores->clear();
    outProjScores->clear();

    for (int base = 0; base < kMaxConstantRegisters; base++) {
        // 4x4 strategy
        if (base + 3 < kMaxConstantRegisters) {
            D3DMATRIX mat = {};
            bool looksLike = false;
            bool hasMatrix = TryBuildMatrixSnapshotInfo(state, base, &mat, &looksLike);
            if (hasMatrix && looksLike) {
                float variance = 0.0f;
                for (int reg = base; reg < base + 4; reg++) {
                    variance += GetVarianceMagnitude(state, reg);
                }
                variance /= 4.0f;
                float varianceScore = variance <= g_config.varianceThreshold ? 1.0f : 0.0f;

                if (LooksLikeViewStrict(mat)) {
                    outViewScores->push_back({base, 1.0f + varianceScore, variance, Layout_4x4, false, false});
                    if (g_probeInverseView) {
                        D3DMATRIX inv = InvertSimpleRigidView(mat);
                        if (LooksLikeViewStrict(inv)) {
                            outViewScores->push_back({base, 0.9f + varianceScore, variance, Layout_4x4, false, true});
                        }
                    }
                }
                if (LooksLikeProjectionStrict(mat)) {
                    outProjScores->push_back({base, 1.0f + varianceScore, variance, Layout_4x4, false, false});
                }

                if (g_probeTransposedLayouts) {
                    D3DMATRIX t = TransposeMatrix(mat);
                    if (LooksLikeViewStrict(t)) {
                        outViewScores->push_back({base, 0.95f + varianceScore, variance, Layout_4x4, true, false});
                    }
                    if (LooksLikeProjectionStrict(t)) {
                        outProjScores->push_back({base, 0.95f + varianceScore, variance, Layout_4x4, true, false});
                    }
                }

                // VP/WVP heuristics
                float vpScore = ComputeProjectionLikeScore(mat);
                if (vpScore > 0.0f) {
                    outProjScores->push_back({base, 0.5f + vpScore + varianceScore * 0.5f, variance, Layout_VP, false, false});
                }
            }
        }

        // 4x3 strategy
        if (base + 2 < kMaxConstantRegisters) {
            for (int transposed = 0; transposed < (g_probeTransposedLayouts ? 2 : 1); transposed++) {
                D3DMATRIX m43 = {};
                if (!TryBuildMatrix4x3FromSnapshot(state, base, transposed != 0, &m43)) {
                    continue;
                }
                float variance = 0.0f;
                for (int reg = base; reg < base + 3; reg++) {
                    variance += GetVarianceMagnitude(state, reg);
                }
                variance /= 3.0f;
                float varianceScore = variance <= g_config.varianceThreshold ? 1.0f : 0.0f;

                if (LooksLikeViewStrict(m43)) {
                    outViewScores->push_back({base, 0.95f + varianceScore, variance, Layout_4x3, transposed != 0, false});
                }
            }
        }
    }

    auto sortScores = [](std::vector<CandidateScore>& scores) {
        std::sort(scores.begin(), scores.end(),
                  [](const CandidateScore& a, const CandidateScore& b) {
                      if (a.score != b.score) {
                          return a.score > b.score;
                      }
                      if (a.variance != b.variance) {
                          return a.variance < b.variance;
                      }
                      return a.base < b.base;
                  });
    };
    sortScores(*outViewScores);
    sortScores(*outProjScores);
}

static void LogTopCandidates(const ShaderConstantState& state, uintptr_t shaderKey) {
    std::vector<CandidateScore> viewScores;
    std::vector<CandidateScore> projScores;
    CollectTopCandidates(state, &viewScores, &projScores);

    LogMsg("Candidate summary for shader 0x%p", reinterpret_cast<void*>(shaderKey));
    int count = 0;
    for (const auto& entry : viewScores) {
        LogMsg("  VIEW c%d-c%d score %.2f variance %.6f strategy %d transposed %d inverse %d", entry.base, entry.base + 3,
               entry.score, entry.variance, entry.strategy, entry.transposed ? 1 : 0, entry.inverseView ? 1 : 0);
        if (++count >= g_config.topCandidateCount) {
            break;
        }
    }
    count = 0;
    for (const auto& entry : projScores) {
        LogMsg("  PROJ c%d-c%d score %.2f variance %.6f strategy %d transposed %d", entry.base, entry.base + 3,
               entry.score, entry.variance, entry.strategy, entry.transposed ? 1 : 0);
        if (++count >= g_config.topCandidateCount) {
            break;
        }
    }
}

static void UpdateStability(ShaderConstantState& state, int base, bool isView, bool isProj) {
    if (isView) {
        if (state.stableViewBase == base) {
            state.stableViewCount++;
        } else {
            state.stableViewBase = base;
            state.stableViewCount = 1;
        }
        if (state.stableViewCount == g_config.stabilityFrames) {
            LogMsg("Stable VIEW candidate: c%d-c%d", base, base + 3);
        }
    }
    if (isProj) {
        if (state.stableProjBase == base) {
            state.stableProjCount++;
        } else {
            state.stableProjBase = base;
            state.stableProjCount = 1;
        }
        if (state.stableProjCount == g_config.stabilityFrames) {
            LogMsg("Stable PROJ candidate: c%d-c%d", base, base + 3);
        }
    }
}

static bool TryPromoteStableAutoCandidate(ShaderConstantState& state,
                                          MatrixSlot slot,
                                          uintptr_t shaderKey,
                                          int base,
                                          int rows,
                                          bool transposed,
                                          const D3DMATRIX& mat) {
    if (!IsAutoDetectActive()) {
        return false;
    }

    const bool viewStable = slot == MatrixSlot_View &&
                            state.stableViewBase == base &&
                            state.stableViewCount >= g_config.stabilityFrames;
    const bool projStable = slot == MatrixSlot_Projection &&
                            state.stableProjBase == base &&
                            state.stableProjCount >= g_config.stabilityFrames;

    if (!viewStable && !projStable) {
        return false;
    }

    uint32_t shaderHash = 0;
    const bool hasShaderHash = TryGetShaderBytecodeHash(shaderKey, &shaderHash);
    HeuristicProfile* profile = nullptr;
    if (hasShaderHash) {
        profile = &g_heuristicProfiles[shaderHash];
        if (profile->valid) {
            if (slot == MatrixSlot_View && profile->viewBase >= 0 && profile->viewBase != base) {
                return false;
            }
            if (slot == MatrixSlot_Projection && profile->projBase >= 0 && profile->projBase != base) {
                return false;
            }
        }
    }

    if (slot == MatrixSlot_View && g_config.viewMatrixRegister != base) {
        g_config.viewMatrixRegister = base;
        g_manualBindings[MatrixSlot_View].enabled = false;
        StoreViewMatrix(mat, shaderKey, base, rows, transposed, false);
        LogMsg("AutoDetect refined: promoted stable VIEW matrix c%d-c%d (rows=%d transpose=%d)",
               base, base + rows - 1, rows, transposed ? 1 : 0);
        if (profile) {
            profile->valid = true;
            profile->viewBase = base;
            profile->layoutMode = g_layoutStrategyMode;
            profile->transposed = g_probeTransposedLayouts;
            profile->inverseView = g_probeInverseView;
            SaveHeuristicProfile(shaderHash, *profile);
        }
        return true;
    }
    if (slot == MatrixSlot_Projection && g_config.projMatrixRegister != base) {
        g_config.projMatrixRegister = base;
        g_manualBindings[MatrixSlot_Projection].enabled = false;
        StoreProjectionMatrix(mat, shaderKey, base, rows, transposed, false);
        float fov = ExtractFOV(mat) * 180.0f / 3.14159f;
        LogMsg("AutoDetect refined: promoted stable PROJECTION matrix c%d-c%d (rows=%d transpose=%d FOV %.1f)",
               base, base + rows - 1, rows, transposed ? 1 : 0, fov);
        if (profile) {
            profile->valid = true;
            profile->projBase = base;
            profile->layoutMode = g_layoutStrategyMode;
            profile->transposed = g_probeTransposedLayouts;
            profile->inverseView = g_probeInverseView;
            SaveHeuristicProfile(shaderHash, *profile);
        }
        return true;
    }
    return false;
}


static void ScanBuffer(const void* base, size_t size, int& resultsFound) {
    if (!base || size < sizeof(D3DMATRIX)) {
        return;
    }
    const float* data = reinterpret_cast<const float*>(base);
    size_t count = size / sizeof(float);
    for (size_t i = 0; i + 16 <= count; i++) {
        const float* window = data + i;
        if (!LooksLikeMatrix(window)) {
            continue;
        }
        D3DMATRIX mat = {};
        memcpy(&mat, window, sizeof(D3DMATRIX));
        bool looksView = LooksLikeViewStrict(mat);
        bool looksProj = LooksLikeProjectionStrict(mat);
        if (!looksView && !looksProj) {
            continue;
        }
        uint32_t hash = HashMatrix(mat);
        char resultLine[256];
        snprintf(resultLine, sizeof(resultLine), "Memory scan: %s matrix at %p hash 0x%08X",
                 looksView ? "VIEW" : "PROJ", reinterpret_cast<const void*>(window), hash);
        LogMsg("%s", resultLine);
        {
            std::lock_guard<std::mutex> lock(g_uiDataMutex);
            g_memoryScanResults.emplace_back(resultLine);
            MemoryScanHit hit = {};
            hit.label = resultLine;
            hit.matrix = mat;
            hit.slot = looksView ? MatrixSlot_View : MatrixSlot_Projection;
            hit.address = reinterpret_cast<uintptr_t>(window);
            hit.hash = hash;
            g_memoryScanHits.emplace_back(hit);
        }
        resultsFound++;
        if (resultsFound >= g_config.memoryScannerMaxResults) {
            return;
        }
    }
}

static DWORD WINAPI MemoryScannerThread(LPVOID lpParam) {
    std::string moduleName = lpParam ? static_cast<const char*>(lpParam) : "";
    if (lpParam) {
        free(lpParam);
    }
    HMODULE hmod = moduleName.empty() ? GetModuleHandleA(nullptr)
                                      : GetModuleHandleA(moduleName.c_str());
    if (!hmod) {
        LogMsg("Memory scan failed: module not found (%s)", moduleName.c_str());
        g_memoryScannerThread = nullptr;
        return 0;
    }

    MEMORY_BASIC_INFORMATION info = {};
    SIZE_T len = VirtualQuery(hmod, &info, sizeof(info));
    if (len == 0) {
        LogMsg("Memory scan failed: VirtualQuery base.");
        g_memoryScannerThread = nullptr;
        return 0;
    }

    BYTE* dllBase = static_cast<BYTE*>(info.AllocationBase);
    BYTE* address = dllBase;
    int resultsFound = 0;
    while (true) {
        len = VirtualQuery(address, &info, sizeof(info));
        if (len == 0) {
            break;
        }
        if (info.AllocationBase != dllBase) {
            break;
        }
        if (((info.Protect & PAGE_EXECUTE_READWRITE) || (info.Protect & PAGE_READWRITE)) &&
            !(info.Protect & PAGE_GUARD)) {
            ScanBuffer(info.BaseAddress, info.RegionSize, resultsFound);
            if (resultsFound >= g_config.memoryScannerMaxResults) {
                break;
            }
        }
        address = static_cast<BYTE*>(info.BaseAddress) + info.RegionSize;
    }

    LogMsg("Memory scan complete: %d results", resultsFound);
    g_memoryScannerThread = nullptr;
    return 0;
}

static void StartMemoryScanner() {
    if (g_memoryScannerThread) {
        return;
    }
    const char* moduleName = g_config.memoryScannerModule[0] ? g_config.memoryScannerModule : nullptr;
    {
        std::lock_guard<std::mutex> lock(g_uiDataMutex);
        g_memoryScanResults.clear();
        g_memoryScanHits.clear();
    }
    char* moduleCopy = nullptr;
    if (moduleName) {
        moduleCopy = _strdup(moduleName);
    }
    g_memoryScannerThread = CreateThread(
        nullptr,
        0,
        MemoryScannerThread,
        moduleCopy,
        0,
        &g_memoryScannerThreadId);
    if (!g_memoryScannerThread) {
        LogMsg("WARNING: Failed to create memory scan thread.");
        if (moduleCopy) {
            free(moduleCopy);
        }
    }
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
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::NewFrame();

    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("Camera Proxy for RTX Remix", nullptr,
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Toggle menu: Alt+M");
    if (ImGui::Button(g_pauseRendering ? "Resume game rendering" : "Pause game rendering")) {
        g_pauseRendering = !g_pauseRendering;
    }
    ImGui::SameLine();
    ImGui::Text("Status: %s", g_pauseRendering ? "Paused" : "Running");
    ImGui::TextWrapped("This proxy detects World, View, and Projection matrices from shader constants and forwards them "
                       "to the RTX Remix runtime through SetTransform() so Remix gets camera data in D3D9 titles.");

    if (ImGui::Button("Pass camera matrices to RTX Remix (SetTransform)")) {
        if (!g_config.emitFixedFunctionTransforms) {
            snprintf(g_manualEmitStatus, sizeof(g_manualEmitStatus),
                     "Blocked: set EmitFixedFunctionTransforms=1 in camera_proxy.ini first.");
        } else {
            g_requestManualEmit = true;
            snprintf(g_manualEmitStatus, sizeof(g_manualEmitStatus),
                     "Pending: pass cached World/View/Projection matrices to RTX Remix this frame.");
        }
    }
    if (g_manualEmitStatus[0] != '\0') {
        ImGui::TextWrapped("%s", g_manualEmitStatus);
    }

    ImGui::Checkbox("Show FPS stats", &g_showFpsStats);
    ImGui::Checkbox("Show transposed matrices", &g_showTransposedMatrices);
    if (g_showFpsStats && g_frameTimeSamples > 0) {
        double sumMs = 0.0;
        for (int i = 0; i < g_frameTimeCount; i++) {
            float ms = g_frameTimeHistory[i];
            sumMs += ms;
        }
        float avgMs = g_frameTimeCount > 0 ? static_cast<float>(sumMs / g_frameTimeCount) : 0.0f;
        float avgFps = avgMs > 0.0f ? 1000.0f / avgMs : 0.0f;
        float graphMaxMs = (std::max)(33.0f, avgMs * 1.75f);
        ImGui::Text("FPS: %.1f", avgFps);
        ImGui::Text("ms: %.2f", avgMs);
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_PlotLinesHovered, ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
        ImGui::PlotLines("Frame time (ms)", g_frameTimeHistory, g_frameTimeCount, g_frameTimeIndex,
                         nullptr, 0.0f, graphMaxMs,
                         ImVec2(0, 80));
        ImGui::PopStyleColor(2);
    }

    ImGui::Separator();
    ImGui::Text("Credits: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.78f, 0.34f, 0.34f, 1.0f), "Overseer");
    ImGui::SameLine();
    ImGui::Text("- https://github.com/mencelot/dmc4-camera-proxy");
    ImGui::Text("modified by ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.78f, 0.34f, 0.34f, 1.0f), "cobalticarus92");

    ImGui::Separator();
    if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Camera")) {
            DrawMatrixWithTranspose("World", g_cameraMatrices.world, g_cameraMatrices.hasWorld,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_World, g_cameraMatrices.hasWorld);
            ImGui::Separator();
            DrawMatrixWithTranspose("View", g_cameraMatrices.view, g_cameraMatrices.hasView,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_View, g_cameraMatrices.hasView);
            ImGui::Separator();
            DrawMatrixWithTranspose("Projection", g_cameraMatrices.projection,
                                    g_cameraMatrices.hasProjection, g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_Projection, g_cameraMatrices.hasProjection);
            ImGui::Separator();
            DrawMatrixWithTranspose("MVP", g_cameraMatrices.mvp, g_cameraMatrices.hasMVP,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_MVP, g_cameraMatrices.hasMVP);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Constants")) {
            ImGui::Text("Per-shader snapshots update every frame.");
            if (g_selectedShaderKey == 0) {
                if (g_activeShaderKey != 0) {
                    g_selectedShaderKey = g_activeShaderKey;
                } else if (!g_shaderOrder.empty()) {
                    g_selectedShaderKey = g_shaderOrder.front();
                }
            }
            if (!g_shaderOrder.empty()) {
                char preview[128];
                BuildShaderComboLabel(g_selectedShaderKey, preview, sizeof(preview));
                if (ImGui::BeginCombo("Shader", preview)) {
                    for (uintptr_t key : g_shaderOrder) {
                        char itemLabel[128];
                        BuildShaderComboLabel(key, itemLabel, sizeof(itemLabel));
                        ShaderConstantState* itemState = GetShaderState(key, false);
                        float flashStrength = GetShaderFlashStrength(itemState);
                        if (flashStrength > 0.0f) {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                                  ImVec4(1.00f,
                                                         0.35f + 0.45f * flashStrength,
                                                         0.35f + 0.45f * flashStrength,
                                                         1.00f));
                        }
                        bool selected = (key == g_selectedShaderKey);
                        if (ImGui::Selectable(itemLabel, selected)) {
                            g_selectedShaderKey = key;
                            g_selectedRegister = -1;
                        }
                        if (flashStrength > 0.0f) {
                            ImGui::PopStyleColor();
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                bool disableSelected = IsShaderDisabled(g_selectedShaderKey);
                if (ImGui::Checkbox("Disable shader draws", &disableSelected)) {
                    SetShaderDisabled(g_selectedShaderKey, disableSelected);
                }
            } else {
                ImGui::Text("<no shader constants captured yet>");
            }

            ImGui::Checkbox("Group by 4-register matrices", &g_showConstantsAsMatrices);
            ImGui::SameLine();
            ImGui::Checkbox("Only show detected matrices", &g_filterDetectedMatrices);
            ImGui::Text("Manual matrix range");
            ImGui::SameLine();
            ImGui::RadioButton("4 registers", &g_manualAssignRows, 4);
            ImGui::SameLine();
            ImGui::RadioButton("3 registers", &g_manualAssignRows, 3);
            if (g_matrixAssignStatus[0] != '\0') {
                ImGui::TextWrapped("%s", g_matrixAssignStatus);
            }

            ImGui::Separator();
            ImGui::Checkbox("Enable shader constant editing", &g_enableShaderEditing);
            ImGui::SameLine();
            if (ImGui::Button("Reset all overrides")) {
                ClearAllShaderOverrides();
            }
            static const char* kOverrideModes[] = {"Sticky", "One-frame", "N-frames"};
            ImGui::Combo("Override scope", &g_overrideScopeMode, kOverrideModes, IM_ARRAYSIZE(kOverrideModes));
            if (g_overrideScopeMode == Override_NFrames) {
                ImGui::InputInt("Override frames", &g_overrideNFrames);
                if (g_overrideNFrames < 1) g_overrideNFrames = 1;
            }

            ShaderConstantState* editState = GetShaderState(g_selectedShaderKey, false);
            if (g_selectedRegister >= 0) {
                ImGui::Text("Selected register: c%d", g_selectedRegister);
                if (editState && g_selectedRegister < kMaxConstantRegisters) {
                    float editValues[4] = {};
                    if (editState->overrideValid[g_selectedRegister]) {
                        memcpy(editValues, editState->overrideConstants[g_selectedRegister], sizeof(editValues));
                    } else if (editState->valid[g_selectedRegister]) {
                        memcpy(editValues, editState->constants[g_selectedRegister], sizeof(editValues));
                    }

                    if (ImGui::InputFloat4("Override values", editValues, "%.6f")) {
                        memcpy(editState->overrideConstants[g_selectedRegister], editValues,
                               sizeof(editState->overrideConstants[g_selectedRegister]));
                        editState->overrideValid[g_selectedRegister] = true;
                        if (g_overrideScopeMode == Override_OneFrame) {
                            editState->overrideFramesRemaining[g_selectedRegister] = 1;
                        } else if (g_overrideScopeMode == Override_NFrames) {
                            editState->overrideFramesRemaining[g_selectedRegister] = g_overrideNFrames;
                        } else {
                            editState->overrideFramesRemaining[g_selectedRegister] = -1;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset selected override")) {
                        ClearShaderRegisterOverride(g_selectedShaderKey, g_selectedRegister);
                    }
                    if (!g_enableShaderEditing) {
                        ImGui::TextDisabled("Editing is armed but inactive until enabled.");
                    }
                }
            }

            ImGui::BeginChild("ConstantsScroll", ImVec2(0, 270), true);
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
                                        snprintf(rowLabel, sizeof(rowLabel), "r%d: [%.3f %.3f %.3f %.3f]###reg_%d",
                                                 row, data[0], data[1], data[2], data[3], reg);
                                    } else {
                                        const float* data = state->constants[reg];
                                        snprintf(rowLabel, sizeof(rowLabel), "c%d: [%.3f %.3f %.3f %.3f]###reg_%d",
                                                 reg, data[0], data[1], data[2], data[3], reg);
                                    }
                                } else {
                                    snprintf(rowLabel, sizeof(rowLabel), "c%d: <unset>###reg_%d", reg, reg);
                                }
                                bool selected = (g_selectedRegister == reg);
                                ImGui::PushID(reg);
                                if (ImGui::Selectable(rowLabel, selected)) {
                                    g_selectedRegister = reg;
                                }
                                ImGui::PopID();
                            }

                            int selectedRows = g_manualAssignRows;
                            if (selectedRows == 3 && !state->valid[base + 2]) {
                                selectedRows = 4;
                            }
                            bool canAssign = state->valid[base] && state->valid[base + 1] && state->valid[base + 2] &&
                                             (selectedRows == 3 || state->valid[base + 3]);
                            if (canAssign) {
                                D3DMATRIX assignedMat = {};
                                if (!TryBuildMatrixSnapshot(*state, base, selectedRows, false, &assignedMat)) {
                                    canAssign = false;
                                }
                                if (canAssign) {
                                    ImGui::PushID(base);
                                    if (ImGui::Button("Use as World")) {
                                        TryAssignManualMatrixFromSelection(MatrixSlot_World, g_selectedShaderKey,
                                                                           base, selectedRows, assignedMat);
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button("Use as View")) {
                                        TryAssignManualMatrixFromSelection(MatrixSlot_View, g_selectedShaderKey,
                                                                           base, selectedRows, assignedMat);
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button("Use as Projection")) {
                                        TryAssignManualMatrixFromSelection(MatrixSlot_Projection, g_selectedShaderKey,
                                                                           base, selectedRows, assignedMat);
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button("Use as MVP")) {
                                        TryAssignManualMatrixFromSelection(MatrixSlot_MVP, g_selectedShaderKey,
                                                                           base, selectedRows, assignedMat);
                                    }
                                    ImGui::PopID();
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
                        snprintf(rowLabel, sizeof(rowLabel), "c%d: [%.3f %.3f %.3f %.3f]###reg_%d",
                                 reg, data[0], data[1], data[2], data[3], reg);
                        ImGui::PushID(reg);
                        if (ImGui::Selectable(rowLabel, g_selectedRegister == reg)) {
                            g_selectedRegister = reg;
                        }
                        ImGui::PopID();
                    }
                }
            } else {
                ImGui::Text("<no constants captured yet>");
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Heuristics")) {
            ImGui::Text("Refined legacy matrix auto-detection (deterministic)");
            ImGui::Text("AutoDetectMatrices (camera_proxy.ini): %s", g_config.autoDetectMatrices ? "ON" : "OFF");
            ImGui::TextWrapped("This tab is runtime heuristics only. To enable/disable auto-detect, keep using AutoDetectMatrices in camera_proxy.ini.");
            static const char* kLayoutModes[] = {"Auto", "4x4", "4x3", "VP", "MVP"};
            ImGui::Combo("Layout strategy", &g_layoutStrategyMode, kLayoutModes, IM_ARRAYSIZE(kLayoutModes));
            ImGui::Checkbox("Probe transposed layouts", &g_probeTransposedLayouts);
            ImGui::Checkbox("Probe inverse-view candidates", &g_probeInverseView);
            ImGui::InputInt("Stability frames", &g_config.stabilityFrames);
            if (g_config.stabilityFrames < 1) g_config.stabilityFrames = 1;

            ImGui::Separator();
            ImGui::TextWrapped("Stable candidates are pinned by shader hash once promoted, preventing register hopping between unrelated transform sets.");

            ShaderConstantState* state = GetShaderState(g_selectedShaderKey, false);
            if (state) {
                ImGui::Text("Stable VIEW candidate: %s", state->stableViewBase >= 0 ? "yes" : "no");
                if (state->stableViewBase >= 0) {
                    ImGui::Text("  c%d (count=%d)", state->stableViewBase, state->stableViewCount);
                }
                ImGui::Text("Stable PROJECTION candidate: %s", state->stableProjBase >= 0 ? "yes" : "no");
                if (state->stableProjBase >= 0) {
                    ImGui::Text("  c%d (count=%d)", state->stableProjBase, state->stableProjCount);
                }
            }

            uint32_t selectedShaderHash = 0;
            if (TryGetShaderBytecodeHash(g_selectedShaderKey, &selectedShaderHash)) {
                HeuristicProfile* profile = nullptr;
                auto profileIt = g_heuristicProfiles.find(selectedShaderHash);
                if (profileIt != g_heuristicProfiles.end()) {
                    profile = &profileIt->second;
                }
                const bool hasProfile = profile && profile->valid;
                ImGui::Text("Selected shader hash: 0x%08X", selectedShaderHash);
                ImGui::Text("Pinned VIEW: %s", (hasProfile && profile->viewBase >= 0) ? "yes" : "no");
                if (hasProfile && profile->viewBase >= 0) {
                    ImGui::SameLine();
                    ImGui::Text("c%d", profile->viewBase);
                }
                ImGui::Text("Pinned PROJECTION: %s", (hasProfile && profile->projBase >= 0) ? "yes" : "no");
                if (hasProfile && profile->projBase >= 0) {
                    ImGui::SameLine();
                    ImGui::Text("c%d", profile->projBase);
                }
                if (ImGui::Button("Pin current registers to selected shader")) {
                    HeuristicProfile& pinned = g_heuristicProfiles[selectedShaderHash];
                    pinned.valid = true;
                    pinned.viewBase = g_config.viewMatrixRegister;
                    pinned.projBase = g_config.projMatrixRegister;
                    pinned.layoutMode = g_layoutStrategyMode;
                    pinned.transposed = g_probeTransposedLayouts;
                    pinned.inverseView = g_probeInverseView;
                    SaveHeuristicProfile(selectedShaderHash, pinned);
                    LogMsg("Pinned matrix registers for shader hash 0x%08X: view=c%d proj=c%d",
                           selectedShaderHash, pinned.viewBase, pinned.projBase);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear selected shader pin")) {
                    HeuristicProfile cleared = {};
                    SaveHeuristicProfile(selectedShaderHash, cleared);
                    g_heuristicProfiles.erase(selectedShaderHash);
                    LogMsg("Cleared heuristic pin for shader hash 0x%08X", selectedShaderHash);
                }
            } else {
                ImGui::Text("Selected shader hash: <not available yet>");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Memory Scanner")) {
            if (ImGui::Button("Start memory scan")) {
                StartMemoryScanner();
            }
            ImGui::SameLine();
            ImGui::Text("Status: %s", g_memoryScannerThread ? "running" : "idle");
            ImGui::SameLine();
            if (ImGui::Button("Clear results")) {
                std::lock_guard<std::mutex> lock(g_uiDataMutex);
                g_memoryScanResults.clear();
                g_memoryScanHits.clear();
            }
            ImGui::Separator();
            ImGui::Text("Memory scan output");
            ImGui::BeginChild("MemoryScanResults", ImVec2(0, 360), true);
            {
                std::lock_guard<std::mutex> lock(g_uiDataMutex);
                if (g_memoryScanHits.empty()) {
                    ImGui::Text("<no scan results>");
                } else {
                    for (size_t i = 0; i < g_memoryScanHits.size(); ++i) {
                        const MemoryScanHit& hit = g_memoryScanHits[i];
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TextWrapped("%s", hit.label.c_str());
                        if (ImGui::Button("Use as View")) {
                            StoreViewMatrix(hit.matrix, 0, -1, 4, false, true);
                            snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                                     "Assigned VIEW from memory scan @ 0x%p (hash 0x%08X).",
                                     reinterpret_cast<void*>(hit.address), hit.hash);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Use as Projection")) {
                            StoreProjectionMatrix(hit.matrix, 0, -1, 4, false, true);
                            snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                                     "Assigned PROJECTION from memory scan @ 0x%p (hash 0x%08X).",
                                     reinterpret_cast<void*>(hit.address), hit.hash);
                        }
                        ImGui::PopID();
                        ImGui::Separator();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Logs")) {
            ImGui::Checkbox("Live update", &g_logsLiveUpdate);
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                RefreshLogSnapshot();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear logs")) {
                std::lock_guard<std::mutex> lock(g_uiDataMutex);
                g_logLines.clear();
                g_logSnapshot.clear();
                g_logSnapshotDirty = false;
            }
            if (g_logsLiveUpdate && g_logSnapshotDirty) {
                RefreshLogSnapshot();
            } else if (!g_logsLiveUpdate && g_logSnapshot.empty() && g_logSnapshotDirty) {
                RefreshLogSnapshot();
            }
            ImGui::Separator();
            ImGui::BeginChild("FormattedLogs", ImVec2(0, 380), true);
            if (g_logSnapshot.empty()) {
                ImGui::Text("<no logs>");
            } else {
                for (const std::string& line : g_logSnapshot) {
                    ImGui::TextWrapped("%s", line.c_str());
                }
                if (g_logsLiveUpdate) {
                    ImGui::SetScrollHereY(1.0f);
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();

    ImGui::EndFrame();
    ImGui::Render();
    g_isRenderingImGui = true;
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    g_isRenderingImGui = false;
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

static void AppendUiLogLine(const char* text) {
    if (!text || !text[0]) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_uiDataMutex);
    g_logLines.emplace_back(text);
    g_logSnapshotDirty = true;
    while (g_logLines.size() > kMaxUiLogLines) {
        g_logLines.pop_front();
    }
}

// Logging helper
void LogMsg(const char* fmt, ...) {
    char line[2048] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    AppendUiLogLine(line);

    if (!g_config.enableLogging || !g_logFile) return;

    fprintf(g_logFile, "%s\n", line);
    fflush(g_logFile);
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

static float Dot3(float ax, float ay, float az, float bx, float by, float bz) {
    return ax * bx + ay * by + az * bz;
}

static float Determinant3x3(const D3DMATRIX& m) {
    return m._11 * (m._22 * m._33 - m._23 * m._32) -
           m._12 * (m._21 * m._33 - m._23 * m._31) +
           m._13 * (m._21 * m._32 - m._22 * m._31);
}

static bool LooksLikeViewStrict(const D3DMATRIX& m) {
    float row0len = sqrtf(Dot3(m._11, m._12, m._13, m._11, m._12, m._13));
    float row1len = sqrtf(Dot3(m._21, m._22, m._23, m._21, m._22, m._23));
    float row2len = sqrtf(Dot3(m._31, m._32, m._33, m._31, m._32, m._33));

    if (fabsf(row0len - 1.0f) > 0.05f) return false;
    if (fabsf(row1len - 1.0f) > 0.05f) return false;
    if (fabsf(row2len - 1.0f) > 0.05f) return false;

    if (fabsf(Dot3(m._11, m._12, m._13, m._21, m._22, m._23)) > 0.05f) return false;
    if (fabsf(Dot3(m._11, m._12, m._13, m._31, m._32, m._33)) > 0.05f) return false;
    if (fabsf(Dot3(m._21, m._22, m._23, m._31, m._32, m._33)) > 0.05f) return false;

    if (fabsf(m._14) > 0.01f || fabsf(m._24) > 0.01f || fabsf(m._34) > 0.01f) return false;
    if (fabsf(m._44 - 1.0f) > 0.01f) return false;

    float det = Determinant3x3(m);
    if (fabsf(det - 1.0f) > 0.1f) return false;

    return true;
}

static bool LooksLikeProjectionStrict(const D3DMATRIX& m) {
    if (fabsf(m._12) > 0.01f || fabsf(m._13) > 0.01f || fabsf(m._14) > 0.01f) return false;
    if (fabsf(m._21) > 0.01f || fabsf(m._23) > 0.01f || fabsf(m._24) > 0.01f) return false;
    if (fabsf(m._31) > 0.01f || fabsf(m._32) > 0.01f) return false;
    if (fabsf(m._11) < 0.01f || fabsf(m._22) < 0.01f) return false;
    if (fabsf(m._34 - 1.0f) > 0.05f) return false;
    if (fabsf(m._44) > 0.05f) return false;

    float fov = ExtractFOV(m);
    if (fov < g_config.minFOV || fov > g_config.maxFOV) return false;

    return true;
}

static D3DMATRIX MultiplyMatrix(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX out = {};
    out._11 = a._11*b._11 + a._12*b._21 + a._13*b._31 + a._14*b._41;
    out._12 = a._11*b._12 + a._12*b._22 + a._13*b._32 + a._14*b._42;
    out._13 = a._11*b._13 + a._12*b._23 + a._13*b._33 + a._14*b._43;
    out._14 = a._11*b._14 + a._12*b._24 + a._13*b._34 + a._14*b._44;

    out._21 = a._21*b._11 + a._22*b._21 + a._23*b._31 + a._24*b._41;
    out._22 = a._21*b._12 + a._22*b._22 + a._23*b._32 + a._24*b._42;
    out._23 = a._21*b._13 + a._22*b._23 + a._23*b._33 + a._24*b._43;
    out._24 = a._21*b._14 + a._22*b._24 + a._23*b._34 + a._24*b._44;

    out._31 = a._31*b._11 + a._32*b._21 + a._33*b._31 + a._34*b._41;
    out._32 = a._31*b._12 + a._32*b._22 + a._33*b._32 + a._34*b._42;
    out._33 = a._31*b._13 + a._32*b._23 + a._33*b._33 + a._34*b._43;
    out._34 = a._31*b._14 + a._32*b._24 + a._33*b._34 + a._34*b._44;

    out._41 = a._41*b._11 + a._42*b._21 + a._43*b._31 + a._44*b._41;
    out._42 = a._41*b._12 + a._42*b._22 + a._43*b._32 + a._44*b._42;
    out._43 = a._41*b._13 + a._42*b._23 + a._43*b._33 + a._44*b._43;
    out._44 = a._41*b._14 + a._42*b._24 + a._43*b._34 + a._44*b._44;
    return out;
}

static bool IsIdentityMatrix(const D3DMATRIX& m, float tolerance) {
    return fabsf(m._11 - 1.0f) < tolerance &&
           fabsf(m._22 - 1.0f) < tolerance &&
           fabsf(m._33 - 1.0f) < tolerance &&
           fabsf(m._44 - 1.0f) < tolerance &&
           fabsf(m._12) < tolerance &&
           fabsf(m._13) < tolerance &&
           fabsf(m._14) < tolerance &&
           fabsf(m._21) < tolerance &&
           fabsf(m._23) < tolerance &&
           fabsf(m._24) < tolerance &&
           fabsf(m._31) < tolerance &&
           fabsf(m._32) < tolerance &&
           fabsf(m._34) < tolerance &&
           fabsf(m._41) < tolerance &&
           fabsf(m._42) < tolerance &&
           fabsf(m._43) < tolerance;
}

static bool MatrixClose(const D3DMATRIX& a, const D3DMATRIX& b, float tolerance) {
    const float* pa = reinterpret_cast<const float*>(&a);
    const float* pb = reinterpret_cast<const float*>(&b);
    for (int i = 0; i < 16; i++) {
        if (fabsf(pa[i] - pb[i]) > tolerance) {
            return false;
        }
    }
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
    bool m_hasWorld = false;
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

    void EmitFixedFunctionTransforms() {
        if (!g_config.emitFixedFunctionTransforms) {
            return;
        }
        if (m_hasWorld) {
            m_real->SetTransform(D3DTS_WORLD, &m_lastWorldMatrix);
        }
        if (m_hasView) {
            m_real->SetTransform(D3DTS_VIEW, &m_lastViewMatrix);
        }
        if (m_hasProj) {
            m_real->SetTransform(D3DTS_PROJECTION, &m_lastProjMatrix);
        }
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

        std::vector<float> overrideScratch;
        const float* effectiveConstantData = pConstantData;
        if (BuildOverriddenConstants(*state, StartRegister, Vector4fCount, pConstantData, overrideScratch)) {
            effectiveConstantData = overrideScratch.data();
        }


        bool constantsChanged = false;

        for (UINT i = 0; i < Vector4fCount; i++) {
            UINT reg = StartRegister + i;
            if (reg >= kMaxConstantRegisters) {
                break;
            }
            if (!constantsChanged) {
                if (!state->valid[reg] ||
                    memcmp(state->constants[reg], effectiveConstantData + i * 4,
                           sizeof(state->constants[reg])) != 0) {
                    constantsChanged = true;
                }
            }
            memcpy(state->constants[reg], effectiveConstantData + i * 4, sizeof(state->constants[reg]));
            state->valid[reg] = true;
            UpdateVariance(*state, static_cast<int>(reg), effectiveConstantData + i * 4);
        }
        if (constantsChanged) {
            state->lastChangeSerial = ++g_constantChangeSerial;
        }
        state->snapshotReady = true;

        if (shaderKey != 0) {
            for (int slot = 0; slot < MatrixSlot_Count; slot++) {
                const ManualMatrixBinding& binding = g_manualBindings[slot];
                if (!binding.enabled || binding.shaderKey != shaderKey) {
                    continue;
                }
                D3DMATRIX manualMat = {};
                if (!TryBuildMatrixSnapshot(*state, binding.baseRegister, binding.rows, false, &manualMat)) {
                    continue;
                }
                if (slot == MatrixSlot_World) {
                    m_lastWorldMatrix = manualMat;
                    m_hasWorld = true;
                    StoreWorldMatrix(m_lastWorldMatrix, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_View) {
                    m_lastViewMatrix = manualMat;
                    m_hasView = true;
                    StoreViewMatrix(m_lastViewMatrix, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_Projection) {
                    m_lastProjMatrix = manualMat;
                    m_hasProj = true;
                    StoreProjectionMatrix(m_lastProjMatrix, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_MVP) {
                    StoreMVPMatrix(manualMat, shaderKey, binding.baseRegister, binding.rows, false, true);
                }
            }
            EmitFixedFunctionTransforms();
        }

        // Diagnostic logging mode - log ALL constant updates
        if (g_config.logAllConstants && m_constantLogThrottle == 0) {
            if (Vector4fCount >= 4) {
                LogMsg("SetVertexShaderConstantF: c%d-%d (%d vectors)",
                       StartRegister, StartRegister + Vector4fCount - 1, Vector4fCount);

                // Log first 16 floats (one matrix worth)
                for (UINT i = 0; i < Vector4fCount && i < 4; i++) {
                    LogMsg("  c%d: [%.3f, %.3f, %.3f, %.3f]",
                           StartRegister + i,
                           effectiveConstantData[i*4+0], effectiveConstantData[i*4+1],
                           effectiveConstantData[i*4+2], effectiveConstantData[i*4+3]);
                }
            }
        }

        // DMC4-specific: c0-c3 contains combined MVP matrix
        // Extract what we can and synthesize valid View/Projection data
        if (StartRegister == 0 && Vector4fCount >= 4) {
            D3DMATRIX mvp;
            memcpy(&mvp, effectiveConstantData, sizeof(D3DMATRIX));
            StoreMVPMatrix(mvp, shaderKey, 0, 4, false, false);

            bool allowMvpExtraction = IsAutoDetectActive() ||
                                      (g_config.viewMatrixRegister < 0 &&
                                       g_config.projMatrixRegister < 0);

            if (allowMvpExtraction) {
                // Check for valid floats (skip LooksLikeMatrix - MVP has large values)
                bool validFloats = true;
                for (int i = 0; i < 16 && validFloats; i++) {
                    if (!std::isfinite(effectiveConstantData[i])) validFloats = false;
                }

                if (validFloats) {
                    // Extract approximate View from MVP
                    ExtractCameraFromMVP(mvp, &m_lastViewMatrix);

                    // Create standard projection (60 degree FOV, 16:9 aspect)
                    CreateProjectionMatrix(&m_lastProjMatrix, 1.047f, 16.0f/9.0f, 0.1f, 10000.0f);

                    StoreViewMatrix(m_lastViewMatrix, shaderKey, 0, 4, false, false);
                    StoreProjectionMatrix(m_lastProjMatrix, shaderKey, 0, 4, false, false);
                    EmitFixedFunctionTransforms();

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

        // Refined legacy auto-detect: deterministic strict heuristics + temporal stability.
        if (IsAutoDetectActive() && Vector4fCount >= 3) {
            for (UINT offset = 0; offset + 3 <= Vector4fCount; offset++) {
                UINT reg = StartRegister + offset;

                if (offset + 4 <= Vector4fCount) {
                    const float* matData = effectiveConstantData + offset * 4;
                    if (LooksLikeMatrix(matData)) {
                        D3DMATRIX mat = {};
                        memcpy(&mat, matData, sizeof(D3DMATRIX));

                        auto evaluate4x4Variant = [&](const D3DMATRIX& candidate, bool transposed) {
                            bool projStrict = LooksLikeProjectionStrict(candidate) && PassesPerspectiveHeuristics(candidate);
                            bool viewStrict = LooksLikeViewStrict(candidate);

                            if (!viewStrict && g_probeInverseView) {
                                D3DMATRIX inv = InvertSimpleRigidView(candidate);
                                if (LooksLikeViewStrict(inv)) {
                                    viewStrict = true;
                                    UpdateStability(*state, static_cast<int>(reg), true, false);
                                    if (shaderKey != 0 && TryPromoteStableAutoCandidate(*state, MatrixSlot_View, shaderKey,
                                                                                       static_cast<int>(reg), 4,
                                                                                       transposed, inv)) {
                                        m_lastViewMatrix = inv;
                                        m_hasView = true;
                                        EmitFixedFunctionTransforms();
                                    }
                                }
                            }

                            if (viewStrict) {
                                UpdateStability(*state, static_cast<int>(reg), true, false);
                                if (shaderKey != 0 && TryPromoteStableAutoCandidate(*state, MatrixSlot_View, shaderKey,
                                                                                   static_cast<int>(reg), 4,
                                                                                   transposed, candidate)) {
                                    m_lastViewMatrix = candidate;
                                    m_hasView = true;
                                    EmitFixedFunctionTransforms();
                                }
                            }

                            if (projStrict) {
                                UpdateStability(*state, static_cast<int>(reg), false, true);
                                if (shaderKey != 0 && TryPromoteStableAutoCandidate(*state, MatrixSlot_Projection,
                                                                                   shaderKey,
                                                                                   static_cast<int>(reg), 4,
                                                                                   transposed, candidate)) {
                                    m_lastProjMatrix = candidate;
                                    m_hasProj = true;
                                    EmitFixedFunctionTransforms();
                                }
                            }
                        };

                        evaluate4x4Variant(mat, false);
                        if (g_probeTransposedLayouts) {
                            evaluate4x4Variant(TransposeMatrix(mat), true);
                        }
                    }
                }

                if (offset + 3 <= Vector4fCount) {
                    D3DMATRIX mat43 = {};
                    if (TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                         static_cast<int>(reg), 3, false, &mat43)) {
                        auto evaluate4x3Variant = [&](const D3DMATRIX& candidate, bool transposed) {
                            if (!LooksLikeViewStrict(candidate)) {
                                return;
                            }
                            UpdateStability(*state, static_cast<int>(reg), true, false);
                            if (shaderKey != 0 && TryPromoteStableAutoCandidate(*state, MatrixSlot_View, shaderKey,
                                                                               static_cast<int>(reg), 3,
                                                                               transposed, candidate)) {
                                m_lastViewMatrix = candidate;
                                m_hasView = true;
                                EmitFixedFunctionTransforms();
                            }
                        };
                        evaluate4x3Variant(mat43, false);
                        if (g_probeTransposedLayouts) {
                            evaluate4x3Variant(TransposeMatrix(mat43), true);
                        }
                    }
                }
            }
        }

        // Configured register detection
        int configuredRows = (g_layoutStrategyMode == Layout_4x3) ? 3 : 4;
        bool configuredTranspose = (g_layoutStrategyMode == Layout_Auto) ? g_probeTransposedLayouts : false;

        // Check for view matrix at configured register
        if (g_config.viewMatrixRegister >= 0) {
            for (int transposePass = 0; transposePass < (configuredTranspose ? 2 : 1); transposePass++) {
                D3DMATRIX mat = {};
                if (!TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                      g_config.viewMatrixRegister, configuredRows,
                                                      transposePass != 0, &mat)) {
                    continue;
                }

                bool viewStrict = LooksLikeViewStrict(mat);
                if (!viewStrict && g_probeInverseView) {
                    D3DMATRIX inv = InvertSimpleRigidView(mat);
                    if (LooksLikeViewStrict(inv)) {
                        mat = inv;
                        viewStrict = true;
                    }
                }
                if (viewStrict || LooksLikeView(mat)) {
                    memcpy(&m_lastViewMatrix, &mat, sizeof(D3DMATRIX));
                    m_hasView = true;
                    StoreViewMatrix(m_lastViewMatrix, shaderKey, g_config.viewMatrixRegister, configuredRows,
                                    transposePass != 0, false);
                    EmitFixedFunctionTransforms();
                    LogMsg("Extracted VIEW matrix from c%d (rows=%d transpose=%d)",
                           g_config.viewMatrixRegister, configuredRows, transposePass != 0 ? 1 : 0);
                    UpdateStability(*state, g_config.viewMatrixRegister, viewStrict, false);
                    break;
                }
            }
        }

        // Check for projection matrix at configured register
        if (g_config.projMatrixRegister >= 0) {
            for (int transposePass = 0; transposePass < (configuredTranspose ? 2 : 1); transposePass++) {
                D3DMATRIX mat = {};
                if (!TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                      g_config.projMatrixRegister, configuredRows,
                                                      transposePass != 0, &mat)) {
                    continue;
                }

                bool projStrict = LooksLikeProjectionStrict(mat);
                bool projLike = LooksLikeProjection(mat);
                if (!projStrict && !projLike && g_layoutStrategyMode == Layout_VP) {
                    projLike = ComputeProjectionLikeScore(mat) > 1.0f;
                }
                if (projStrict || projLike) {
                    memcpy(&m_lastProjMatrix, &mat, sizeof(D3DMATRIX));
                    m_hasProj = true;
                    StoreProjectionMatrix(m_lastProjMatrix, shaderKey, g_config.projMatrixRegister, configuredRows,
                                          transposePass != 0, false);
                    EmitFixedFunctionTransforms();

                    float fov = ExtractFOV(mat) * 180.0f / 3.14159f;
                    LogMsg("Extracted PROJECTION matrix from c%d (rows=%d transpose=%d FOV: %.1f deg)",
                           g_config.projMatrixRegister, configuredRows, transposePass != 0 ? 1 : 0, fov);
                    UpdateStability(*state, g_config.projMatrixRegister, false, projStrict);
                    break;
                }
            }
        }

        // Check for world matrix at configured register
        if (g_config.worldMatrixRegister >= 0) {
            D3DMATRIX mat = {};
            if (TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                 g_config.worldMatrixRegister, configuredRows,
                                                 false, &mat) && LooksLikeMatrix(reinterpret_cast<const float*>(&mat))) {
                memcpy(&m_lastWorldMatrix, &mat, sizeof(D3DMATRIX));
                m_hasWorld = true;
                StoreWorldMatrix(m_lastWorldMatrix, shaderKey, g_config.worldMatrixRegister, configuredRows, false, false);
                EmitFixedFunctionTransforms();
            }
        }

        if (g_cameraMatrices.hasMVP && g_cameraMatrices.hasView && g_cameraMatrices.hasProjection &&
            g_cameraMatrices.hasWorld && IsIdentityMatrix(g_cameraMatrices.world, 0.05f)) {
            D3DMATRIX vp = MultiplyMatrix(g_cameraMatrices.view, g_cameraMatrices.projection);
            if (MatrixClose(vp, g_cameraMatrices.mvp, 0.05f)) {
                LogMsg("MVP consistency check: view*proj matches MVP (identity world).");
            }
        }

        return m_real->SetVertexShaderConstantF(StartRegister, effectiveConstantData, Vector4fCount);
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
        if (g_config.enableMemoryScanner && g_config.memoryScannerIntervalSec > 0) {
            DWORD nowTick = GetTickCount();
            if (g_memoryScannerLastTick == 0 ||
                nowTick - g_memoryScannerLastTick >= static_cast<DWORD>(g_config.memoryScannerIntervalSec) * 1000u) {
                StartMemoryScanner();
                g_memoryScannerLastTick = nowTick;
            }
        }

        // Log periodic status
        if (g_frameCount % 300 == 0) {
            LogMsg("Frame %d - hasView: %d, hasProj: %d", g_frameCount, m_hasView, m_hasProj);
            if (g_config.logCandidateSummary) {
                ShaderConstantState* state = GetShaderState(g_activeShaderKey, false);
                if (state) {
                    LogTopCandidates(*state, g_activeShaderKey);
                }
            }
        }

        if (!g_imguiInitialized) {
            InitializeImGui(m_real, m_hwnd);
        }
        UpdateImGuiToggle();
        if (g_imguiInitialized) {
            ImGui::GetIO().MouseDrawCursor = g_showImGui;
        }
        RenderImGuiOverlay();
        if (g_requestManualEmit) {
            EmitFixedFunctionTransforms();
            g_requestManualEmit = false;
            snprintf(g_manualEmitStatus, sizeof(g_manualEmitStatus),
                     "Sent cached World/View/Projection matrices to RTX Remix via SetTransform().");
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
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override {
        if ((g_pauseRendering || IsCurrentShaderDrawDisabled(m_currentVertexShader)) && !g_isRenderingImGui) {
            return D3D_OK;
        }
        return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override {
        if ((g_pauseRendering || IsCurrentShaderDrawDisabled(m_currentVertexShader)) && !g_isRenderingImGui) {
            return D3D_OK;
        }
        return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override {
        if ((g_pauseRendering || IsCurrentShaderDrawDisabled(m_currentVertexShader)) && !g_isRenderingImGui) {
            return D3D_OK;
        }
        return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override {
        if ((g_pauseRendering || IsCurrentShaderDrawDisabled(m_currentVertexShader)) && !g_isRenderingImGui) {
            return D3D_OK;
        }
        return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
    }
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

        if (pShader) {
            uint32_t shaderHash = ComputeShaderBytecodeHash(pShader);
            if (shaderHash != 0) {
                g_shaderBytecodeHashes[g_activeShaderKey] = shaderHash;
                auto profileIt = g_heuristicProfiles.find(shaderHash);
                if (profileIt != g_heuristicProfiles.end() && profileIt->second.valid) {
                    const HeuristicProfile& profile = profileIt->second;
                    if (profile.viewBase >= 0) {
                        g_config.viewMatrixRegister = profile.viewBase;
                    }
                    if (profile.projBase >= 0) {
                        g_config.projMatrixRegister = profile.projBase;
                    }
                    g_layoutStrategyMode = profile.layoutMode;
                    g_probeTransposedLayouts = profile.transposed;
                    g_probeInverseView = profile.inverseView;
                    LogMsg("Applied heuristic profile for shader hash 0x%08X", shaderHash);
                }
            }
        }

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
    g_iniViewMatrixRegister = g_config.viewMatrixRegister;
    g_iniProjMatrixRegister = g_config.projMatrixRegister;
    g_iniWorldMatrixRegister = g_config.worldMatrixRegister;
    g_config.enableLogging = GetPrivateProfileIntA("CameraProxy", "EnableLogging", 1, path) != 0;
    g_config.logAllConstants = GetPrivateProfileIntA("CameraProxy", "LogAllConstants", 0, path) != 0;
    g_config.autoDetectMatrices = GetPrivateProfileIntA("CameraProxy", "AutoDetectMatrices", 0, path) != 0;
    g_config.stabilityFrames = GetPrivateProfileIntA("CameraProxy", "StabilityFrames", 5, path);
    g_config.varianceThreshold = static_cast<float>(
        GetPrivateProfileIntA("CameraProxy", "VarianceThresholdMilli", 3, path)) / 1000.0f;
    g_config.topCandidateCount = GetPrivateProfileIntA("CameraProxy", "TopCandidateCount", 5, path);
    g_config.logCandidateSummary = GetPrivateProfileIntA("CameraProxy", "LogCandidateSummary", 1, path) != 0;
    g_config.enableMemoryScanner = GetPrivateProfileIntA("CameraProxy", "EnableMemoryScanner", 0, path) != 0;
    g_config.memoryScannerIntervalSec = GetPrivateProfileIntA("CameraProxy", "MemoryScannerIntervalSec", 0, path);
    g_config.memoryScannerMaxResults = GetPrivateProfileIntA("CameraProxy", "MemoryScannerMaxResults", 25, path);
    GetPrivateProfileStringA("CameraProxy", "MemoryScannerModule", "", g_config.memoryScannerModule,
                             MAX_PATH, path);
    g_config.useRemixRuntime = GetPrivateProfileIntA("CameraProxy", "UseRemixRuntime", 1, path) != 0;
    g_config.emitFixedFunctionTransforms = GetPrivateProfileIntA("CameraProxy", "EmitFixedFunctionTransforms", 1, path) != 0;
    GetPrivateProfileStringA("CameraProxy", "RemixDllName", "d3d9_remix.dll", g_config.remixDllName,
                             MAX_PATH, path);
    g_layoutStrategyMode = GetPrivateProfileIntA("CameraProxy", "LayoutStrategyMode", Layout_Auto, path);
    g_probeTransposedLayouts = GetPrivateProfileIntA("CameraProxy", "ProbeTransposedLayouts", 1, path) != 0;
    g_probeInverseView = GetPrivateProfileIntA("CameraProxy", "ProbeInverseView", 1, path) != 0;
    g_autoPickCandidates = GetPrivateProfileIntA("CameraProxy", "AutoPickCandidates", 1, path) != 0;
    g_overrideScopeMode = GetPrivateProfileIntA("CameraProxy", "OverrideScopeMode", Override_Sticky, path);
    g_overrideNFrames = GetPrivateProfileIntA("CameraProxy", "OverrideNFrames", 3, path);
    g_autoDetectMode = GetPrivateProfileIntA("CameraProxy", "AutoDetectMode", AutoDetect_IndividualWVP, path);
    g_autoDetectMinFramesSeen = GetPrivateProfileIntA("CameraProxy", "AutoDetectMinFramesSeen", 12, path);
    g_autoDetectMinConsecutiveFrames = GetPrivateProfileIntA("CameraProxy", "AutoDetectMinConsecutiveFrames", 4, path);
    g_autoDetectSamplingFrames = GetPrivateProfileIntA("CameraProxy", "AutoDetectSamplingFrames", 180, path);
    g_autoApplyDetectedMatrices = GetPrivateProfileIntA("CameraProxy", "AutoApplyDetectedMatrices", 1, path) != 0;

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
        LoadHeuristicProfiles();

        if (g_config.enableLogging) {
            g_logFile = fopen("camera_proxy.log", "w");
            LogMsg("=== DMC4 Camera Proxy for D3D9 ===");
            LogMsg("View matrix register: c%d-c%d", g_config.viewMatrixRegister, g_config.viewMatrixRegister + 3);
            LogMsg("Projection matrix register: c%d-c%d", g_config.projMatrixRegister, g_config.projMatrixRegister + 3);
            LogMsg("Auto-detect matrices: %s", g_config.autoDetectMatrices ? "ENABLED" : "disabled");
            LogMsg("Log all constants: %s", g_config.logAllConstants ? "ENABLED" : "disabled");
            LogMsg("Stability frames: %d", g_config.stabilityFrames);
            LogMsg("Variance threshold: %.4f", g_config.varianceThreshold);
            LogMsg("Candidate summary: %s", g_config.logCandidateSummary ? "ENABLED" : "disabled");
            LogMsg("Memory scanner: %s", g_config.enableMemoryScanner ? "ENABLED" : "disabled");
            LogMsg("Use Remix runtime: %s", g_config.useRemixRuntime ? "ENABLED" : "disabled");
            LogMsg("Remix runtime DLL: %s", g_config.remixDllName);
            LogMsg("Emit fixed-function transforms: %s", g_config.emitFixedFunctionTransforms ? "ENABLED" : "disabled");
            LogMsg("Layout strategy mode: %d", g_layoutStrategyMode);
            LogMsg("Probe transposed layouts: %s", g_probeTransposedLayouts ? "ENABLED" : "disabled");
            LogMsg("Probe inverse view: %s", g_probeInverseView ? "ENABLED" : "disabled");
            LogMsg("Override scope mode: %d (N=%d)", g_overrideScopeMode, g_overrideNFrames);
            LogMsg("Auto detect mode: %d, min frames=%d, min streak=%d",
                   g_autoDetectMode, g_autoDetectMinFramesSeen, g_autoDetectMinConsecutiveFrames);
            LogMsg("Auto detect legacy compatibility keys: samplingFrames=%d autoApply=%s (not used by refined detector)",
                   g_autoDetectSamplingFrames, g_autoApplyDetectedMatrices ? "ENABLED" : "disabled");
            if (g_config.enableMemoryScanner) {
                LogMsg("Memory scanner interval: %d sec", g_config.memoryScannerIntervalSec);
                LogMsg("Memory scanner module: %s", g_config.memoryScannerModule[0]
                                                   ? g_config.memoryScannerModule
                                                   : "<main module>");
            }
        }

        // Load the real D3D9 runtime (Remix or system, based on config)
        g_hD3D9 = LoadTargetD3D9();

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
            LogMsg("Loaded target d3d9 runtime successfully");
            LogMsg("  Direct3DCreate9: %p", g_origDirect3DCreate9);
            LogMsg("  Direct3DCreate9Ex: %p", g_origDirect3DCreate9Ex);
        } else {
            LogMsg("ERROR: Failed to load target d3d9 runtime!");
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
