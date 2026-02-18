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
#include <cassert>

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

static float Dot3(float ax, float ay, float az, float bx, float by, float bz);

enum ProjectionHandedness {
    ProjectionHandedness_Unknown = 0,
    ProjectionHandedness_Left,
    ProjectionHandedness_Right
};

enum CustomProjectionMode {
    CustomProjectionMode_Manual = 1,
    CustomProjectionMode_Auto = 2
};

struct ProjectionAnalysis {
    bool valid = false;
    float fovRadians = 0.0f;
    ProjectionHandedness handedness = ProjectionHandedness_Unknown;
};

static bool AnalyzeProjectionMatrixNumeric(const D3DMATRIX& m, ProjectionAnalysis* out);
static const char* ProjectionHandednessLabel(ProjectionHandedness handedness);
static bool InvertMatrix4x4Deterministic(const D3DMATRIX& in, D3DMATRIX* out, float* outDeterminant = nullptr);
void CreateIdentityMatrix(D3DMATRIX* out);
class WrappedD3D9Device;

#pragma comment(lib, "user32.lib")

// Configuration
struct ProxyConfig {
    int viewMatrixRegister = -1;
    int projMatrixRegister = -1;
    int worldMatrixRegister = -1;
    bool enableLogging = true;
    float minFOV = 0.1f;
    float maxFOV = 2.5f;
    bool enableMemoryScanner = false;
    int memoryScannerIntervalSec = 0;
    int memoryScannerMaxResults = 25;
    char memoryScannerModule[MAX_PATH] = {};
    bool useRemixRuntime = true;
    char remixDllName[MAX_PATH] = "d3d9_remix.dll";
    bool emitFixedFunctionTransforms = true;
    char gameProfile[64] = "";

    // Diagnostic mode - log ALL shader constant updates
    bool logAllConstants = false;
    bool autoDetectMatrices = false;
    float imguiScale = 1.0f;
    int hotkeyToggleMenuVk = VK_F10;
    int hotkeyTogglePauseVk = VK_F9;
    int hotkeyEmitMatricesVk = VK_F8;
    int hotkeyResetMatrixOverridesVk = VK_F7;

    bool enableCombinedMVP = false;
    bool combinedMVPRequireWorld = false;
    bool combinedMVPAssumeIdentityWorld = true;
    bool combinedMVPForceDecomposition = false;
    bool combinedMVPLogDecomposition = false;

    bool experimentalCustomProjectionEnabled = false;
    CustomProjectionMode experimentalCustomProjectionMode = CustomProjectionMode_Auto;
    bool experimentalCustomProjectionOverrideDetectedProjection = false;
    bool experimentalCustomProjectionOverrideCombinedMVP = false;
    bool mgrrUseAutoProjectionWhenC4Invalid = false;
    bool barnyardUseGameSetTransformsForViewProjection = true;
    bool disableGameInputWhileMenuOpen = false;
    bool setTransformBypassProxyWhenGameProvides = false;
    bool setTransformRoundTripCompatibilityMode = false;
    float experimentalCustomProjectionAutoFovDeg = 60.0f;
    float experimentalCustomProjectionAutoNearZ = 0.1f;
    float experimentalCustomProjectionAutoFarZ = 1000.0f;
    float experimentalCustomProjectionAutoAspectFallback = 16.0f / 9.0f;
    ProjectionHandedness experimentalCustomProjectionAutoHandedness = ProjectionHandedness_Left;
    D3DMATRIX experimentalCustomProjectionManualMatrix = {};
    bool experimentalInverseViewAsWorld = false;
    bool experimentalInverseViewAsWorldAllowUnverified = false;
    bool experimentalInverseViewAsWorldFast = false;
};

enum CombinedMVPStrategy {
    CombinedMVPStrategy_None = 0,
    CombinedMVPStrategy_WorldAndMVP,
    CombinedMVPStrategy_MVPOnly,
    CombinedMVPStrategy_WorldRequiredNoWorld,
    CombinedMVPStrategy_Disabled,
    CombinedMVPStrategy_SkippedFullWVP,
    CombinedMVPStrategy_Failed
};

static const char* CombinedMVPStrategyLabel(CombinedMVPStrategy strategy);

struct CombinedMVPDebugState {
    int registerBase = -1;
    CombinedMVPStrategy strategy = CombinedMVPStrategy_None;
    bool succeeded = false;
    float fovRadians = 0.0f;
    ProjectionHandedness handedness = ProjectionHandedness_Unknown;
};

enum GameProfileKind {
    GameProfile_None = 0,
    GameProfile_MetalGearRising,
    GameProfile_DevilMayCry4,
    GameProfile_Barnyard
};

struct RegisterLayoutProfile {
    int combinedMvpBase = -1;
    int projectionBase = -1;
    int viewInverseBase = -1;
    int worldBase = -1;
    int viewProjectionBase = -1;
    int worldViewBase = -1;
};

static GameProfileKind g_activeGameProfile = GameProfile_None;
static RegisterLayoutProfile g_profileLayout = {};
static bool g_profileViewDerivedFromInverse = false;
static bool g_profileCoreRegistersSeen[3] = { false, false, false }; // proj, viewInv, world
static bool g_profileOptionalRegistersSeen[2] = { false, false }; // VP, WV
static char g_profileStatusMessage[256] = "";
static bool g_profileDisableStructuralDetection = false;
static bool g_mgrProjCapturedThisFrame = false;
static bool g_mgrViewCapturedThisFrame = false;
static bool g_mgrWorldCapturedForDraw = false;
static bool g_mgrProjectionRegisterValid = false;
static bool g_barnyardForceWorldFromC0 = false;

static ProxyConfig g_config;
static HMODULE g_hD3D9 = nullptr;
static HINSTANCE g_moduleInstance = nullptr;
static std::once_flag g_initOnce;
static FILE* g_logFile = nullptr;
static int g_frameCount = 0;

struct CameraMatrices {
    D3DMATRIX view;
    D3DMATRIX projection;
    D3DMATRIX world;
    D3DMATRIX mvp;
    D3DMATRIX vp;
    D3DMATRIX wv;
    bool hasView;
    bool hasProjection;
    bool hasWorld;
    bool hasMVP;
    bool hasVP;
    bool hasWV;
};

enum MatrixSlot {
    MatrixSlot_World = 0,
    MatrixSlot_View = 1,
    MatrixSlot_Projection = 2,
    MatrixSlot_MVP = 3,
    MatrixSlot_VP = 4,
    MatrixSlot_WV = 5,
    MatrixSlot_Count = 6
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
    const char* sourceLabel = "unknown";
    int extractedFromRegister = -1;
};

static CameraMatrices g_cameraMatrices = {};
static MatrixSourceInfo g_matrixSources[MatrixSlot_Count] = {};
static std::mutex g_cameraMatricesMutex;
static ManualMatrixBinding g_manualBindings[MatrixSlot_Count] = {};
static void UpdateMatrixSource(MatrixSlot slot,
                               uintptr_t shaderKey,
                               int baseRegister,
                               int rows,
                               bool transposed,
                               bool manual,
                               const char* sourceLabel = nullptr,
                               int extractedFromRegister = -1);

static bool g_imguiInitialized = false;
static HWND g_imguiHwnd = nullptr;
static bool g_showImGui = false;
static bool g_prevShowImGui = false;
static bool g_pauseRendering = false;
static bool g_isRenderingImGui = false;
static WNDPROC g_imguiPrevWndProc = nullptr;
static bool g_showConstantsAsMatrices = true;
static bool g_filterDetectedMatrices = false;
static bool g_showAllConstantRegisters = false;
static bool g_showFpsStats = false;
static bool g_showTransposedMatrices = false;
static float g_imguiScaleRuntime = 1.0f;
static ImGuiStyle g_imguiBaseStyle = {};
static bool g_imguiMgrrUseAutoProjection = false;
static bool g_imguiBarnyardUseGameSetTransformsForViewProjection = true;
static bool g_imguiDisableGameInputWhileMenuOpen = false;
static bool g_imguiBaseStyleCaptured = false;
static bool g_enableShaderEditing = false;
static bool g_requestManualEmit = false;
static char g_manualEmitStatus[192] = "";
static char g_matrixAssignStatus[256] = "";
static int g_manualAssignRows = 4;
static bool g_projectionDetectedByNumericStructure = false;
static float g_projectionDetectedFovRadians = 0.0f;
static int g_projectionDetectedRegister = -1;
static ProjectionHandedness g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
static CombinedMVPDebugState g_combinedMvpDebug = {};
static char g_customProjectionStatus[256] = "";
static bool g_lastInverseViewAsWorldEligible = false;
static bool g_lastInverseViewAsWorldApplied = false;
static bool g_lastInverseViewAsWorldUsedFast = false;
static bool g_gameSetTransformSeen[3] = { false, false, false }; // world, view, projection
static bool g_gameSetTransformAnySeen = false;

static HHOOK g_keyboardBlockHook = nullptr;
static HHOOK g_mouseBlockHook = nullptr;
static bool g_imguiAsyncKeyboardPrev[256] = {};

enum HotkeyAction {
    HotkeyAction_ToggleMenu = 0,
    HotkeyAction_TogglePause,
    HotkeyAction_EmitMatrices,
    HotkeyAction_ResetMatrixOverrides,
    HotkeyAction_Count
};

static bool g_hotkeyWasDown[HotkeyAction_Count] = {};

static int g_iniViewMatrixRegister = -1;
static int g_iniProjMatrixRegister = -1;
static int g_iniWorldMatrixRegister = -1;
static char g_iniPath[MAX_PATH] = {};

static constexpr int kMaxConstantRegisters = 256;
static int g_selectedRegister = -1;
static uintptr_t g_activeShaderKey = 0;
static uintptr_t g_selectedShaderKey = 0;

enum OverrideScopeMode {
    Override_Sticky = 0,
    Override_OneFrame = 1,
    Override_NFrames = 2
};

static bool g_probeTransposedLayouts = true;
static bool g_constantUploadRecordingEnabled = false;
static bool g_probeInverseView = true;
static int g_overrideScopeMode = Override_Sticky;
static int g_overrideNFrames = 3;
static std::unordered_map<uintptr_t, uint32_t> g_shaderBytecodeHashes = {};

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
    int overrideExpiresAtFrame[kMaxConstantRegisters];
    bool snapshotReady = false;
    unsigned long long sampleCounts[kMaxConstantRegisters] = {};
    double mean[kMaxConstantRegisters][4] = {};
    double m2[kMaxConstantRegisters][4] = {};
    unsigned long long lastChangeSerial = 0;
};

enum ConstantUploadStage {
    ConstantUploadStage_Vertex = 0,
    ConstantUploadStage_Pixel = 1
};


struct ConstantUploadEvent {
    ConstantUploadStage stage = ConstantUploadStage_Vertex;
    uintptr_t shaderKey = 0;
    uint32_t shaderHash = 0;
    UINT startRegister = 0;
    UINT vectorCount = 0;
    unsigned long long changeSerial = 0;
};

struct GlobalVertexRegisterState {
    float value[4] = {};
    bool valid = false;
    unsigned long long lastUploadSerial = 0;
    uintptr_t lastShaderKey = 0;
    uint32_t lastShaderHash = 0;
};

static ShaderConstantState* GetShaderState(uintptr_t shaderKey, bool createIfMissing);

static std::unordered_map<uintptr_t, ShaderConstantState> g_shaderConstants = {};
static std::vector<uintptr_t> g_shaderOrder = {};
static std::unordered_map<uintptr_t, bool> g_disabledShaders = {};
static unsigned long long g_constantChangeSerial = 0;
static unsigned long long g_constantUploadSerial = 0;
static std::deque<ConstantUploadEvent> g_constantUploadEvents = {};
static constexpr size_t kMaxConstantUploadEvents = 2000;
static GlobalVertexRegisterState g_allVertexRegisters[kMaxConstantRegisters] = {};
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
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam) && g_showImGui) {
            return TRUE;
        }
        if (g_showImGui) {
            ImGuiIO& io = ImGui::GetIO();
            const bool keyboardMsg = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || msg == WM_CHAR || msg == WM_SYSCHAR;
            const bool mouseMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
            if (g_config.disableGameInputWhileMenuOpen && (keyboardMsg || mouseMsg)) {
                return TRUE;
            }
            if ((keyboardMsg && io.WantCaptureKeyboard) || (mouseMsg && io.WantCaptureMouse)) {
                return TRUE;
            }
        }
    }
    if (g_imguiPrevWndProc) {
        return CallWindowProc(g_imguiPrevWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static bool ShouldBypassInputForImGuiMenu() {
    return g_imguiInitialized && g_showImGui && g_config.disableGameInputWhileMenuOpen;
}

static bool IsProxyHotkeyVk(DWORD vkCode) {
    return vkCode == static_cast<DWORD>(g_config.hotkeyToggleMenuVk) ||
           vkCode == static_cast<DWORD>(g_config.hotkeyTogglePauseVk) ||
           vkCode == static_cast<DWORD>(g_config.hotkeyEmitMatricesVk) ||
           vkCode == static_cast<DWORD>(g_config.hotkeyResetMatrixOverridesVk);
}

static LRESULT CALLBACK LowLevelKeyboardBlockHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && ShouldBypassInputForImGuiMenu()) {
        const KBDLLHOOKSTRUCT* keyInfo = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (keyInfo && !IsProxyHotkeyVk(keyInfo->vkCode)) {
            return 1;
        }
    }
    return CallNextHookEx(g_keyboardBlockHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK LowLevelMouseBlockHook(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && ShouldBypassInputForImGuiMenu()) {
        return 1;
    }
    return CallNextHookEx(g_mouseBlockHook, nCode, wParam, lParam);
}

static void UpdateInputBlockHooks() {
    const bool shouldBlock = ShouldBypassInputForImGuiMenu();
    if (shouldBlock) {
        if (!g_keyboardBlockHook) {
            g_keyboardBlockHook = SetWindowsHookExA(WH_KEYBOARD_LL, LowLevelKeyboardBlockHook, g_moduleInstance, 0);
        }
        if (!g_mouseBlockHook) {
            g_mouseBlockHook = SetWindowsHookExA(WH_MOUSE_LL, LowLevelMouseBlockHook, g_moduleInstance, 0);
        }
    } else {
        if (g_keyboardBlockHook) {
            UnhookWindowsHookEx(g_keyboardBlockHook);
            g_keyboardBlockHook = nullptr;
        }
        if (g_mouseBlockHook) {
            UnhookWindowsHookEx(g_mouseBlockHook);
            g_mouseBlockHook = nullptr;
        }
    }
}

static ImGuiKey VkToImGuiKey(UINT vk) {
    if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;
    case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;
    case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown;
    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space;
    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_OEM_7: return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA: return ImGuiKey_Comma;
    case VK_OEM_MINUS: return ImGuiKey_Minus;
    case VK_OEM_PERIOD: return ImGuiKey_Period;
    case VK_OEM_2: return ImGuiKey_Slash;
    case VK_OEM_1: return ImGuiKey_Semicolon;
    case VK_OEM_PLUS: return ImGuiKey_Equal;
    case VK_OEM_4: return ImGuiKey_LeftBracket;
    case VK_OEM_5: return ImGuiKey_Backslash;
    case VK_OEM_6: return ImGuiKey_RightBracket;
    case VK_OEM_3: return ImGuiKey_GraveAccent;
    case VK_CAPITAL: return ImGuiKey_CapsLock;
    case VK_SCROLL: return ImGuiKey_ScrollLock;
    case VK_NUMLOCK: return ImGuiKey_NumLock;
    case VK_SNAPSHOT: return ImGuiKey_PrintScreen;
    case VK_PAUSE: return ImGuiKey_Pause;
    case VK_NUMPAD0: return ImGuiKey_Keypad0;
    case VK_NUMPAD1: return ImGuiKey_Keypad1;
    case VK_NUMPAD2: return ImGuiKey_Keypad2;
    case VK_NUMPAD3: return ImGuiKey_Keypad3;
    case VK_NUMPAD4: return ImGuiKey_Keypad4;
    case VK_NUMPAD5: return ImGuiKey_Keypad5;
    case VK_NUMPAD6: return ImGuiKey_Keypad6;
    case VK_NUMPAD7: return ImGuiKey_Keypad7;
    case VK_NUMPAD8: return ImGuiKey_Keypad8;
    case VK_NUMPAD9: return ImGuiKey_Keypad9;
    case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE: return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case VK_ADD: return ImGuiKey_KeypadAdd;
    case VK_LSHIFT: return ImGuiKey_LeftShift;
    case VK_RSHIFT: return ImGuiKey_RightShift;
    case VK_LCONTROL: return ImGuiKey_LeftCtrl;
    case VK_RCONTROL: return ImGuiKey_RightCtrl;
    case VK_LMENU: return ImGuiKey_LeftAlt;
    case VK_RMENU: return ImGuiKey_RightAlt;
    case VK_LWIN: return ImGuiKey_LeftSuper;
    case VK_RWIN: return ImGuiKey_RightSuper;
    case VK_APPS: return ImGuiKey_Menu;
    default: return ImGuiKey_None;
    }
}

static void UpdateImGuiKeyboardFromAsyncState() {
    if (!g_showImGui || !g_imguiInitialized) {
        memset(g_imguiAsyncKeyboardPrev, 0, sizeof(g_imguiAsyncKeyboardPrev));
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    for (UINT vk = 0; vk < 256; ++vk) {
        const bool down = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        const bool prev = g_imguiAsyncKeyboardPrev[vk];
        if (down == prev) {
            continue;
        }
        g_imguiAsyncKeyboardPrev[vk] = down;
        const ImGuiKey key = VkToImGuiKey(vk);
        if (key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
            io.SetKeyEventNativeData(key, vk, static_cast<int>(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC)));
        }
        if (down) {
            BYTE keyboardState[256] = {};
            if (GetKeyboardState(keyboardState)) {
                WCHAR utf16Buf[4] = {};
                const int translated = ToUnicode(vk,
                                                 MapVirtualKeyA(vk, MAPVK_VK_TO_VSC),
                                                 keyboardState,
                                                 utf16Buf,
                                                 4,
                                                 0);
                if (translated > 0) {
                    for (int i = 0; i < translated; ++i) {
                        io.AddInputCharacterUTF16(utf16Buf[i]);
                    }
                }
            }
        }
    }

    io.AddKeyEvent(ImGuiMod_Ctrl, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                                  (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
}

static void EnsureWndProcHookInstalled() {
    if (!g_imguiInitialized || !g_imguiHwnd) {
        return;
    }
    WNDPROC current = reinterpret_cast<WNDPROC>(
        GetWindowLongPtr(g_imguiHwnd, GWLP_WNDPROC));
    if (current != ImGuiWndProcHook) {
        g_imguiPrevWndProc = current;
        SetWindowLongPtr(g_imguiHwnd, GWLP_WNDPROC,
                         reinterpret_cast<LONG_PTR>(ImGuiWndProcHook));
    }
}

static void StoreViewMatrix(const D3DMATRIX& view,
                            uintptr_t shaderKey = 0,
                            int baseRegister = -1,
                            int rows = 4,
                            bool transposed = false,
                            bool manual = false,
                            const char* sourceLabel = nullptr,
                            int extractedFromRegister = -1) {
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        g_cameraMatrices.view = view;
        g_cameraMatrices.hasView = true;
    }
    UpdateMatrixSource(MatrixSlot_View, shaderKey, baseRegister, rows, transposed, manual,
                       sourceLabel, extractedFromRegister);
}

static void StoreProjectionMatrix(const D3DMATRIX& projection,
                                  uintptr_t shaderKey = 0,
                                  int baseRegister = -1,
                                  int rows = 4,
                                  bool transposed = false,
                                  bool manual = false,
                                  const char* sourceLabel = nullptr,
                                  int extractedFromRegister = -1) {
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        g_cameraMatrices.projection = projection;
        g_cameraMatrices.hasProjection = true;
    }
    UpdateMatrixSource(MatrixSlot_Projection, shaderKey, baseRegister, rows, transposed, manual,
                       sourceLabel, extractedFromRegister);
}

static void StoreWorldMatrix(const D3DMATRIX& world,
                             uintptr_t shaderKey = 0,
                             int baseRegister = -1,
                             int rows = 4,
                             bool transposed = false,
                             bool manual = false,
                             const char* sourceLabel = nullptr,
                             int extractedFromRegister = -1) {
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        g_cameraMatrices.world = world;
        g_cameraMatrices.hasWorld = true;
    }
    UpdateMatrixSource(MatrixSlot_World, shaderKey, baseRegister, rows, transposed, manual,
                       sourceLabel, extractedFromRegister);
}

static void StoreMVPMatrix(const D3DMATRIX& mvp,
                           uintptr_t shaderKey = 0,
                           int baseRegister = -1,
                           int rows = 4,
                           bool transposed = false,
                           bool manual = false,
                           const char* sourceLabel = nullptr,
                           int extractedFromRegister = -1) {
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        g_cameraMatrices.mvp = mvp;
        g_cameraMatrices.hasMVP = true;
    }
    UpdateMatrixSource(MatrixSlot_MVP, shaderKey, baseRegister, rows, transposed, manual,
                       sourceLabel, extractedFromRegister);
}


static void StoreVPMatrix(const D3DMATRIX& vp,
                          uintptr_t shaderKey = 0,
                          int baseRegister = -1,
                          int rows = 4,
                          bool transposed = false,
                          bool manual = false,
                          const char* sourceLabel = nullptr,
                          int extractedFromRegister = -1) {
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        g_cameraMatrices.vp = vp;
        g_cameraMatrices.hasVP = true;
    }
    UpdateMatrixSource(MatrixSlot_VP, shaderKey, baseRegister, rows, transposed, manual,
                       sourceLabel, extractedFromRegister);
}

static void StoreWVMatrix(const D3DMATRIX& wv,
                          uintptr_t shaderKey = 0,
                          int baseRegister = -1,
                          int rows = 4,
                          bool transposed = false,
                          bool manual = false,
                          const char* sourceLabel = nullptr,
                          int extractedFromRegister = -1) {
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        g_cameraMatrices.wv = wv;
        g_cameraMatrices.hasWV = true;
    }
    UpdateMatrixSource(MatrixSlot_WV, shaderKey, baseRegister, rows, transposed, manual,
                       sourceLabel, extractedFromRegister);
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

static float GetWindowDpiScale(HWND hwnd) {
    if (!hwnd) {
        return 1.0f;
    }
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) {
        return 1.0f;
    }
    typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
    GetDpiForWindowFn getDpiForWindow =
        reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow"));
    if (!getDpiForWindow) {
        return 1.0f;
    }
    UINT dpi = getDpiForWindow(hwnd);
    if (dpi == 0) {
        return 1.0f;
    }
    return static_cast<float>(dpi) / 96.0f;
}

static void ApplyImGuiScale(HWND hwnd) {
    if (!g_imguiBaseStyleCaptured) {
        return;
    }
    const float dpiScale = GetWindowDpiScale(hwnd);
    const float clampedUiScale = (std::max)(0.5f, (std::min)(3.0f, g_imguiScaleRuntime));
    const float finalScale = clampedUiScale * dpiScale;

    ImGuiStyle& style = ImGui::GetStyle();
    style = g_imguiBaseStyle;
    style.ScaleAllSizes(finalScale);

    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = finalScale;
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

    g_imguiBaseStyle = style;
    g_imguiBaseStyleCaptured = true;
    g_imguiScaleRuntime = g_config.imguiScale;
    if (g_imguiScaleRuntime < 0.5f) g_imguiScaleRuntime = 0.5f;
    if (g_imguiScaleRuntime > 3.0f) g_imguiScaleRuntime = 3.0f;
    ApplyImGuiScale(hwnd);

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

    // Remove our hook first so ImGui_ImplWin32_Shutdown restores correctly.
    if (g_imguiHwnd && g_imguiPrevWndProc) {
        WNDPROC current = reinterpret_cast<WNDPROC>(
            GetWindowLongPtr(g_imguiHwnd, GWLP_WNDPROC));
        if (current == ImGuiWndProcHook) {
            SetWindowLongPtr(g_imguiHwnd, GWLP_WNDPROC,
                             reinterpret_cast<LONG_PTR>(g_imguiPrevWndProc));
        }
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_showImGui = false;
    g_imguiInitialized = false;
    UpdateInputBlockHooks();
    g_imguiPrevWndProc = nullptr;
    g_imguiHwnd = nullptr;
    g_prevShowImGui = false;
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


static void RecordConstantUpload(ConstantUploadStage stage,
                                 uintptr_t shaderKey,
                                 UINT startRegister,
                                 UINT vectorCount) {
    ConstantUploadEvent ev = {};
    ev.stage = stage;
    ev.shaderKey = shaderKey;
    ev.shaderHash = GetShaderHashForKey(shaderKey);
    ev.startRegister = startRegister;
    ev.vectorCount = vectorCount;
    ev.changeSerial = ++g_constantUploadSerial;

    std::lock_guard<std::mutex> lock(g_uiDataMutex);
    g_constantUploadEvents.emplace_back(ev);
    while (g_constantUploadEvents.size() > kMaxConstantUploadEvents) {
        g_constantUploadEvents.pop_front();
    }
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
    snprintf(outLabel, outSize, "%p (hash 0x%08X)%s%s%s",
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
                               bool manual,
                               const char* sourceLabel,
                               int extractedFromRegister) {
    MatrixSourceInfo info = {};
    info.valid = true;
    info.manual = manual;
    info.shaderKey = shaderKey;
    info.shaderHash = GetShaderHashForKey(shaderKey);
    info.baseRegister = baseRegister;
    info.rows = rows;
    info.transposed = transposed;
    info.sourceLabel = sourceLabel ? sourceLabel : (manual ? "manual constants selection" : "auto/config detection");
    info.extractedFromRegister = extractedFromRegister >= 0 ? extractedFromRegister : baseRegister;
    std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
    g_matrixSources[slot] = info;
}

static void DrawMatrixSourceInfo(MatrixSlot slot, bool available) {
    if (!available) {
        return;
    }

    MatrixSourceInfo source = {};
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        source = g_matrixSources[slot];
    }
    if (!source.valid) {
        ImGui::Text("Source: <unknown>");
        return;
    }

    uint32_t shaderHash = 0;
    bool hasBytecodeHash = TryGetShaderBytecodeHash(source.shaderKey, &shaderHash);
    if (source.shaderKey == 0) {
        ImGui::Text("Source shader: <none/runtime>");
    } else {
        ImGui::Text("Source shader: %p", reinterpret_cast<void*>(source.shaderKey));
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
        ImGui::Text("Stored from: c%d-c%d (%d rows)%s",
                    source.baseRegister,
                    source.baseRegister + (rows - 1),
                    rows,
                    source.transposed ? " [transposed]" : "");
    } else {
        ImGui::Text("Stored from: <not from shader constants>");
    }

    if (source.extractedFromRegister >= 0 && source.extractedFromRegister != source.baseRegister) {
        ImGui::Text("Extracted from: c%d", source.extractedFromRegister);
    }

    ImGui::Text("Origin: %s", source.sourceLabel ? source.sourceLabel : "unknown");
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
        case MatrixSlot_VP: return "VP";
        case MatrixSlot_WV: return "WV";
        default: return "UNKNOWN";
    }
}

static bool SaveConfigRegisterValue(const char* key, int value) {
    if (g_iniPath[0] == '\0') return false;
    char valueBuf[32] = {};
    snprintf(valueBuf, sizeof(valueBuf), "%d", value);
    return WritePrivateProfileStringA("CameraProxy", key, valueBuf, g_iniPath) != FALSE;
}

static bool SaveConfigBoolValue(const char* key, bool value) {
    return SaveConfigRegisterValue(key, value ? 1 : 0);
}

static bool SaveConfigFloatValue(const char* key, float value) {
    if (g_iniPath[0] == '\0') return false;
    char valueBuf[64] = {};
    snprintf(valueBuf, sizeof(valueBuf), "%.7g", value);
    return WritePrivateProfileStringA("CameraProxy", key, valueBuf, g_iniPath) != FALSE;
}

static bool ConsumeSingleKeyHotkey(HotkeyAction action, int virtualKey) {
    if (action < 0 || action >= HotkeyAction_Count || virtualKey <= 0) {
        return false;
    }
    bool keyDown = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    bool pressed = keyDown && !g_hotkeyWasDown[action];
    g_hotkeyWasDown[action] = keyDown;
    return pressed;
}

static void ResetMatrixRegisterOverridesToAuto() {
    g_config.worldMatrixRegister = -1;
    g_config.viewMatrixRegister = -1;
    g_config.projMatrixRegister = -1;
    memset(g_manualBindings, 0, sizeof(g_manualBindings));

    bool savedWorld = SaveConfigRegisterValue("WorldMatrixRegister", -1);
    bool savedView = SaveConfigRegisterValue("ViewMatrixRegister", -1);
    bool savedProj = SaveConfigRegisterValue("ProjMatrixRegister", -1);
    bool savedAll = savedWorld && savedView && savedProj;
    if (g_config.autoDetectMatrices) {
        snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                 "Cleared matrix register overrides. Falling back to deterministic auto-detect (AutoDetectMatrices=1).%s",
                 savedAll ? "" : " Failed to persist at least one key to camera_proxy.ini.");
    } else {
        snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                 "Cleared matrix register overrides. Runtime now uses structural detection.%s",
                 savedAll ? "" : " Failed to persist at least one key to camera_proxy.ini.");
    }
    LogMsg("Matrix register overrides reset to auto (AutoDetectMatrices=%s).", g_config.autoDetectMatrices ? "on" : "off");
}

static void PinRegisterFromSource(MatrixSlot slot) {
    MatrixSourceInfo source = {};
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        source = g_matrixSources[slot];
    }
    if (!source.valid || source.baseRegister < 0) {
        snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                 "Cannot pin %s: no register-backed source available.", MatrixSlotLabel(slot));
        return;
    }

    const char* key = nullptr;
    int* target = nullptr;
    if (slot == MatrixSlot_View) {
        key = "ViewMatrixRegister";
        target = &g_config.viewMatrixRegister;
    } else if (slot == MatrixSlot_Projection) {
        key = "ProjMatrixRegister";
        target = &g_config.projMatrixRegister;
    } else if (slot == MatrixSlot_World) {
        key = "WorldMatrixRegister";
        target = &g_config.worldMatrixRegister;
    }

    if (!key || !target) {
        snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                 "Pinning is not supported for %s.", MatrixSlotLabel(slot));
        return;
    }

    *target = source.baseRegister;
    if (SaveConfigRegisterValue(key, source.baseRegister)) {
        snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                 "Pinned %s register to c%d and saved to camera_proxy.ini (%s).",
                 MatrixSlotLabel(slot), source.baseRegister, key);
    } else {
        snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                 "Pinned %s register to c%d (failed to save camera_proxy.ini).",
                 MatrixSlotLabel(slot), source.baseRegister);
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
    } else if (slot == MatrixSlot_VP) {
        StoreVPMatrix(mat, shaderKey, baseRegister, rows, false, true);
    } else if (slot == MatrixSlot_WV) {
        StoreWVMatrix(mat, shaderKey, baseRegister, rows, false, true);
    }

    snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
             "Assigned %s from shader %p registers c%d-c%d (%d rows).",
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
    for (int i = 0; i < kMaxConstantRegisters; ++i) inserted.first->second.overrideExpiresAtFrame[i] = -1;
    return &inserted.first->second;
}


static void OnVertexShaderReleased(uintptr_t shaderKey) {
    if (shaderKey == 0) {
        return;
    }
    g_shaderConstants.erase(shaderKey);
    g_disabledShaders.erase(shaderKey);
    g_shaderBytecodeHashes.erase(shaderKey);
    auto it = std::find(g_shaderOrder.begin(), g_shaderOrder.end(), shaderKey);
    if (it != g_shaderOrder.end()) {
        g_shaderOrder.erase(it);
    }
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

static bool ViewMatrixCanUseFastInverse(const D3DMATRIX& view) {
    const float row0len = sqrtf(Dot3(view._11, view._12, view._13, view._11, view._12, view._13));
    const float row1len = sqrtf(Dot3(view._21, view._22, view._23, view._21, view._22, view._23));
    const float row2len = sqrtf(Dot3(view._31, view._32, view._33, view._31, view._32, view._33));

    if (fabsf(row0len - 1.0f) > 0.05f || fabsf(row1len - 1.0f) > 0.05f || fabsf(row2len - 1.0f) > 0.05f) {
        return false;
    }
    if (fabsf(Dot3(view._11, view._12, view._13, view._21, view._22, view._23)) > 0.05f) return false;
    if (fabsf(Dot3(view._11, view._12, view._13, view._31, view._32, view._33)) > 0.05f) return false;
    if (fabsf(Dot3(view._21, view._22, view._23, view._31, view._32, view._33)) > 0.05f) return false;
    if (fabsf(view._14) > 0.01f || fabsf(view._24) > 0.01f || fabsf(view._34) > 0.01f || fabsf(view._44 - 1.0f) > 0.01f) {
        return false;
    }
    return true;
}

static bool TryBuildWorldFromView(const D3DMATRIX& view, bool preferFastInverse, D3DMATRIX* outWorld,
                                  bool* outUsedFast, bool* outFastEligible) {
    if (!outWorld) {
        return false;
    }

    const bool fastEligible = ViewMatrixCanUseFastInverse(view);
    if (outFastEligible) {
        *outFastEligible = fastEligible;
    }

    if (preferFastInverse && fastEligible) {
        *outWorld = InvertSimpleRigidView(view);
        if (outUsedFast) {
            *outUsedFast = true;
        }
        return true;
    }

    if (outUsedFast) {
        *outUsedFast = false;
    }
    return InvertMatrix4x4Deterministic(view, outWorld, nullptr);
}

static const char* GameProfileLabel(GameProfileKind profile) {
    switch (profile) {
        case GameProfile_Barnyard: return "Barnyard";
        case GameProfile_DevilMayCry4: return "DevilMayCry4";
        case GameProfile_MetalGearRising: return "MetalGearRising";
        case GameProfile_None:
        default:
            return "None";
    }
}

static GameProfileKind ParseGameProfile(const char* profileName) {
    if (!profileName || profileName[0] == '\0') {
        return GameProfile_None;
    }
    if (_stricmp(profileName, "MetalGearRising") == 0 ||
        _stricmp(profileName, "MGR") == 0 ||
        _stricmp(profileName, "MetalGearRisingRevengeance") == 0) {
        return GameProfile_MetalGearRising;
    }
    if (_stricmp(profileName, "DevilMayCry4") == 0 ||
        _stricmp(profileName, "DMC4") == 0 ||
        _stricmp(profileName, "DevilMayCry4Original") == 0) {
        return GameProfile_DevilMayCry4;
    }
    if (_stricmp(profileName, "Barnyard") == 0 ||
        _stricmp(profileName, "Barnyard2006") == 0) {
        return GameProfile_Barnyard;
    }
    return GameProfile_None;
}

static void ConfigureActiveProfileLayout() {
    g_profileLayout = {};
    if (g_activeGameProfile == GameProfile_MetalGearRising) {
        g_profileLayout.projectionBase = 4;
        g_profileLayout.viewProjectionBase = 8;
        g_profileLayout.viewInverseBase = 12;
        g_profileLayout.worldBase = 16;
        g_profileLayout.worldViewBase = 20;
    } else if (g_activeGameProfile == GameProfile_DevilMayCry4) {
        // Original DMC4 fixed layout.
        g_profileLayout.combinedMvpBase = 0;
        g_profileLayout.worldBase = 0;
        g_profileLayout.viewInverseBase = 4;
        g_profileLayout.projectionBase = 8;
    }
}

static bool InvertMatrix4x4Deterministic(const D3DMATRIX& in, D3DMATRIX* out, float* outDeterminant) {
    if (!out) {
        return false;
    }

    const float* m = reinterpret_cast<const float*>(&in);
    float inv[16] = {};

    inv[0] = m[5]  * m[10] * m[15] -
             m[5]  * m[11] * m[14] -
             m[9]  * m[6]  * m[15] +
             m[9]  * m[7]  * m[14] +
             m[13] * m[6]  * m[11] -
             m[13] * m[7]  * m[10];

    inv[4] = -m[4]  * m[10] * m[15] +
              m[4]  * m[11] * m[14] +
              m[8]  * m[6]  * m[15] -
              m[8]  * m[7]  * m[14] -
              m[12] * m[6]  * m[11] +
              m[12] * m[7]  * m[10];

    inv[8] = m[4]  * m[9] * m[15] -
             m[4]  * m[11] * m[13] -
             m[8]  * m[5] * m[15] +
             m[8]  * m[7] * m[13] +
             m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];

    inv[12] = -m[4]  * m[9] * m[14] +
               m[4]  * m[10] * m[13] +
               m[8]  * m[5] * m[14] -
               m[8]  * m[6] * m[13] -
               m[12] * m[5] * m[10] +
               m[12] * m[6] * m[9];

    inv[1] = -m[1]  * m[10] * m[15] +
              m[1]  * m[11] * m[14] +
              m[9]  * m[2] * m[15] -
              m[9]  * m[3] * m[14] -
              m[13] * m[2] * m[11] +
              m[13] * m[3] * m[10];

    inv[5] = m[0]  * m[10] * m[15] -
             m[0]  * m[11] * m[14] -
             m[8]  * m[2] * m[15] +
             m[8]  * m[3] * m[14] +
             m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];

    inv[9] = -m[0]  * m[9] * m[15] +
              m[0]  * m[11] * m[13] +
              m[8]  * m[1] * m[15] -
              m[8]  * m[3] * m[13] -
              m[12] * m[1] * m[11] +
              m[12] * m[3] * m[9];

    inv[13] = m[0]  * m[9] * m[14] -
              m[0]  * m[10] * m[13] -
              m[8]  * m[1] * m[14] +
              m[8]  * m[2] * m[13] +
              m[12] * m[1] * m[10] -
              m[12] * m[2] * m[9];

    inv[2] = m[1]  * m[6] * m[15] -
             m[1]  * m[7] * m[14] -
             m[5]  * m[2] * m[15] +
             m[5]  * m[3] * m[14] +
             m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];

    inv[6] = -m[0]  * m[6] * m[15] +
              m[0]  * m[7] * m[14] +
              m[4]  * m[2] * m[15] -
              m[4]  * m[3] * m[14] -
              m[12] * m[2] * m[7] +
              m[12] * m[3] * m[6];

    inv[10] = m[0]  * m[5] * m[15] -
              m[0]  * m[7] * m[13] -
              m[4]  * m[1] * m[15] +
              m[4]  * m[3] * m[13] +
              m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];

    inv[14] = -m[0]  * m[5] * m[14] +
               m[0]  * m[6] * m[13] +
               m[4]  * m[1] * m[14] -
               m[4]  * m[2] * m[13] -
               m[12] * m[1] * m[6] +
               m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] +
              m[1] * m[7] * m[10] +
              m[5] * m[2] * m[11] -
              m[5] * m[3] * m[10] -
              m[9] * m[2] * m[7] +
              m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] -
             m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] +
               m[0] * m[7] * m[9] +
               m[4] * m[1] * m[11] -
               m[4] * m[3] * m[9] -
               m[8] * m[1] * m[7] +
               m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] -
              m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (outDeterminant) {
        *outDeterminant = det;
    }
    if (fabsf(det) <= 1e-8f) {
        return false;
    }

    const float detInv = 1.0f / det;
    float* dst = reinterpret_cast<float*>(out);
    for (int i = 0; i < 16; i++) {
        dst[i] = inv[i] * detInv;
    }
    return true;
}

static void ClearAllShaderOverrides() {
    for (auto& entry : g_shaderConstants) {
        ShaderConstantState& state = entry.second;
        memset(state.overrideConstants, 0, sizeof(state.overrideConstants));
        memset(state.overrideValid, 0, sizeof(state.overrideValid));
        for (int i = 0; i < kMaxConstantRegisters; ++i) state.overrideExpiresAtFrame[i] = -1;
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
    state->overrideExpiresAtFrame[reg] = -1;
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

        if (state.overrideExpiresAtFrame[reg] >= 0 &&
            g_frameCount >= state.overrideExpiresAtFrame[reg]) {
            state.overrideValid[reg] = false;
            state.overrideExpiresAtFrame[reg] = -1;
            continue;
        }
    }
    return true;
}

static void UpdateVariance(ShaderConstantState& state, int reg, const float* values) {
    state.sampleCounts[reg]++;
    for (int i = 0; i < 4; i++) {
        double value = static_cast<double>(values[i]);
        double delta = value - state.mean[reg][i];
        state.mean[reg][i] += delta / static_cast<double>(state.sampleCounts[reg]);
        double delta2 = value - state.mean[reg][i];
        state.m2[reg][i] += delta * delta2;
    }
}

static float GetVarianceMagnitude(const ShaderConstantState& state, int reg) {
    if (state.sampleCounts[reg] < 2) {
        return 0.0f;
    }
    double sum = 0.0;
    for (int i = 0; i < 4; i++) {
        sum += state.m2[reg][i] / static_cast<double>(state.sampleCounts[reg] - 1);
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

static bool TryBuildMatrixFromGlobalRegisters(int baseRegister,
                                              int rows,
                                              bool transposed,
                                              D3DMATRIX* outMatrix) {
    if (!outMatrix || baseRegister < 0 || rows < 3 || rows > 4 ||
        baseRegister + rows - 1 >= kMaxConstantRegisters) {
        return false;
    }

    float m[16] = {};
    for (int i = 0; i < rows; i++) {
        const GlobalVertexRegisterState& globalState = g_allVertexRegisters[baseRegister + i];
        if (!globalState.valid) {
            return false;
        }
        memcpy(&m[i * 4], globalState.value, sizeof(globalState.value));
    }

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

static void UpdateHotkeys() {
    EnsureWndProcHookInstalled();

    if (ConsumeSingleKeyHotkey(HotkeyAction_ToggleMenu, g_config.hotkeyToggleMenuVk)) {
        g_showImGui = !g_showImGui;
    }

    if (ConsumeSingleKeyHotkey(HotkeyAction_TogglePause, g_config.hotkeyTogglePauseVk)) {
        g_pauseRendering = !g_pauseRendering;
    }

    if (ConsumeSingleKeyHotkey(HotkeyAction_EmitMatrices, g_config.hotkeyEmitMatricesVk)) {
        if (!g_config.emitFixedFunctionTransforms) {
            snprintf(g_manualEmitStatus, sizeof(g_manualEmitStatus),
                     "Blocked: set EmitFixedFunctionTransforms=1 in camera_proxy.ini first.");
        } else {
            g_requestManualEmit = true;
            snprintf(g_manualEmitStatus, sizeof(g_manualEmitStatus),
                     "Pending (hotkey): pass cached World/View/Projection matrices to RTX Remix this frame.");
        }
    }

    if (ConsumeSingleKeyHotkey(HotkeyAction_ResetMatrixOverrides, g_config.hotkeyResetMatrixOverridesVk)) {
        ResetMatrixRegisterOverridesToAuto();
    }

    if (g_showImGui && !g_prevShowImGui) {
        // Menu just became visible — release any game mouse capture.
        ReleaseCapture();
    }
    g_prevShowImGui = g_showImGui;
    UpdateInputBlockHooks();
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
    EnsureWndProcHookInstalled();

    if (!g_imguiInitialized || !g_showImGui) {
        g_constantUploadRecordingEnabled = false;
        return;
    }

    ClipCursor(NULL);

    CameraMatrices camSnapshot = {};
    {
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        camSnapshot = g_cameraMatrices;
    }

    ApplyImGuiScale(g_imguiHwnd);
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    UpdateImGuiKeyboardFromAsyncState();
    if (g_showImGui) {
        POINT cursorScreen = {};
        if (GetCursorPos(&cursorScreen) && ScreenToClient(g_imguiHwnd, &cursorScreen)) {
            ImGui::GetIO().MousePos = ImVec2(
                static_cast<float>(cursorScreen.x),
                static_cast<float>(cursorScreen.y));
        }
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDown[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        io.MouseDown[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

        io.KeyCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        io.KeyShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        io.KeyAlt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

        static const int kNavKeys[] = {
            VK_TAB, VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN,
            VK_PRIOR, VK_NEXT, VK_HOME, VK_END, VK_INSERT, VK_DELETE,
            VK_BACK, VK_RETURN, VK_ESCAPE, VK_SPACE
        };
        for (int vk : kNavKeys) {
            io.KeysDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
    }
    ImGui::GetIO().MouseDrawCursor = true;
    ImGui::NewFrame();

    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("Camera Proxy for RTX Remix", nullptr,
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("Hotkeys: Toggle menu(F10) Pause(F9) Emit matrices(F8) Reset matrix overrides(F7)");
    if (ImGui::Button(g_pauseRendering ? "Resume game rendering" : "Pause game rendering")) {
        g_pauseRendering = !g_pauseRendering;
    }
    ImGui::SameLine();
    ImGui::Text("Status: %s", g_pauseRendering ? "Paused" : "Running");
    ImGui::TextWrapped("This proxy detects World, View, and Projection matrices from shader constants and forwards them "
                       "to the RTX Remix runtime through SetTransform() so Remix gets camera data in D3D9 titles.");

    if (ImGui::SliderFloat("UI scale", &g_imguiScaleRuntime, 0.5f, 3.0f, "%.2fx")) {
        ApplyImGuiScale(g_imguiHwnd);
        g_config.imguiScale = g_imguiScaleRuntime;
    }

    if (ImGui::Checkbox("Disable game input while menu is open", &g_imguiDisableGameInputWhileMenuOpen)) {
        g_config.disableGameInputWhileMenuOpen = g_imguiDisableGameInputWhileMenuOpen;
        SaveConfigBoolValue("DisableGameInputWhileMenuOpen", g_config.disableGameInputWhileMenuOpen);
        UpdateInputBlockHooks();
    }

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
            ImGui::Text("Active game profile: %s", GameProfileLabel(g_activeGameProfile));
            ImGui::Text("Game SetTransform seen: WORLD=%s VIEW=%s PROJECTION=%s",
                        g_gameSetTransformSeen[0] ? "yes" : "no",
                        g_gameSetTransformSeen[1] ? "yes" : "no",
                        g_gameSetTransformSeen[2] ? "yes" : "no");
            if (g_gameSetTransformAnySeen) {
                if (ImGui::Checkbox("Bypass proxy WVP emit when game provides SetTransform", &g_config.setTransformBypassProxyWhenGameProvides)) {
                    SaveConfigBoolValue("SetTransformBypassProxyWhenGameProvides",
                                        g_config.setTransformBypassProxyWhenGameProvides);
                }
                if (ImGui::Checkbox("Round-trip game SetTransform via GetTransform for strict compatibility", &g_config.setTransformRoundTripCompatibilityMode)) {
                    SaveConfigBoolValue("SetTransformRoundTripCompatibilityMode",
                                        g_config.setTransformRoundTripCompatibilityMode);
                }
            } else {
                ImGui::TextDisabled("SetTransform compatibility options unlock once game calls SetTransform(WORLD/VIEW/PROJECTION).");
            }
            if (g_activeGameProfile == GameProfile_MetalGearRising) {
                ImGui::Text("MGR layout: Proj=c4-c7, ViewProjection=c8-c11, World=c16-c19");
                ImGui::Checkbox("Use auto projection when c4 is invalid", &g_imguiMgrrUseAutoProjection);
                if (g_imguiMgrrUseAutoProjection != g_config.mgrrUseAutoProjectionWhenC4Invalid) {
                    g_config.mgrrUseAutoProjectionWhenC4Invalid = g_imguiMgrrUseAutoProjection;
                    SaveConfigBoolValue("MGRRUseAutoProjectionWhenC4Invalid",
                                        g_config.mgrrUseAutoProjectionWhenC4Invalid);
                }
                ImGui::Text("Projection c4-c7 validity: %s", g_mgrProjectionRegisterValid ? "valid" : "invalid");
                float activeViewDeterminant = 0.0f;
                D3DMATRIX activeViewInverse = {};
                InvertMatrix4x4Deterministic(camSnapshot.view, &activeViewInverse, &activeViewDeterminant);
                ImGui::Text("Determinant of active View matrix: %.6f", activeViewDeterminant);
                ImGui::Text("Captured this frame: Proj=%s View=%s",
                            g_mgrProjCapturedThisFrame ? "yes" : "no",
                            g_mgrViewCapturedThisFrame ? "yes" : "no");
                ImGui::Text("Captured for draw: World=%s", g_mgrWorldCapturedForDraw ? "yes" : "no");
                ImGui::Text("Core seen: Proj=%s ViewProj=%s World=%s",
                            g_profileCoreRegistersSeen[0] ? "yes" : "no",
                            g_profileCoreRegistersSeen[1] ? "yes" : "no",
                            g_profileCoreRegistersSeen[2] ? "yes" : "no");
                ImGui::Text("View source: %s", g_profileViewDerivedFromInverse ? "Derived from VP via inverse projection" : "Not yet derived");
                if (g_profileStatusMessage[0] != '\0') {
                    ImGui::TextWrapped("%s", g_profileStatusMessage);
                }
            }
            else if (g_activeGameProfile == GameProfile_DevilMayCry4) {
                ImGui::Text("DMC4 layout: MVP=c0-c3, World=c0-c3, View=c4-c7, Projection=c8-c11");
                ImGui::Text("Core seen: MVP/World=%s View=%s Projection=%s",
                            g_profileCoreRegistersSeen[0] ? "yes" : "no",
                            g_profileCoreRegistersSeen[1] ? "yes" : "no",
                            g_profileCoreRegistersSeen[2] ? "yes" : "no");
                if (g_profileStatusMessage[0] != '\0') {
                    ImGui::TextWrapped("%s", g_profileStatusMessage);
                }
            }
            else if (g_activeGameProfile == GameProfile_Barnyard) {
                ImGui::Text("Barnyard profile: WORLD from VS constants; VIEW/PROJECTION via intercepted game SetTransform");
                if (ImGui::Checkbox("Use intercepted game View/Projection SetTransform", &g_imguiBarnyardUseGameSetTransformsForViewProjection)) {
                    g_config.barnyardUseGameSetTransformsForViewProjection = g_imguiBarnyardUseGameSetTransformsForViewProjection;
                    SaveConfigBoolValue("BarnyardUseGameSetTransformsForViewProjection",
                                        g_config.barnyardUseGameSetTransformsForViewProjection);
                }
                if (ImGui::Checkbox("Always use c0-c3 as World", &g_barnyardForceWorldFromC0)) {
                    SaveConfigBoolValue("BarnyardForceWorldFromC0", g_barnyardForceWorldFromC0);
                }
                ImGui::Text("Seen: View=%s Projection=%s World=%s",
                            g_profileCoreRegistersSeen[0] ? "yes" : "no",
                            g_profileCoreRegistersSeen[1] ? "yes" : "no",
                            g_profileCoreRegistersSeen[2] ? "yes" : "no");
                if (g_profileStatusMessage[0] != '\0') {
                    ImGui::TextWrapped("%s", g_profileStatusMessage);
                }
            }
            ImGui::Separator();
            DrawMatrixWithTranspose("World", camSnapshot.world, camSnapshot.hasWorld,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_World, camSnapshot.hasWorld);
            ImGui::Separator();
            DrawMatrixWithTranspose("View", camSnapshot.view, camSnapshot.hasView,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_View, camSnapshot.hasView);
            if (ImGui::CollapsingHeader("Experimental inverse View -> World", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Checkbox("Use inverse(View) as emitted World matrix", &g_config.experimentalInverseViewAsWorld)) {
                    SaveConfigBoolValue("ExperimentalInverseViewAsWorld", g_config.experimentalInverseViewAsWorld);
                }
                if (ImGui::Checkbox("Allow inverse(View) even if strict validity check fails", &g_config.experimentalInverseViewAsWorldAllowUnverified)) {
                    SaveConfigBoolValue("ExperimentalInverseViewAsWorldAllowUnverified",
                                        g_config.experimentalInverseViewAsWorldAllowUnverified);
                }
                if (ImGui::Checkbox("Fast inverse (rigid transform only)", &g_config.experimentalInverseViewAsWorldFast)) {
                    SaveConfigBoolValue("ExperimentalInverseViewAsWorldFast",
                                        g_config.experimentalInverseViewAsWorldFast);
                }
                ImGui::TextWrapped("Fast inverse assumes no scaling/shear: transpose the 3x3 rotation and recompute translation via negative dot products.");
                ImGui::Text("Last strict validity result: %s", g_lastInverseViewAsWorldEligible ? "valid" : "invalid");
                ImGui::Text("Last inverse(View)->World application: %s", g_lastInverseViewAsWorldApplied ? "applied" : "not applied");
                ImGui::Text("Last method: %s", g_lastInverseViewAsWorldUsedFast ? "fast inverse" : "full 4x4 inverse");
            }
            ImGui::Separator();
            DrawMatrixWithTranspose("Projection", camSnapshot.projection,
                                    camSnapshot.hasProjection, g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_Projection, camSnapshot.hasProjection);
            if (g_projectionDetectedByNumericStructure) {
                ImGui::Text("Projection numeric detection: ACTIVE");
                ImGui::Text("FOV: %.2f deg (%.3f rad)",
                            g_projectionDetectedFovRadians * 180.0f / 3.14159265f,
                            g_projectionDetectedFovRadians);
                ImGui::Text("Handedness: %s", ProjectionHandednessLabel(g_projectionDetectedHandedness));
                if (g_projectionDetectedRegister >= 0) {
                    ImGui::Text("Detected register: c%d", g_projectionDetectedRegister);
                }
            } else {
                ImGui::Text("Projection numeric detection: waiting for structural match");
            }

            if (ImGui::CollapsingHeader("Experimental custom projection", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextWrapped("This feature is experimental and must be enabled in camera_proxy.ini. "
                                   "By default it only supplies projection when no register-sourced projection is available.");
                ImGui::Text("Enabled in ini: %s", g_config.experimentalCustomProjectionEnabled ? "yes" : "no");
                if (!g_config.experimentalCustomProjectionEnabled) {
                    ImGui::TextWrapped("Set ExperimentalCustomProjectionEnabled=1 in camera_proxy.ini to activate this section.");
                } else {
                    if (ImGui::RadioButton("Manual matrix", g_config.experimentalCustomProjectionMode == CustomProjectionMode_Manual)) {
                        g_config.experimentalCustomProjectionMode = CustomProjectionMode_Manual;
                        SaveConfigRegisterValue("ExperimentalCustomProjectionMode", g_config.experimentalCustomProjectionMode);
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("Auto-generate", g_config.experimentalCustomProjectionMode == CustomProjectionMode_Auto)) {
                        g_config.experimentalCustomProjectionMode = CustomProjectionMode_Auto;
                        SaveConfigRegisterValue("ExperimentalCustomProjectionMode", g_config.experimentalCustomProjectionMode);
                    }

                    if (ImGui::Checkbox("Override detected projection", &g_config.experimentalCustomProjectionOverrideDetectedProjection)) {
                        SaveConfigBoolValue("ExperimentalCustomProjectionOverrideDetectedProjection",
                                            g_config.experimentalCustomProjectionOverrideDetectedProjection);
                    }
                    if (ImGui::Checkbox("Override combined MVP-derived projection", &g_config.experimentalCustomProjectionOverrideCombinedMVP)) {
                        SaveConfigBoolValue("ExperimentalCustomProjectionOverrideCombinedMVP",
                                            g_config.experimentalCustomProjectionOverrideCombinedMVP);
                    }

                    if (g_config.experimentalCustomProjectionMode == CustomProjectionMode_Manual) {
                        float rows[4][4] = {
                            { g_config.experimentalCustomProjectionManualMatrix._11, g_config.experimentalCustomProjectionManualMatrix._12, g_config.experimentalCustomProjectionManualMatrix._13, g_config.experimentalCustomProjectionManualMatrix._14 },
                            { g_config.experimentalCustomProjectionManualMatrix._21, g_config.experimentalCustomProjectionManualMatrix._22, g_config.experimentalCustomProjectionManualMatrix._23, g_config.experimentalCustomProjectionManualMatrix._24 },
                            { g_config.experimentalCustomProjectionManualMatrix._31, g_config.experimentalCustomProjectionManualMatrix._32, g_config.experimentalCustomProjectionManualMatrix._33, g_config.experimentalCustomProjectionManualMatrix._34 },
                            { g_config.experimentalCustomProjectionManualMatrix._41, g_config.experimentalCustomProjectionManualMatrix._42, g_config.experimentalCustomProjectionManualMatrix._43, g_config.experimentalCustomProjectionManualMatrix._44 }
                        };
                        bool edited = false;
                        edited |= ImGui::InputFloat4("ExpProj row1", rows[0], "%.6f");
                        edited |= ImGui::InputFloat4("ExpProj row2", rows[1], "%.6f");
                        edited |= ImGui::InputFloat4("ExpProj row3", rows[2], "%.6f");
                        edited |= ImGui::InputFloat4("ExpProj row4", rows[3], "%.6f");
                        if (edited) {
                            g_config.experimentalCustomProjectionManualMatrix._11 = rows[0][0]; g_config.experimentalCustomProjectionManualMatrix._12 = rows[0][1]; g_config.experimentalCustomProjectionManualMatrix._13 = rows[0][2]; g_config.experimentalCustomProjectionManualMatrix._14 = rows[0][3];
                            g_config.experimentalCustomProjectionManualMatrix._21 = rows[1][0]; g_config.experimentalCustomProjectionManualMatrix._22 = rows[1][1]; g_config.experimentalCustomProjectionManualMatrix._23 = rows[1][2]; g_config.experimentalCustomProjectionManualMatrix._24 = rows[1][3];
                            g_config.experimentalCustomProjectionManualMatrix._31 = rows[2][0]; g_config.experimentalCustomProjectionManualMatrix._32 = rows[2][1]; g_config.experimentalCustomProjectionManualMatrix._33 = rows[2][2]; g_config.experimentalCustomProjectionManualMatrix._34 = rows[2][3];
                            g_config.experimentalCustomProjectionManualMatrix._41 = rows[3][0]; g_config.experimentalCustomProjectionManualMatrix._42 = rows[3][1]; g_config.experimentalCustomProjectionManualMatrix._43 = rows[3][2]; g_config.experimentalCustomProjectionManualMatrix._44 = rows[3][3];
                            const float* values = reinterpret_cast<const float*>(&g_config.experimentalCustomProjectionManualMatrix);
                            for (int i = 0; i < 16; ++i) {
                                int row = (i / 4) + 1;
                                int col = (i % 4) + 1;
                                char key[64] = {};
                                snprintf(key, sizeof(key), "ExperimentalCustomProjectionM%d%d", row, col);
                                SaveConfigFloatValue(key, values[i]);
                            }
                        }
                    } else {
                        if (ImGui::SliderFloat("Auto FOV (deg)", &g_config.experimentalCustomProjectionAutoFovDeg, 1.0f, 179.0f, "%.2f")) {
                            SaveConfigFloatValue("ExperimentalCustomProjectionAutoFovDeg", g_config.experimentalCustomProjectionAutoFovDeg);
                        }
                        if (ImGui::InputFloat("Auto Near Z", &g_config.experimentalCustomProjectionAutoNearZ, 0.01f, 0.1f, "%.6f")) {
                            SaveConfigFloatValue("ExperimentalCustomProjectionAutoNearZ", g_config.experimentalCustomProjectionAutoNearZ);
                        }
                        if (ImGui::InputFloat("Auto Far Z", &g_config.experimentalCustomProjectionAutoFarZ, 1.0f, 10.0f, "%.3f")) {
                            SaveConfigFloatValue("ExperimentalCustomProjectionAutoFarZ", g_config.experimentalCustomProjectionAutoFarZ);
                        }
                        if (ImGui::InputFloat("Aspect fallback", &g_config.experimentalCustomProjectionAutoAspectFallback, 0.01f, 0.1f, "%.6f")) {
                            SaveConfigFloatValue("ExperimentalCustomProjectionAutoAspectFallback", g_config.experimentalCustomProjectionAutoAspectFallback);
                        }
                        int handednessIndex = g_config.experimentalCustomProjectionAutoHandedness == ProjectionHandedness_Right ? 1 : 0;
                        if (ImGui::RadioButton("Left-handed", handednessIndex == 0)) {
                            g_config.experimentalCustomProjectionAutoHandedness = ProjectionHandedness_Left;
                            SaveConfigRegisterValue("ExperimentalCustomProjectionAutoHandedness", g_config.experimentalCustomProjectionAutoHandedness);
                        }
                        ImGui::SameLine();
                        if (ImGui::RadioButton("Right-handed", handednessIndex == 1)) {
                            g_config.experimentalCustomProjectionAutoHandedness = ProjectionHandedness_Right;
                            SaveConfigRegisterValue("ExperimentalCustomProjectionAutoHandedness", g_config.experimentalCustomProjectionAutoHandedness);
                        }
                    }

                    if (g_customProjectionStatus[0] != '\0') {
                        ImGui::TextWrapped("%s", g_customProjectionStatus);
                    }
                }
            }

            ImGui::Separator();
            DrawMatrixWithTranspose("MVP", camSnapshot.mvp, camSnapshot.hasMVP,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_MVP, camSnapshot.hasMVP);
            DrawMatrixWithTranspose("VP", camSnapshot.vp, camSnapshot.hasVP,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_VP, camSnapshot.hasVP);
            DrawMatrixWithTranspose("WV", camSnapshot.wv, camSnapshot.hasWV,
                                    g_showTransposedMatrices);
            DrawMatrixSourceInfo(MatrixSlot_WV, camSnapshot.hasWV);

            if (ImGui::CollapsingHeader("Combined MVP handling", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Checkbox("Enable Combined MVP", &g_config.enableCombinedMVP)) {
                    SaveConfigBoolValue("EnableCombinedMVP", g_config.enableCombinedMVP);
                }
                if (ImGui::Checkbox("Require World", &g_config.combinedMVPRequireWorld)) {
                    SaveConfigBoolValue("CombinedMVPRequireWorld", g_config.combinedMVPRequireWorld);
                }
                if (ImGui::Checkbox("Assume Identity World", &g_config.combinedMVPAssumeIdentityWorld)) {
                    SaveConfigBoolValue("CombinedMVPAssumeIdentityWorld", g_config.combinedMVPAssumeIdentityWorld);
                }
                if (ImGui::Checkbox("Force Decomposition", &g_config.combinedMVPForceDecomposition)) {
                    SaveConfigBoolValue("CombinedMVPForceDecomposition", g_config.combinedMVPForceDecomposition);
                }
                if (ImGui::Checkbox("Log Decomposition", &g_config.combinedMVPLogDecomposition)) {
                    SaveConfigBoolValue("CombinedMVPLogDecomposition", g_config.combinedMVPLogDecomposition);
                }

                ImGui::Separator();
                ImGui::Text("Current MVP register: %s", g_combinedMvpDebug.registerBase >= 0 ? "captured" : "n/a");
                if (g_combinedMvpDebug.registerBase >= 0) {
                    ImGui::SameLine();
                    ImGui::Text("(c%d)", g_combinedMvpDebug.registerBase);
                }
                ImGui::Text("Strategy selected: %s", CombinedMVPStrategyLabel(g_combinedMvpDebug.strategy));
                ImGui::Text("Decomposition succeeded: %s", g_combinedMvpDebug.succeeded ? "yes" : "no");
                ImGui::Text("Extracted FOV: %.2f deg", g_combinedMvpDebug.fovRadians * 180.0f / 3.14159265f);
                ImGui::Text("Handedness: %s", ProjectionHandednessLabel(g_combinedMvpDebug.handedness));
            }

            ImGui::Separator();
            ImGui::Text("Register pinning (camera tab)");
            ImGui::TextWrapped("Pin currently detected matrix registers directly from this tab. Values are saved to camera_proxy.ini immediately. Use reset to return to auto-detect.");
            if (ImGui::Button("Pin World register")) {
                PinRegisterFromSource(MatrixSlot_World);
            }
            ImGui::SameLine();
            if (ImGui::Button("Pin View register")) {
                PinRegisterFromSource(MatrixSlot_View);
            }
            ImGui::SameLine();
            if (ImGui::Button("Pin Projection register")) {
                PinRegisterFromSource(MatrixSlot_Projection);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset register overrides to auto")) {
                ResetMatrixRegisterOverridesToAuto();
            }
            ImGui::Text("Pinned registers: World=c%d View=c%d Projection=c%d",
                        g_config.worldMatrixRegister, g_config.viewMatrixRegister,
                        g_config.projMatrixRegister);
            if (g_matrixAssignStatus[0] != '\0') {
                ImGui::TextWrapped("%s", g_matrixAssignStatus);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Constants")) {
            g_constantUploadRecordingEnabled = true;
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

            ImGui::Checkbox("View all VS constant registers (all shaders)", &g_showAllConstantRegisters);

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
                            editState->overrideExpiresAtFrame[g_selectedRegister] = g_frameCount + 1;
                        } else if (g_overrideScopeMode == Override_NFrames) {
                            editState->overrideExpiresAtFrame[g_selectedRegister] = g_frameCount + g_overrideNFrames;
                        } else {
                            editState->overrideExpiresAtFrame[g_selectedRegister] = -1;
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
            if (g_showAllConstantRegisters) {
                bool anyShown = false;
                ImGui::Text("All vertex shader constant registers (all shaders):");
                if (g_showConstantsAsMatrices) {
                    for (int base = 0; base < kMaxConstantRegisters; base += 4) {
                        bool anyValid = false;
                        for (int reg = base; reg < base + 4; reg++) {
                            if (g_allVertexRegisters[reg].valid) {
                                anyValid = true;
                                break;
                            }
                        }
                        if (!anyValid) {
                            continue;
                        }

                        D3DMATRIX mat = {};
                        bool hasMatrix = TryBuildMatrixFromGlobalRegisters(base, 4, false, &mat);
                        bool looksLike = hasMatrix && LooksLikeMatrix(reinterpret_cast<const float*>(&mat));
                        if (g_filterDetectedMatrices && (!hasMatrix || !looksLike)) {
                            continue;
                        }
                        anyShown = true;

                        char label[64] = {};
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
                            for (int reg = base; reg < base + 4; reg++) {
                                char rowLabel[192] = {};
                                const GlobalVertexRegisterState& globalState = g_allVertexRegisters[reg];
                                if (globalState.valid) {
                                    snprintf(rowLabel, sizeof(rowLabel), "c%d: [%.3f %.3f %.3f %.3f]###all_reg_%d",
                                             reg,
                                             globalState.value[0], globalState.value[1],
                                             globalState.value[2], globalState.value[3],
                                             reg);
                                } else {
                                    snprintf(rowLabel, sizeof(rowLabel), "c%d: <unset>###all_reg_%d", reg, reg);
                                }
                                if (ImGui::Selectable(rowLabel, g_selectedRegister == reg)) {
                                    if (globalState.lastShaderKey != 0) {
                                        g_selectedShaderKey = globalState.lastShaderKey;
                                    }
                                    g_selectedRegister = reg;
                                }
                            }

                            int selectedRows = g_manualAssignRows;
                            bool canAssign = (selectedRows == 4)
                                ? TryBuildMatrixFromGlobalRegisters(base, 4, false, &mat)
                                : TryBuildMatrixFromGlobalRegisters(base, 3, false, &mat);
                            if (canAssign) {
                                uintptr_t sourceShaderKey = g_allVertexRegisters[base].lastShaderKey;
                                ImGui::PushID(base + 5000);
                                if (ImGui::Button("Use as World")) {
                                    TryAssignManualMatrixFromSelection(MatrixSlot_World, sourceShaderKey,
                                                                       base, selectedRows, mat);
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Use as View")) {
                                    TryAssignManualMatrixFromSelection(MatrixSlot_View, sourceShaderKey,
                                                                       base, selectedRows, mat);
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Use as Projection")) {
                                    TryAssignManualMatrixFromSelection(MatrixSlot_Projection, sourceShaderKey,
                                                                       base, selectedRows, mat);
                                }
                                ImGui::SameLine();
                                if (ImGui::Button("Use as MVP")) {
                                    TryAssignManualMatrixFromSelection(MatrixSlot_MVP, sourceShaderKey,
                                                                       base, selectedRows, mat);
                                }
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                    }
                } else {
                    for (int reg = 0; reg < kMaxConstantRegisters; ++reg) {
                        const GlobalVertexRegisterState& globalState = g_allVertexRegisters[reg];
                        if (!globalState.valid) {
                            continue;
                        }
                        anyShown = true;
                        char rowLabel[192] = {};
                        snprintf(rowLabel, sizeof(rowLabel), "c%d: [%.3f %.3f %.3f %.3f]###all_reg_%d",
                                 reg,
                                 globalState.value[0], globalState.value[1], globalState.value[2], globalState.value[3],
                                 reg);
                        if (ImGui::Selectable(rowLabel, g_selectedRegister == reg)) {
                            if (globalState.lastShaderKey != 0) {
                                g_selectedShaderKey = globalState.lastShaderKey;
                            }
                            g_selectedRegister = reg;
                        }
                    }
                }
                if (!anyShown) {
                    ImGui::Text("<no vertex shader constants captured yet>");
                }
            } else if (state && state->snapshotReady) {
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
                            StoreViewMatrix(hit.matrix, 0, -1, 4, false, true, "memory scanner");
                            snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                                     "Assigned VIEW from memory scan @ %p (hash 0x%08X).",
                                     reinterpret_cast<void*>(hit.address), hit.hash);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Use as Projection")) {
                            StoreProjectionMatrix(hit.matrix, 0, -1, 4, false, true, "memory scanner");
                            snprintf(g_matrixAssignStatus, sizeof(g_matrixAssignStatus),
                                     "Assigned PROJECTION from memory scan @ %p (hash 0x%08X).",
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
    float sum = 0.0f;
    bool hasNearUnitValue = false;
    for (int i = 0; i < 16; i++) {
        const float v = data[i];
        if (!std::isfinite(v)) return false;
        const float a = fabsf(v);
        if (a >= 1e5f) return false;
        if (a > 0.5f && a < 2.0f) hasNearUnitValue = true;
        sum += a;
    }
    if (!hasNearUnitValue) return false;
    if (sum <= 0.5f || sum >= 5000.0f) return false;
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
    return AnalyzeProjectionMatrixNumeric(m, nullptr);
}

static bool IsTypicalProjectionMatrix(const D3DMATRIX& m) {
    ProjectionAnalysis analysis = {};
    if (!AnalyzeProjectionMatrixNumeric(m, &analysis) || !analysis.valid) {
        return false;
    }
    if (analysis.fovRadians < g_config.minFOV || analysis.fovRadians > g_config.maxFOV) {
        return false;
    }

    const bool perspectiveTermsLookValid =
        fabsf(m._14) <= 0.05f &&
        fabsf(m._24) <= 0.05f &&
        fabsf(m._44) <= 0.05f &&
        fabsf(fabsf(m._34) - 1.0f) <= 0.05f;

    return perspectiveTermsLookValid;
}

static bool AnalyzeProjectionMatrixNumeric(const D3DMATRIX& m, ProjectionAnalysis* out) {
    constexpr float kZeroEpsilon = 0.02f;
    constexpr float kPerspectiveEpsilon = 0.05f;

    if (!std::isfinite(m._11) || !std::isfinite(m._22) || !std::isfinite(m._33) ||
        !std::isfinite(m._34) || !std::isfinite(m._43) || !std::isfinite(m._44)) {
        return false;
    }

    if (fabsf(m._12) > kZeroEpsilon || fabsf(m._13) > kZeroEpsilon ||
        fabsf(m._21) > kZeroEpsilon || fabsf(m._23) > kZeroEpsilon ||
        fabsf(m._31) > kZeroEpsilon || fabsf(m._32) > kZeroEpsilon) {
        return false;
    }

    if (fabsf(m._14) > kZeroEpsilon || fabsf(m._24) > kZeroEpsilon) {
        return false;
    }

    if (fabsf(fabsf(m._34) - 1.0f) > kPerspectiveEpsilon) {
        return false;
    }

    if (fabsf(m._44) > kPerspectiveEpsilon) {
        return false;
    }

    if (fabsf(m._11) < 0.001f || fabsf(m._22) < 0.001f) {
        return false;
    }

    if (fabsf(m._33) < 0.0001f || fabsf(m._43) < 0.0001f) {
        return false;
    }

    const float fov = 2.0f * atanf(1.0f / fabsf(m._22));
    if (!std::isfinite(fov) || fov < 0.01f || fov > 3.13f) {
        return false;
    }

    if (out) {
        out->valid = true;
        out->fovRadians = fov;
        out->handedness = (m._34 >= 0.0f) ? ProjectionHandedness_Left : ProjectionHandedness_Right;
    }
    return true;
}

static const char* ProjectionHandednessLabel(ProjectionHandedness handedness) {
    switch (handedness) {
        case ProjectionHandedness_Left: return "LH";
        case ProjectionHandedness_Right: return "RH";
        default: return "Unknown";
    }
}

static const char* CombinedMVPStrategyLabel(CombinedMVPStrategy strategy) {
    switch (strategy) {
        case CombinedMVPStrategy_WorldAndMVP: return "Strategy 1 (World + MVP)";
        case CombinedMVPStrategy_MVPOnly: return "Strategy 2 (MVP only)";
        case CombinedMVPStrategy_WorldRequiredNoWorld: return "Strategy 3 (world required, missing)";
        case CombinedMVPStrategy_Disabled: return "Disabled";
        case CombinedMVPStrategy_SkippedFullWVP: return "Skipped (full W/V/P already present)";
        case CombinedMVPStrategy_Failed: return "Failed";
        case CombinedMVPStrategy_None:
        default:
            return "None";
    }
}

static bool HasPerspectiveComponent(const D3DMATRIX& m) {
    return fabsf(m._34) > 0.5f && fabsf(m._44) < 0.5f;
}

static bool IsAffineMatrixNoPerspective(const D3DMATRIX& m) {
    return fabsf(m._14) < 0.02f && fabsf(m._24) < 0.02f && fabsf(m._34) < 0.02f && fabsf(m._44 - 1.0f) < 0.02f;
}

static bool IsLikelyBoneTransform(const D3DMATRIX& m,
                                  int rows,
                                  UINT vectorCountInUpload,
                                  UINT uploadStartReg,
                                  UINT candidateBaseReg) {
    if (!IsAffineMatrixNoPerspective(m)) {
        return false;
    }

    const float r0 = sqrtf(Dot3(m._11, m._12, m._13, m._11, m._12, m._13));
    const float r1 = sqrtf(Dot3(m._21, m._22, m._23, m._21, m._22, m._23));
    const float r2 = sqrtf(Dot3(m._31, m._32, m._33, m._31, m._32, m._33));
    const bool nearUnitRows = fabsf(r0 - 1.0f) < 0.25f && fabsf(r1 - 1.0f) < 0.25f && fabsf(r2 - 1.0f) < 0.25f;

    const bool appearsInPaletteUpload =
        vectorCountInUpload >= 8 &&
        candidateBaseReg >= uploadStartReg &&
        (candidateBaseReg + static_cast<UINT>(rows)) <= (uploadStartReg + vectorCountInUpload);

    return nearUnitRows && appearsInPaletteUpload;
}

static bool LooksLikeWorldStrict(const D3DMATRIX& m,
                                 int rows,
                                 UINT vectorCountInUpload,
                                 UINT uploadStartReg,
                                 UINT candidateBaseReg) {
    if (!IsAffineMatrixNoPerspective(m)) return false;
    if (LooksLikeViewStrict(m)) return false;

    const float det = Determinant3x3(m);
    if (!std::isfinite(det) || fabsf(det) < 0.0001f) return false;

    if (IsLikelyBoneTransform(m, rows, vectorCountInUpload, uploadStartReg, candidateBaseReg)) {
        return false;
    }
    return true;
}

enum MatrixClassification {
    MatrixClass_None = 0,
    MatrixClass_World,
    MatrixClass_View,
    MatrixClass_Projection,
    MatrixClass_CombinedPerspective
};

static MatrixClassification ClassifyMatrixDeterministic(const D3DMATRIX& m,
                                                        int rows,
                                                        UINT vectorCountInUpload,
                                                        UINT uploadStartReg,
                                                        UINT candidateBaseReg) {
    if (LooksLikeProjectionStrict(m)) {
        return MatrixClass_Projection;
    }
    if (HasPerspectiveComponent(m)) {
        return MatrixClass_CombinedPerspective;
    }
    if (LooksLikeViewStrict(m)) {
        return MatrixClass_View;
    }
    if (LooksLikeWorldStrict(m, rows, vectorCountInUpload, uploadStartReg, candidateBaseReg)) {
        return MatrixClass_World;
    }
    return MatrixClass_None;
}

static bool IsThreeRowPrefixOfPerspectiveMatrix(const float* data,
                                                UINT startReg,
                                                UINT vectorCount,
                                                UINT candidateBaseReg,
                                                bool transposedLayout);

static int CountStridedCandidates(const float* data,
                                  UINT startReg,
                                  UINT vectorCount,
                                  UINT strideRows,
                                  MatrixClassification targetClass) {
    int count = 0;
    for (UINT offset = 0; offset + strideRows <= vectorCount; offset += strideRows) {
        D3DMATRIX candidate = {};
        if (!TryBuildMatrixFromConstantUpdate(data + offset * 4,
                                              startReg + offset,
                                              strideRows,
                                              static_cast<int>(startReg + offset),
                                              static_cast<int>(strideRows),
                                              false,
                                              &candidate)) {
            continue;
        }
        MatrixClassification cls = ClassifyMatrixDeterministic(
            candidate,
            static_cast<int>(strideRows),
            vectorCount,
            startReg,
            startReg + offset);

        if (strideRows == 3u &&
            (cls == MatrixClass_View || cls == MatrixClass_World) &&
            IsThreeRowPrefixOfPerspectiveMatrix(data,
                                                startReg,
                                                vectorCount,
                                                startReg + offset,
                                                false)) {
            continue;
        }

        if (cls == targetClass && ++count > 1) {
            return count;
        }
    }
    return count;
}

static bool IsThreeRowPrefixOfPerspectiveMatrix(const float* data,
                                                UINT startReg,
                                                UINT vectorCount,
                                                UINT candidateBaseReg,
                                                bool transposedLayout) {
    if (!data || vectorCount < 4) {
        return false;
    }
    if (candidateBaseReg < startReg) {
        return false;
    }

    const UINT offset = candidateBaseReg - startReg;
    if (offset + 4 > vectorCount) {
        return false;
    }

    D3DMATRIX candidate4x4 = {};
    if (!TryBuildMatrixFromConstantUpdate(data + offset * 4,
                                          candidateBaseReg,
                                          4u,
                                          static_cast<int>(candidateBaseReg),
                                          4,
                                          transposedLayout,
                                          &candidate4x4)) {
        return false;
    }

    const MatrixClassification directClass = ClassifyMatrixDeterministic(
        candidate4x4,
        4,
        vectorCount,
        startReg,
        candidateBaseReg);
    if (directClass == MatrixClass_Projection || directClass == MatrixClass_CombinedPerspective) {
        return true;
    }

    const D3DMATRIX transposed = TransposeMatrix(candidate4x4);
    const MatrixClassification transposedClass = ClassifyMatrixDeterministic(
        transposed,
        4,
        vectorCount,
        startReg,
        candidateBaseReg);
    return transposedClass == MatrixClass_Projection ||
           transposedClass == MatrixClass_CombinedPerspective;
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


// Returns true if candidateView is consistent with being a pure orthonormal
// view matrix relative to knownProjection.
static bool CrossValidateViewAgainstProjection(const D3DMATRIX& candidateView,
                                               const D3DMATRIX& knownProjection) {
    D3DMATRIX vp = MultiplyMatrix(candidateView, knownProjection);

    const float p_c0 = sqrtf(
        knownProjection._11 * knownProjection._11 +
        knownProjection._21 * knownProjection._21 +
        knownProjection._31 * knownProjection._31);
    const float p_c1 = sqrtf(
        knownProjection._12 * knownProjection._12 +
        knownProjection._22 * knownProjection._22 +
        knownProjection._32 * knownProjection._32);
    const float vp_c0 = sqrtf(
        vp._11 * vp._11 + vp._21 * vp._21 + vp._31 * vp._31);
    const float vp_c1 = sqrtf(
        vp._12 * vp._12 + vp._22 * vp._22 + vp._32 * vp._32);

    if (p_c0 < 1e-6f || p_c1 < 1e-6f) {
        return true;
    }

    constexpr float kTol = 0.15f;
    return (fabsf(vp_c0 - p_c0) / p_c0 < kTol) &&
           (fabsf(vp_c1 - p_c1) / p_c1 < kTol);
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
static float MatrixIdentityMaxError(const D3DMATRIX& m) {
    const D3DMATRIX identity = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    const float* a = reinterpret_cast<const float*>(&m);
    const float* b = reinterpret_cast<const float*>(&identity);
    float maxErr = 0.0f;
    for (int i = 0; i < 16; ++i) {
        maxErr = (std::max)(maxErr, fabsf(a[i] - b[i]));
    }
    return maxErr;
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
    return 2.0f * atanf(1.0f / fabsf(proj._22));
}

// Check if matrix looks like projection
bool LooksLikeProjection(const D3DMATRIX& m) {
    return AnalyzeProjectionMatrixNumeric(m, nullptr);
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

static void CreateProjectionMatrixWithHandedness(D3DMATRIX* out,
                                                 float fovY,
                                                 float aspect,
                                                 float zNear,
                                                 float zFar,
                                                 ProjectionHandedness handedness) {
    CreateProjectionMatrix(out, fovY, aspect, zNear, zFar);
    if (handedness == ProjectionHandedness_Right) {
        out->_33 = zFar / (zNear - zFar);
        out->_34 = -1.0f;
        out->_43 = zNear * zFar / (zNear - zFar);
    }
}

static bool TryGetCurrentDisplayAspect(IDirect3DDevice9* device,
                                       HWND hwnd,
                                       float* outAspect,
                                       UINT* outWidth,
                                       UINT* outHeight) {
    if (!outAspect) {
        return false;
    }

    if (device) {
        D3DVIEWPORT9 viewport = {};
        if (SUCCEEDED(device->GetViewport(&viewport)) && viewport.Width > 0 && viewport.Height > 0) {
            *outAspect = static_cast<float>(viewport.Width) / static_cast<float>(viewport.Height);
            if (outWidth) {
                *outWidth = viewport.Width;
            }
            if (outHeight) {
                *outHeight = viewport.Height;
            }
            return true;
        }
    }

    RECT rc = {};
    if (hwnd && GetClientRect(hwnd, &rc)) {
        const LONG width = rc.right - rc.left;
        const LONG height = rc.bottom - rc.top;
        if (width > 0 && height > 0) {
            *outAspect = static_cast<float>(width) / static_cast<float>(height);
            if (outWidth) {
                *outWidth = static_cast<UINT>(width);
            }
            if (outHeight) {
                *outHeight = static_cast<UINT>(height);
            }
            return true;
        }
    }

    return false;
}

static bool BuildExperimentalCustomProjectionMatrix(IDirect3DDevice9* device,
                                                    HWND hwnd,
                                                    D3DMATRIX* outProjection,
                                                    bool* outUsedAuto,
                                                    float* outResolvedAspect,
                                                    UINT* outWidth,
                                                    UINT* outHeight) {
    if (!outProjection || !g_config.experimentalCustomProjectionEnabled) {
        return false;
    }

    const CustomProjectionMode mode = g_config.experimentalCustomProjectionMode;
    if (mode == CustomProjectionMode_Manual) {
        *outProjection = g_config.experimentalCustomProjectionManualMatrix;
        if (outUsedAuto) {
            *outUsedAuto = false;
        }
        return true;
    }

    if (mode != CustomProjectionMode_Auto) {
        return false;
    }

    float aspect = g_config.experimentalCustomProjectionAutoAspectFallback;
    UINT width = 0;
    UINT height = 0;
    TryGetCurrentDisplayAspect(device, hwnd, &aspect, &width, &height);

    const float safeAspect = (std::max)(0.1f, aspect);
    const float fovDeg = (std::max)(1.0f, (std::min)(g_config.experimentalCustomProjectionAutoFovDeg, 179.0f));
    const float nearZ = (std::max)(0.0001f, g_config.experimentalCustomProjectionAutoNearZ);
    const float farZ = (std::max)(nearZ + 0.001f, g_config.experimentalCustomProjectionAutoFarZ);
    const ProjectionHandedness handedness =
        g_config.experimentalCustomProjectionAutoHandedness == ProjectionHandedness_Right
            ? ProjectionHandedness_Right
            : ProjectionHandedness_Left;

    CreateProjectionMatrixWithHandedness(outProjection,
                                         fovDeg * (3.14159265f / 180.0f),
                                         safeAspect,
                                         nearZ,
                                         farZ,
                                         handedness);

    if (outUsedAuto) {
        *outUsedAuto = true;
    }
    if (outResolvedAspect) {
        *outResolvedAspect = safeAspect;
    }
    if (outWidth) {
        *outWidth = width;
    }
    if (outHeight) {
        *outHeight = height;
    }
    return true;
}

static void OrthonormalizeViewMatrix(D3DMATRIX* view) {
    if (!view) {
        return;
    }

    const float origTx = view->_41;
    const float origTy = view->_42;
    const float origTz = view->_43;

    float r0x = view->_11, r0y = view->_12, r0z = view->_13;
    float r1x = view->_21, r1y = view->_22, r1z = view->_23;

    const float len0 = sqrtf(Dot3(r0x, r0y, r0z, r0x, r0y, r0z));
    if (len0 > 1e-6f) {
        r0x /= len0; r0y /= len0; r0z /= len0;
    }

    float dot01 = Dot3(r1x, r1y, r1z, r0x, r0y, r0z);
    r1x -= dot01 * r0x;
    r1y -= dot01 * r0y;
    r1z -= dot01 * r0z;

    const float len1 = sqrtf(Dot3(r1x, r1y, r1z, r1x, r1y, r1z));
    if (len1 > 1e-6f) {
        r1x /= len1; r1y /= len1; r1z /= len1;
    }

    const float r2x = r0y * r1z - r0z * r1y;
    const float r2y = r0z * r1x - r0x * r1z;
    const float r2z = r0x * r1y - r0y * r1x;

    view->_11 = r0x; view->_12 = r0y; view->_13 = r0z; view->_14 = 0.0f;
    view->_21 = r1x; view->_22 = r1y; view->_23 = r1z; view->_24 = 0.0f;
    view->_31 = r2x; view->_32 = r2y; view->_33 = r2z; view->_34 = 0.0f;
    view->_41 = Dot3(origTx, origTy, origTz, view->_11, view->_12, view->_13);
    view->_42 = Dot3(origTx, origTy, origTz, view->_21, view->_22, view->_23);
    view->_43 = Dot3(origTx, origTy, origTz, view->_31, view->_32, view->_33);
    view->_44 = 1.0f;
}

static bool TryExtractProjectionFromCombined(const D3DMATRIX& combined,
                                             const D3DMATRIX* worldOptional,
                                             bool worldAvailable,
                                             ProjectionAnalysis* outAnalysis,
                                             D3DMATRIX* outProjection,
                                             bool forceDecomposition) {
    if (!outProjection) {
        return false;
    }

    D3DMATRIX extractionMatrix = combined;
    if (worldAvailable && worldOptional) {
        D3DMATRIX worldInv = {};
        if (!InvertMatrix4x4Deterministic(*worldOptional, &worldInv, nullptr)) {
            return false;
        }
        extractionMatrix = MultiplyMatrix(worldInv, combined);
    } else if (!IsTypicalProjectionMatrix(combined)) {
        return false;
    }

    if (!IsTypicalProjectionMatrix(extractionMatrix)) {
        return false;
    }

    const float sx = sqrtf(Dot3(extractionMatrix._11, extractionMatrix._12, extractionMatrix._13,
                                extractionMatrix._11, extractionMatrix._12, extractionMatrix._13));
    const float sy = sqrtf(Dot3(extractionMatrix._21, extractionMatrix._22, extractionMatrix._23,
                                extractionMatrix._21, extractionMatrix._22, extractionMatrix._23));
    if (!std::isfinite(sx) || !std::isfinite(sy) || sx < 1e-5f || sy < 1e-5f) {
        return false;
    }

    const float fov = 2.0f * atanf(1.0f / sy);
    const float aspect = sy / sx;
    if (!forceDecomposition) {
        if (!std::isfinite(fov) || fov < 0.01f || fov > 3.13f) {
            return false;
        }
    }

    const float det = Determinant3x3(extractionMatrix);
    ProjectionHandedness handedness = ProjectionHandedness_Unknown;
    if (std::isfinite(det)) {
        handedness = (det < 0.0f) ? ProjectionHandedness_Right : ProjectionHandedness_Left;
    }

    const float nearZ = (std::max)(0.0001f, g_config.experimentalCustomProjectionAutoNearZ);
    const float farZ = (std::max)(nearZ + 0.001f, g_config.experimentalCustomProjectionAutoFarZ);
    CreateProjectionMatrixWithHandedness(outProjection,
                                         fov,
                                         (std::max)(0.1f, aspect),
                                         nearZ,
                                         farZ,
                                         handedness);

    if (outAnalysis) {
        outAnalysis->valid = true;
        outAnalysis->fovRadians = fov;
        outAnalysis->handedness = handedness;
    }
    return true;
}

static bool TryDecomposeCombinedMVP(const D3DMATRIX& mvp,
                                    const D3DMATRIX* worldOptional,
                                    bool worldAvailable,
                                    D3DMATRIX* outWorld,
                                    D3DMATRIX* outView,
                                    D3DMATRIX* outProjection,
                                    ProjectionAnalysis* outProjectionAnalysis) {
    if (!outWorld || !outView || !outProjection) {
        return false;
    }

    D3DMATRIX world = {};
    D3DMATRIX viewProjection = {};

    if (worldAvailable && worldOptional) {
        world = *worldOptional;
        D3DMATRIX worldInv = {};
        if (!InvertMatrix4x4Deterministic(world, &worldInv, nullptr)) {
            return false;
        }
        viewProjection = MultiplyMatrix(mvp, worldInv);
    } else {
        CreateIdentityMatrix(&world);
        viewProjection = mvp;
    }

    ProjectionAnalysis analysis = {};
    D3DMATRIX projection = {};
    if (!TryExtractProjectionFromCombined(viewProjection, worldOptional, worldAvailable, &analysis, &projection, g_config.combinedMVPForceDecomposition)) {
        return false;
    }

    D3DMATRIX projectionInv = {};
    if (!InvertMatrix4x4Deterministic(projection, &projectionInv, nullptr)) {
        return false;
    }

    D3DMATRIX view = MultiplyMatrix(projectionInv, viewProjection);
    OrthonormalizeViewMatrix(&view);

    *outWorld = world;
    *outView = view;
    *outProjection = projection;
    if (outProjectionAnalysis) {
        *outProjectionAnalysis = analysis;
    }
    return true;
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

class WrappedVertexShader9 : public IDirect3DVertexShader9 {
private:
    IDirect3DVertexShader9* m_real;
    uintptr_t m_key;
public:
    explicit WrappedVertexShader9(IDirect3DVertexShader9* real)
        : m_real(real), m_key(reinterpret_cast<uintptr_t>(this)) {}

    IDirect3DVertexShader9* GetReal() const { return m_real; }
    uintptr_t GetKey() const { return m_key; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override { return m_real->QueryInterface(riid, ppv); }
    ULONG STDMETHODCALLTYPE AddRef() override { return m_real->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_real->Release();
        if (count == 0) {
            OnVertexShaderReleased(m_key);
            delete this;
        }
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override { return m_real->GetDevice(ppDevice); }
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSizeOfData) override { return m_real->GetFunction(pData, pSizeOfData); }
};

/**
 * Wrapped IDirect3DDevice9 - intercepts SetVertexShaderConstantF
 */
class WrappedD3D9Device : public IDirect3DDevice9Ex {
private:
    IDirect3DDevice9* m_real;
    IDirect3DDevice9Ex* m_realEx = nullptr;
    D3DMATRIX m_currentView;
    D3DMATRIX m_currentProj;
    D3DMATRIX m_currentWorld;
    HWND m_hwnd = nullptr;
    IDirect3DVertexShader9* m_currentVertexShader = nullptr;
    IDirect3DPixelShader9* m_currentPixelShader = nullptr;
    bool m_hasView = false;
    bool m_hasProj = false;
    bool m_hasWorld = false;
    bool m_mgrrUseAutoProjection = false;
    int m_constantLogThrottle = 0;
    int m_viewLastFrame = -1;
    int m_projLastFrame = -1;
    int m_projDetectedFrame = -1;
    int m_worldLastFrame = -1;
    uintptr_t m_viewLockedShader = 0;
    int m_viewLockedRegister = -1;
    uintptr_t m_projLockedShader = 0;
    int m_projLockedRegister = -1;

public:
    WrappedD3D9Device(IDirect3DDevice9* real) : m_real(real) {
        if (m_real) {
            m_real->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&m_realEx));
        }
        CreateIdentityMatrix(&m_currentView);
        CreateIdentityMatrix(&m_currentProj);
        CreateIdentityMatrix(&m_currentWorld);
        m_projDetectedFrame = -1;
        m_mgrrUseAutoProjection = g_config.mgrrUseAutoProjectionWhenC4Invalid;
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
        if (m_realEx) {
            m_realEx->Release();
            m_realEx = nullptr;
        }
        LogMsg("WrappedD3D9Device destroyed");
    }

    void EmitFixedFunctionTransforms() {
        if (!g_config.emitFixedFunctionTransforms) {
            return;
        }
        if (g_config.setTransformBypassProxyWhenGameProvides && g_gameSetTransformAnySeen) {
            return;
        }
        if (g_activeGameProfile == GameProfile_MetalGearRising) {
            // MGR profile is strict: only emit transforms when all three known registers
            // have been captured. Never emit identity/fallback transforms in this mode.
            if (!(m_hasWorld && m_hasView && m_hasProj)) {
                snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                         "MGR draw skipped: missing matrix/matrices (Proj=%s View=%s World=%s).",
                         m_hasProj ? "ready" : "missing",
                         m_hasView ? "ready" : "missing",
                         m_hasWorld ? "ready" : "missing");
                return;
            }

            g_lastInverseViewAsWorldEligible = false;
            g_lastInverseViewAsWorldApplied = false;
            g_lastInverseViewAsWorldUsedFast = false;

            // Keep MGR strict and deterministic: WORLD always comes from c16-c19.
            m_real->SetTransform(D3DTS_WORLD, &m_currentWorld);
            m_real->SetTransform(D3DTS_VIEW, &m_currentView);
            m_real->SetTransform(D3DTS_PROJECTION, &m_currentProj);
            return;
        }

        if (g_activeGameProfile == GameProfile_DevilMayCry4) {
            if (!(m_hasWorld && m_hasView && m_hasProj)) {
                snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                         "DMC4 draw skipped: missing matrix/matrices (World=%s View=%s Proj=%s).",
                         m_hasWorld ? "ready" : "missing",
                         m_hasView ? "ready" : "missing",
                         m_hasProj ? "ready" : "missing");
                return;
            }

            g_lastInverseViewAsWorldEligible = false;
            g_lastInverseViewAsWorldApplied = false;
            g_lastInverseViewAsWorldUsedFast = false;

            // Keep DMC4 strict and deterministic: WORLD always comes from c0-c3.
            m_real->SetTransform(D3DTS_WORLD, &m_currentWorld);
            m_real->SetTransform(D3DTS_VIEW, &m_currentView);
            m_real->SetTransform(D3DTS_PROJECTION, &m_currentProj);
            return;
        }

        if (g_activeGameProfile == GameProfile_Barnyard) {
            const bool useGameViewProj = g_config.barnyardUseGameSetTransformsForViewProjection;
            if (!m_hasWorld || (useGameViewProj && (!m_hasView || !m_hasProj))) {
                snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                         "Barnyard draw skipped: missing matrices (World=%s View=%s Proj=%s).",
                         m_hasWorld ? "ready" : "missing",
                         m_hasView ? "ready" : "missing",
                         m_hasProj ? "ready" : "missing");
                return;
            }

            g_lastInverseViewAsWorldEligible = false;
            g_lastInverseViewAsWorldApplied = false;
            g_lastInverseViewAsWorldUsedFast = false;

            // Keep Barnyard profile semantics deterministic: WORLD comes from shader constants.
            m_real->SetTransform(D3DTS_WORLD, &m_currentWorld);
            if (useGameViewProj) {
                m_real->SetTransform(D3DTS_VIEW, &m_currentView);
                m_real->SetTransform(D3DTS_PROJECTION, &m_currentProj);
            }
            return;
        }

        bool shouldApplyCustomProjection = false;
        if (g_config.experimentalCustomProjectionEnabled) {
            const bool projectionMissing = !m_hasProj;
            const bool projectionOverrideAllowed = g_config.experimentalCustomProjectionOverrideDetectedProjection;
            bool hasMvp = false;
            { std::lock_guard<std::mutex> lock(g_cameraMatricesMutex); hasMvp = g_cameraMatrices.hasMVP; }
            const bool mvpBlocksProjection = hasMvp && !g_config.experimentalCustomProjectionOverrideCombinedMVP;
            shouldApplyCustomProjection = (projectionMissing || projectionOverrideAllowed) && !mvpBlocksProjection;
        }

        if (shouldApplyCustomProjection) {
            D3DMATRIX customProjection = {};
            bool usedAuto = false;
            float resolvedAspect = 0.0f;
            UINT width = 0;
            UINT height = 0;
            if (BuildExperimentalCustomProjectionMatrix(m_real, m_hwnd, &customProjection,
                                                        &usedAuto, &resolvedAspect,
                                                        &width, &height)) {
                m_currentProj = customProjection;
                m_hasProj = true;
                m_projLastFrame = g_frameCount;
                g_projectionDetectedByNumericStructure = false;
                g_projectionDetectedRegister = -1;
                g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
                g_projectionDetectedFovRadians = ExtractFOV(customProjection);
                StoreProjectionMatrix(m_currentProj, 0, -1, 4, false, true,
                                      usedAuto
                                          ? "experimental custom projection auto"
                                          : "experimental custom projection manual");
                if (usedAuto) {
                    snprintf(g_customProjectionStatus, sizeof(g_customProjectionStatus),
                             "Experimental projection active (auto): %ux%u aspect=%.4f fov=%.2f near=%.4f far=%.2f.",
                             width, height, resolvedAspect,
                             g_config.experimentalCustomProjectionAutoFovDeg,
                             g_config.experimentalCustomProjectionAutoNearZ,
                             g_config.experimentalCustomProjectionAutoFarZ);
                } else {
                    snprintf(g_customProjectionStatus, sizeof(g_customProjectionStatus),
                             "Experimental projection active (manual matrix mode).");
                }
            }
        }

        D3DMATRIX identity = {};
        CreateIdentityMatrix(&identity);
        D3DMATRIX emitWorld = m_currentWorld;
        D3DMATRIX emitView = m_currentView;
        D3DMATRIX emitProj = m_currentProj;

        if (!m_hasWorld) emitWorld = identity;
        if (!m_hasView) emitView = identity;
        if (!m_hasProj) emitProj = identity;

        if (m_worldLastFrame >= 0 && g_frameCount > m_worldLastFrame + 1) {
            LogMsg("World matrix stale (last update frame %d, current %d); emitting identity.", m_worldLastFrame, g_frameCount);
            emitWorld = identity;
        }
        if (m_viewLastFrame >= 0 && g_frameCount > m_viewLastFrame + 1) {
            LogMsg("View matrix stale (last update frame %d, current %d); emitting identity.", m_viewLastFrame, g_frameCount);
            emitView = identity;
        }
        if (m_projLastFrame >= 0 && g_frameCount > m_projLastFrame + 1) {
            LogMsg("Projection matrix stale (last update frame %d, current %d); emitting identity.", m_projLastFrame, g_frameCount);
            emitProj = identity;
        }

        g_lastInverseViewAsWorldEligible = false;
        g_lastInverseViewAsWorldApplied = false;
        g_lastInverseViewAsWorldUsedFast = false;
        if (g_config.experimentalInverseViewAsWorld && m_hasView) {
            const bool viewLooksValid = LooksLikeViewStrict(emitView);
            g_lastInverseViewAsWorldEligible = viewLooksValid;
            if (viewLooksValid || g_config.experimentalInverseViewAsWorldAllowUnverified) {
                D3DMATRIX derivedWorld = {};
                bool usedFastInverse = false;
                bool fastEligible = false;
                if (TryBuildWorldFromView(emitView, g_config.experimentalInverseViewAsWorldFast,
                                          &derivedWorld, &usedFastInverse, &fastEligible)) {
                    emitWorld = derivedWorld;
                    g_lastInverseViewAsWorldApplied = true;
                    g_lastInverseViewAsWorldUsedFast = usedFastInverse;
                    if (!fastEligible && g_config.experimentalInverseViewAsWorldFast) {
                        LogMsg("Fast inverse requested but view matrix did not qualify (possible scaling/shear); used full inverse.");
                    }
                }
            }
        }

        m_real->SetTransform(D3DTS_WORLD, &emitWorld);
        m_real->SetTransform(D3DTS_VIEW, &emitView);
        m_real->SetTransform(D3DTS_PROJECTION, &emitProj);
    }


    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IDirect3DDevice9)) {
            *ppvObj = static_cast<IDirect3DDevice9*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IDirect3DDevice9Ex) && m_realEx) {
            *ppvObj = static_cast<IDirect3DDevice9Ex*>(this);
            AddRef();
            return S_OK;
        }
        return m_real->QueryInterface(riid, ppvObj);
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
        if (g_constantUploadRecordingEnabled) {
            RecordConstantUpload(ConstantUploadStage_Vertex, shaderKey, StartRegister, Vector4fCount);
        }
        ShaderConstantState* state = GetShaderState(shaderKey, true);
        const bool profileIsMgr = g_activeGameProfile == GameProfile_MetalGearRising;
        const bool profileIsBarnyard = g_activeGameProfile == GameProfile_Barnyard;

        std::vector<float> overrideScratch;
        const float* effectiveConstantData = pConstantData;
        // Keep MGR profile extraction isolated from manual/override paths.
        if (!profileIsMgr && !profileIsBarnyard && BuildOverriddenConstants(*state, StartRegister, Vector4fCount, pConstantData, overrideScratch)) {
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

            GlobalVertexRegisterState& globalState = g_allVertexRegisters[reg];
            memcpy(globalState.value, effectiveConstantData + i * 4, sizeof(globalState.value));
            globalState.valid = true;
            globalState.lastUploadSerial = g_constantUploadSerial;
            globalState.lastShaderKey = shaderKey;
            globalState.lastShaderHash = GetShaderHashForKey(shaderKey);
        }
        if (constantsChanged) {
            state->lastChangeSerial = ++g_constantChangeSerial;
        }
        state->snapshotReady = true;

        bool slotResolvedByOverride[MatrixSlot_Count] = {};
        bool slotResolvedStructurally[MatrixSlot_Count] = {};

        if (!profileIsMgr && !profileIsBarnyard && shaderKey != 0) {
            for (int slot = 0; slot < MatrixSlot_Count; slot++) {
                const ManualMatrixBinding& binding = g_manualBindings[slot];
                if (!binding.enabled || binding.shaderKey != shaderKey) {
                    continue;
                }
                D3DMATRIX manualMat = {};
                if (!TryBuildMatrixSnapshot(*state, binding.baseRegister, binding.rows, false, &manualMat)) {
                    continue;
                }
                slotResolvedByOverride[slot] = true;
                if (slot == MatrixSlot_World) {
                    m_currentWorld = manualMat;
                    m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                    StoreWorldMatrix(m_currentWorld, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_View) {
                    m_currentView = manualMat;
                    m_hasView = true;
                m_viewLastFrame = g_frameCount;
                    StoreViewMatrix(m_currentView, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_Projection) {
                    m_currentProj = manualMat;
                    m_hasProj = true;
                m_projLastFrame = g_frameCount;
                    g_projectionDetectedByNumericStructure = false;
                    g_projectionDetectedRegister = binding.baseRegister;
                    g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
                    g_projectionDetectedFovRadians = 0.0f;
                    StoreProjectionMatrix(m_currentProj, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_MVP) {
                    StoreMVPMatrix(manualMat, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_VP) {
                    StoreVPMatrix(manualMat, shaderKey, binding.baseRegister, binding.rows, false, true);
                } else if (slot == MatrixSlot_WV) {
                    StoreWVMatrix(manualMat, shaderKey, binding.baseRegister, binding.rows, false, true);
                }
            }
        }

        if (profileIsMgr) {
            g_profileDisableStructuralDetection = true;
            // MGR strict known-layout mode:
            // - capture c4-c7 projection, c8-c11 viewProjection, c16-c19 world
            // - derive View as inverse(Projection) * ViewProjection
            // - do not run structural detection fallback or candidate scanning
            // - persist projection/view across draws until overwritten by the same known registers
            auto tryExtractMgrMatrix = [&](int baseRegister, D3DMATRIX* outMat) -> bool {
                if (!outMat || !effectiveConstantData || baseRegister < 0) {
                    return false;
                }
                const UINT uploadStart = StartRegister;
                const UINT uploadEnd = (Vector4fCount == 0) ? StartRegister : (StartRegister + Vector4fCount - 1);
                const UINT matrixEnd = static_cast<UINT>(baseRegister + 3);
                if (uploadStart > static_cast<UINT>(baseRegister) || uploadEnd < matrixEnd) {
                    return false;
                }
                return TryBuildMatrixFromConstantUpdate(effectiveConstantData,
                                                        StartRegister,
                                                        Vector4fCount,
                                                        baseRegister,
                                                        4,
                                                        false,
                                                        outMat);
            };

            D3DMATRIX mat = {};
            if (tryExtractMgrMatrix(4, &mat)) {
                g_profileCoreRegistersSeen[0] = true;
                g_mgrProjectionRegisterValid = IsTypicalProjectionMatrix(mat);
                m_currentProj = mat;
                m_hasProj = true;
                m_projLastFrame = g_frameCount;
                g_mgrProjCapturedThisFrame = true;
                if (g_mgrProjectionRegisterValid) {
                    g_projectionDetectedByNumericStructure = false;
                    g_projectionDetectedRegister = 4;
                    g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
                    g_projectionDetectedFovRadians = ExtractFOV(mat);
                    StoreProjectionMatrix(m_currentProj, shaderKey, 4, 4, false, true,
                                          "MetalGearRising profile projection (c4-c7)");
                } else {
                    g_projectionDetectedByNumericStructure = false;
                    g_projectionDetectedRegister = 4;
                    g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
                    g_projectionDetectedFovRadians = 0.0f;
                    StoreProjectionMatrix(m_currentProj, shaderKey, 4, 4, false, true,
                                          "MetalGearRising profile projection (c4-c7, non-typical)");
                    snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                             "MGR projection at c4-c7 is non-typical; using it by default.");
                }
            }

            if (tryExtractMgrMatrix(8, &mat)) {
                g_profileCoreRegistersSeen[1] = true;
                g_profileOptionalRegistersSeen[0] = true;

                D3DMATRIX resolvedProjection = {};
                bool haveProjectionForViewDerivation = false;

                if (m_hasProj) {
                    resolvedProjection = m_currentProj;
                    haveProjectionForViewDerivation = true;
                }

                if (!g_mgrProjectionRegisterValid && m_mgrrUseAutoProjection) {
                    ProjectionAnalysis generatedProjectionInfo = {};
                    D3DMATRIX generatedProjection = {};
                    if (TryExtractProjectionFromCombined(mat,
                                                         nullptr,
                                                         false,
                                                         &generatedProjectionInfo,
                                                         &generatedProjection,
                                                         g_config.combinedMVPForceDecomposition)) {
                        resolvedProjection = generatedProjection;
                        m_currentProj = generatedProjection;
                        m_hasProj = true;
                m_projLastFrame = g_frameCount;
                        g_mgrProjCapturedThisFrame = true;
                        g_projectionDetectedByNumericStructure = true;
                        g_projectionDetectedRegister = 8;
                        g_projectionDetectedHandedness = generatedProjectionInfo.handedness;
                        g_projectionDetectedFovRadians = generatedProjectionInfo.fovRadians;
                        StoreProjectionMatrix(m_currentProj, shaderKey, 8, 4, false, true,
                                              "MetalGearRising auto projection from VP (c8-c11)");
                        haveProjectionForViewDerivation = true;
                    }
                }

                if (haveProjectionForViewDerivation) {
                    D3DMATRIX projectionInv = {};
                    if (InvertMatrix4x4Deterministic(resolvedProjection, &projectionInv, nullptr)) {
                        D3DMATRIX derivedView = MultiplyMatrix(projectionInv, mat);
                        m_currentView = derivedView;
                        m_hasView = true;
                m_viewLastFrame = g_frameCount;
                        g_mgrViewCapturedThisFrame = true;
                        g_profileViewDerivedFromInverse = true;
                        snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                                 "MGR view updated from VP (c8-c11) using inverse projection.");
                        StoreViewMatrix(m_currentView, shaderKey, 8, 4, false, true,
                                        "MetalGearRising profile view from VP", 8);
                    } else {
                        g_profileViewDerivedFromInverse = false;
                        snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                                 "MGR VP derivation failed: projection inversion failed.");
                    }
                } else {
                    g_profileViewDerivedFromInverse = false;
                    snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                             "MGR VP detected but projection inversion failed. Enable auto projection to prefer generated projection.");
                }
            }

            if (tryExtractMgrMatrix(16, &mat)) {
                m_currentWorld = mat;
                m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                g_mgrWorldCapturedForDraw = true;
                g_profileCoreRegistersSeen[2] = true;
                StoreWorldMatrix(m_currentWorld, shaderKey, 16, 4, false, true,
                                 "MetalGearRising profile world (c16-c19)");
            }

            return m_real->SetVertexShaderConstantF(StartRegister, effectiveConstantData, Vector4fCount);
        }

        if (profileIsBarnyard) {
            bool worldCaptured = false;
            D3DMATRIX mat = {};

            if (g_barnyardForceWorldFromC0 &&
                TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                 0, 4, false, &mat)) {
                m_currentWorld = mat;
                m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                worldCaptured = true;
                g_profileCoreRegistersSeen[2] = true;
                StoreWorldMatrix(m_currentWorld, shaderKey, 0, 4, false, true,
                                 "Barnyard profile forced world (c0-c3)");
            }

            if (!worldCaptured && g_config.worldMatrixRegister >= 0 &&
                TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                 g_config.worldMatrixRegister, 4, false, &mat)) {
                m_currentWorld = mat;
                m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                worldCaptured = true;
                g_profileCoreRegistersSeen[2] = true;
                StoreWorldMatrix(m_currentWorld, shaderKey, g_config.worldMatrixRegister, 4, false, true,
                                 "Barnyard profile world (explicit register override)");
            }

            if (!worldCaptured && g_config.autoDetectMatrices && effectiveConstantData && Vector4fCount >= 3) {
                for (UINT rows : {4u, 3u}) {
                    if (Vector4fCount < rows || worldCaptured) {
                        continue;
                    }
                    for (UINT offset = 0; offset + rows <= Vector4fCount; ++offset) {
                        UINT baseReg = StartRegister + offset;
                        D3DMATRIX candidate = {};
                        if (!TryBuildMatrixFromConstantUpdate(effectiveConstantData + offset * 4, baseReg, rows,
                                                              static_cast<int>(baseReg), static_cast<int>(rows),
                                                              false, &candidate)) {
                            continue;
                        }

                        MatrixClassification cls = ClassifyMatrixDeterministic(candidate, static_cast<int>(rows),
                                                                               Vector4fCount, StartRegister, baseReg);
                        bool transposed = false;
                        if (cls == MatrixClass_None && g_probeTransposedLayouts) {
                            D3DMATRIX t = TransposeMatrix(candidate);
                            cls = ClassifyMatrixDeterministic(t, static_cast<int>(rows), Vector4fCount, StartRegister, baseReg);
                            if (cls != MatrixClass_None) {
                                candidate = t;
                                transposed = true;
                            }
                        }

                        if (cls == MatrixClass_World) {
                            m_currentWorld = candidate;
                            m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                            worldCaptured = true;
                            g_profileCoreRegistersSeen[2] = true;
                            StoreWorldMatrix(m_currentWorld, shaderKey, static_cast<int>(baseReg),
                                             static_cast<int>(rows), transposed, false,
                                             "Barnyard profile structural world");
                            break;
                        }
                    }
                }
            }

            if (worldCaptured) {
                snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                         "Barnyard profile active: forwarding WORLD only; VIEW/PROJECTION SetTransform blocked.");
            } else {
                snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                         "Barnyard profile active: waiting for world matrix in shader constants.");
            }

            g_profileDisableStructuralDetection = true;
            return m_real->SetVertexShaderConstantF(StartRegister, effectiveConstantData, Vector4fCount);
        }

        const bool profileIsDmc4 = g_activeGameProfile == GameProfile_DevilMayCry4;
        const bool profileActive = profileIsMgr || profileIsDmc4 || profileIsBarnyard;

        auto tryExtractProfileMatrix = [&](int baseRegister, D3DMATRIX* outMat) -> bool {
            if (!outMat || !effectiveConstantData || baseRegister < 0) {
                return false;
            }
            return TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                    baseRegister, 4, false, outMat);
        };

        if (profileIsDmc4) {
            D3DMATRIX mat = {};
            bool anyCaptured = false;

            if (tryExtractProfileMatrix(g_profileLayout.combinedMvpBase, &mat)) {
                anyCaptured = true;
                StoreMVPMatrix(mat, shaderKey, g_profileLayout.combinedMvpBase, 4, false, true,
                               "DevilMayCry4 profile combined MVP (c0-c3)");
                m_currentWorld = mat;
                m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                slotResolvedByOverride[MatrixSlot_World] = true;
                g_profileCoreRegistersSeen[0] = true;
                StoreWorldMatrix(m_currentWorld, shaderKey, g_profileLayout.worldBase, 4, false, true,
                                 "DevilMayCry4 profile world (c0-c3)");
            }

            if (tryExtractProfileMatrix(g_profileLayout.viewInverseBase, &mat)) {
                anyCaptured = true;
                m_currentView = mat;
                m_hasView = true;
                m_viewLastFrame = g_frameCount;
                slotResolvedByOverride[MatrixSlot_View] = true;
                g_profileCoreRegistersSeen[1] = true;
                g_profileViewDerivedFromInverse = false;
                StoreViewMatrix(m_currentView, shaderKey, g_profileLayout.viewInverseBase, 4, false, true,
                                "DevilMayCry4 profile view (c4-c7)");
            }

            if (tryExtractProfileMatrix(g_profileLayout.projectionBase, &mat)) {
                anyCaptured = true;
                m_currentProj = mat;
                m_hasProj = true;
                m_projLastFrame = g_frameCount;
                slotResolvedByOverride[MatrixSlot_Projection] = true;
                g_projectionDetectedByNumericStructure = false;
                g_projectionDetectedRegister = g_profileLayout.projectionBase;
                g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
                g_projectionDetectedFovRadians = ExtractFOV(mat);
                g_profileCoreRegistersSeen[2] = true;
                StoreProjectionMatrix(m_currentProj, shaderKey, g_profileLayout.projectionBase, 4, false, true,
                                      "DevilMayCry4 profile projection (c8-c11)");
            }

            if (anyCaptured) {
                snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                         "DMC4 profile active: strict mapping MVP/World=c0-c3 View=c4-c7 Projection=c8-c11.");
            } else {
                snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                         "DMC4 profile active but upload did not hit c0-c11 transform registers.");
            }

            g_profileDisableStructuralDetection = true;
            return m_real->SetVertexShaderConstantF(StartRegister, effectiveConstantData, Vector4fCount);
        }

        auto tryExplicitRegisterOverride = [&](MatrixSlot slot, int configuredRegister) {
            if (profileActive) {
                return;
            }
            if (configuredRegister < 0 || !effectiveConstantData) {
                return;
            }
            if (slotResolvedByOverride[slot]) {
                return;
            }
            for (UINT rows : {4u, 3u}) {
                D3DMATRIX mat = {};
                if (!TryBuildMatrixFromConstantUpdate(effectiveConstantData, StartRegister, Vector4fCount,
                                                      configuredRegister, static_cast<int>(rows), false, &mat)) {
                    continue;
                }
                slotResolvedByOverride[slot] = true;
                if (slot == MatrixSlot_World) {
                    m_currentWorld = mat;
                    m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                    StoreWorldMatrix(m_currentWorld, shaderKey, configuredRegister, static_cast<int>(rows), false, true,
                                     "explicit register override");
                } else if (slot == MatrixSlot_View) {
                    m_currentView = mat;
                    m_hasView = true;
                m_viewLastFrame = g_frameCount;
                    StoreViewMatrix(m_currentView, shaderKey, configuredRegister, static_cast<int>(rows), false, true,
                                    "explicit register override");
                } else if (slot == MatrixSlot_Projection) {
                    m_currentProj = mat;
                    m_hasProj = true;
                m_projLastFrame = g_frameCount;
                    g_projectionDetectedByNumericStructure = false;
                    g_projectionDetectedRegister = configuredRegister;
                    g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
                    g_projectionDetectedFovRadians = 0.0f;
                    StoreProjectionMatrix(m_currentProj, shaderKey, configuredRegister, static_cast<int>(rows), false, true,
                                          "explicit register override");
                }
                return;
            }
        };

        tryExplicitRegisterOverride(MatrixSlot_World, g_config.worldMatrixRegister);
        tryExplicitRegisterOverride(MatrixSlot_View, g_config.viewMatrixRegister);
        tryExplicitRegisterOverride(MatrixSlot_Projection, g_config.projMatrixRegister);

        auto tryHandleCombinedMVP = [&](const D3DMATRIX& combinedMvp, UINT baseReg, int rows, bool transposed) {
            StoreMVPMatrix(combinedMvp, shaderKey, static_cast<int>(baseReg), rows, transposed, false,
                           "deterministic structural combined MVP", static_cast<int>(baseReg));
            g_combinedMvpDebug.registerBase = static_cast<int>(baseReg);
            g_combinedMvpDebug.succeeded = false;
            g_combinedMvpDebug.fovRadians = 0.0f;
            g_combinedMvpDebug.handedness = ProjectionHandedness_Unknown;

            if (!g_config.enableCombinedMVP) {
                g_combinedMvpDebug.strategy = CombinedMVPStrategy_Disabled;
                return;
            }
            if (m_hasWorld && m_hasView && m_hasProj) {
                g_combinedMvpDebug.strategy = CombinedMVPStrategy_SkippedFullWVP;
                return;
            }

            const bool worldAvailable = m_hasWorld;
            CombinedMVPStrategy strategy = CombinedMVPStrategy_None;
            if (worldAvailable) {
                strategy = CombinedMVPStrategy_WorldAndMVP;
            } else if (g_config.combinedMVPRequireWorld) {
                strategy = CombinedMVPStrategy_WorldRequiredNoWorld;
            } else if (g_config.combinedMVPAssumeIdentityWorld) {
                strategy = CombinedMVPStrategy_MVPOnly;
            } else {
                strategy = CombinedMVPStrategy_Failed;
            }
            g_combinedMvpDebug.strategy = strategy;

            if (strategy == CombinedMVPStrategy_WorldRequiredNoWorld) {
                if (g_config.combinedMVPLogDecomposition) {
                    LogMsg("Combined MVP ignored at c%d-c%d: world required but missing.",
                           static_cast<int>(baseReg), static_cast<int>(baseReg) + rows - 1);
                }
                return;
            }

            D3DMATRIX decompWorld = {};
            D3DMATRIX decompView = {};
            D3DMATRIX decompProj = {};
            ProjectionAnalysis projectionInfo = {};
            if (!TryDecomposeCombinedMVP(combinedMvp,
                                         worldAvailable ? &m_currentWorld : nullptr,
                                         worldAvailable,
                                         &decompWorld,
                                         &decompView,
                                         &decompProj,
                                         &projectionInfo)) {
                g_combinedMvpDebug.strategy = CombinedMVPStrategy_Failed;
                if (g_config.combinedMVPLogDecomposition) {
                    LogMsg("Combined MVP decomposition failed at c%d-c%d.",
                           static_cast<int>(baseReg), static_cast<int>(baseReg) + rows - 1);
                }
                return;
            }

            m_currentWorld = decompWorld;
            m_currentView = decompView;
            m_currentProj = decompProj;
            m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
            m_hasView = true;
                m_viewLastFrame = g_frameCount;
            m_hasProj = true;
            m_projLastFrame = g_frameCount;
            m_projDetectedFrame = g_frameCount;
            slotResolvedStructurally[MatrixSlot_World] = true;
            slotResolvedStructurally[MatrixSlot_View] = true;
            slotResolvedStructurally[MatrixSlot_Projection] = true;
            g_projectionDetectedByNumericStructure = true;
            g_projectionDetectedFovRadians = projectionInfo.fovRadians;
            g_projectionDetectedRegister = static_cast<int>(baseReg);
            g_projectionDetectedHandedness = projectionInfo.handedness;

            StoreWorldMatrix(m_currentWorld, shaderKey, static_cast<int>(baseReg), rows, transposed, false,
                             "combined MVP decomposition world", static_cast<int>(baseReg));
            StoreViewMatrix(m_currentView, shaderKey, static_cast<int>(baseReg), rows, transposed, false,
                            "combined MVP decomposition view", static_cast<int>(baseReg));
            StoreProjectionMatrix(m_currentProj, shaderKey, static_cast<int>(baseReg), rows, transposed, false,
                                  "combined MVP decomposition projection", static_cast<int>(baseReg));

            g_combinedMvpDebug.succeeded = true;
            g_combinedMvpDebug.fovRadians = projectionInfo.fovRadians;
            g_combinedMvpDebug.handedness = projectionInfo.handedness;
            if (g_config.combinedMVPLogDecomposition) {
                LogMsg("Combined MVP decomposition success at c%d-c%d using %s, FOV=%.2f deg, handedness=%s.",
                       static_cast<int>(baseReg), static_cast<int>(baseReg) + rows - 1,
                       CombinedMVPStrategyLabel(strategy),
                       projectionInfo.fovRadians * 180.0f / 3.14159265f,
                       ProjectionHandednessLabel(projectionInfo.handedness));
            }
        };

        bool suppressViewFromUpload = false;
        bool suppressWorldFromUpload = false;

        auto updateFromClassification = [&](D3DMATRIX mat, UINT baseReg, int rows, bool transposed) {
            MatrixClassification cls = ClassifyMatrixDeterministic(mat, rows, Vector4fCount, StartRegister, baseReg);

            if (cls == MatrixClass_Projection &&
                g_config.projMatrixRegister < 0 &&
                !slotResolvedByOverride[MatrixSlot_Projection] &&
                !slotResolvedStructurally[MatrixSlot_Projection]) {
                ProjectionAnalysis projectionInfo = {};
                if (!AnalyzeProjectionMatrixNumeric(mat, &projectionInfo)) return;
                if (projectionInfo.fovRadians < g_config.minFOV ||
                    projectionInfo.fovRadians > g_config.maxFOV) return;

                const bool sameSource =
                    (m_projLockedShader == 0) ||
                    (shaderKey == m_projLockedShader &&
                     static_cast<int>(baseReg) == m_projLockedRegister);
                if (sameSource) {
                    m_projLockedShader = shaderKey;
                    m_projLockedRegister = static_cast<int>(baseReg);
                    m_currentProj = mat;
                    m_hasProj = true;
                    m_projLastFrame = g_frameCount;
                    m_projDetectedFrame = g_frameCount;
                    slotResolvedStructurally[MatrixSlot_Projection] = true;
                    g_projectionDetectedByNumericStructure = true;
                    g_projectionDetectedFovRadians = projectionInfo.fovRadians;
                    g_projectionDetectedRegister = static_cast<int>(baseReg);
                    g_projectionDetectedHandedness = projectionInfo.handedness;
                    StoreProjectionMatrix(m_currentProj, shaderKey, static_cast<int>(baseReg),
                                          rows, transposed, false,
                                          "deterministic structural projection");
                    LogMsg("Projection accepted: c%d-c%d rows=%d fov=%.2f deg (%s)",
                           static_cast<int>(baseReg), static_cast<int>(baseReg) + rows - 1, rows,
                           projectionInfo.fovRadians * 180.0f / 3.14159265f,
                           ProjectionHandednessLabel(projectionInfo.handedness));
                }
            } else if (cls == MatrixClass_View &&
                       !suppressViewFromUpload &&
                       g_config.viewMatrixRegister < 0 &&
                       !slotResolvedByOverride[MatrixSlot_View] &&
                       !slotResolvedStructurally[MatrixSlot_View]) {
                const bool sameSource =
                    (m_viewLockedShader == 0) ||
                    (shaderKey == m_viewLockedShader &&
                     static_cast<int>(baseReg) == m_viewLockedRegister);
                if (sameSource) {
                    if (m_hasProj && m_projDetectedFrame == g_frameCount &&
                        !CrossValidateViewAgainstProjection(mat, m_currentProj)) {
                        // Column norms of candidateView * P do not match column norms of P.
                        // This is WV, VP, or some other combined form — not a pure view matrix.
                        // Do not mark slotResolvedStructurally so detection can continue.
                        return;
                    }
                    m_viewLockedShader = shaderKey;
                    m_viewLockedRegister = static_cast<int>(baseReg);
                    m_currentView = mat;
                    m_hasView = true;
                    m_viewLastFrame = g_frameCount;
                    slotResolvedStructurally[MatrixSlot_View] = true;
                    StoreViewMatrix(m_currentView, shaderKey, static_cast<int>(baseReg),
                                    rows, transposed, false,
                                    "deterministic structural view");
                }
            } else if (cls == MatrixClass_World &&
                       !suppressWorldFromUpload &&
                       g_config.worldMatrixRegister < 0 &&
                       !slotResolvedByOverride[MatrixSlot_World] &&
                       !slotResolvedStructurally[MatrixSlot_World]) {
                m_currentWorld = mat;
                m_hasWorld = true;
                m_worldLastFrame = g_frameCount;
                slotResolvedStructurally[MatrixSlot_World] = true;
                StoreWorldMatrix(m_currentWorld, shaderKey, static_cast<int>(baseReg), rows, transposed, false, "deterministic structural world");
            } else if (cls == MatrixClass_CombinedPerspective &&
                       g_config.worldMatrixRegister < 0 && g_config.viewMatrixRegister < 0 && g_config.projMatrixRegister < 0 &&
                       !slotResolvedByOverride[MatrixSlot_World] && !slotResolvedByOverride[MatrixSlot_View] && !slotResolvedByOverride[MatrixSlot_Projection] &&
                       !slotResolvedStructurally[MatrixSlot_World] && !slotResolvedStructurally[MatrixSlot_View] && !slotResolvedStructurally[MatrixSlot_Projection] &&
                       rows == 4) {
                tryHandleCombinedMVP(mat, baseReg, rows, transposed);
            }
        };

        g_profileDisableStructuralDetection = false;
        const bool allowStructuralDetection = !profileActive;

        if (allowStructuralDetection && effectiveConstantData && Vector4fCount >= 12) {
            if (CountStridedCandidates(effectiveConstantData, StartRegister,
                                      Vector4fCount, 4u, MatrixClass_View) > 2) {
                suppressViewFromUpload = true;
            }
            if (CountStridedCandidates(effectiveConstantData, StartRegister,
                                      Vector4fCount, 4u, MatrixClass_World) > 2) {
                suppressWorldFromUpload = true;
            }
            if (!suppressViewFromUpload &&
                CountStridedCandidates(effectiveConstantData, StartRegister,
                                      Vector4fCount, 3u, MatrixClass_View) > 2) {
                suppressViewFromUpload = true;
            }
            if (!suppressWorldFromUpload &&
                CountStridedCandidates(effectiveConstantData, StartRegister,
                                      Vector4fCount, 3u, MatrixClass_World) > 2) {
                suppressWorldFromUpload = true;
            }
        }

        bool anyStructuralMatch = false;
        if (allowStructuralDetection && effectiveConstantData && Vector4fCount >= 3) {
            for (UINT rows : {4u, 3u}) {
                if (Vector4fCount < rows) {
                    continue;
                }
                for (UINT offset = 0; offset + rows <= Vector4fCount; ++offset) {
                    UINT baseReg = StartRegister + offset;
                    D3DMATRIX mat = {};
                    if (!TryBuildMatrixFromConstantUpdate(effectiveConstantData + offset * 4, baseReg, rows,
                                                          static_cast<int>(baseReg), static_cast<int>(rows),
                                                          false, &mat)) {
                        continue;
                    }

                    MatrixClassification directClass = ClassifyMatrixDeterministic(mat, static_cast<int>(rows), Vector4fCount, StartRegister, baseReg);
                    bool transposed = false;
                    if (directClass == MatrixClass_None && g_probeTransposedLayouts) {
                        D3DMATRIX t = TransposeMatrix(mat);
                        MatrixClassification transposedClass = ClassifyMatrixDeterministic(t, static_cast<int>(rows), Vector4fCount, StartRegister, baseReg);
                        if (transposedClass != MatrixClass_None) {
                            mat = t;
                            transposed = true;
                        }
                    }

                    MatrixClassification finalClass = ClassifyMatrixDeterministic(mat, static_cast<int>(rows), Vector4fCount, StartRegister, baseReg);
                    if (rows == 3u &&
                        (finalClass == MatrixClass_View || finalClass == MatrixClass_World) &&
                        IsThreeRowPrefixOfPerspectiveMatrix(effectiveConstantData,
                                                            StartRegister,
                                                            Vector4fCount,
                                                            baseReg,
                                                            transposed)) {
                        finalClass = MatrixClass_None;
                    }
                    if (finalClass == MatrixClass_None && g_probeInverseView && rows == 4u) {
                        const float row0Len = sqrtf(Dot3(mat._11, mat._12, mat._13, mat._11, mat._12, mat._13));
                        const float row1Len = sqrtf(Dot3(mat._21, mat._22, mat._23, mat._21, mat._22, mat._23));
                        const float row2Len = sqrtf(Dot3(mat._31, mat._32, mat._33, mat._31, mat._32, mat._33));
                        const bool orthonormal =
                            fabsf(row0Len - 1.0f) < 0.05f &&
                            fabsf(row1Len - 1.0f) < 0.05f &&
                            fabsf(row2Len - 1.0f) < 0.05f &&
                            fabsf(Dot3(mat._11, mat._12, mat._13, mat._21, mat._22, mat._23)) < 0.05f &&
                            fabsf(Dot3(mat._11, mat._12, mat._13, mat._31, mat._32, mat._33)) < 0.05f &&
                            fabsf(Dot3(mat._21, mat._22, mat._23, mat._31, mat._32, mat._33)) < 0.05f;
                        if (orthonormal) {
                            D3DMATRIX inverseView = InvertSimpleRigidView(mat);
                            MatrixClassification inverseClass = ClassifyMatrixDeterministic(inverseView, static_cast<int>(rows), Vector4fCount, StartRegister, baseReg);
                            if (inverseClass == MatrixClass_View) {
                                mat = inverseView;
                                finalClass = inverseClass;
                            }
                        }
                    }
                    if (finalClass != MatrixClass_None) {
                        anyStructuralMatch = true;
                        updateFromClassification(mat, baseReg, static_cast<int>(rows), transposed);
                    }

                    const bool allSlotsResolved =
                        (slotResolvedStructurally[MatrixSlot_View] || slotResolvedByOverride[MatrixSlot_View] || g_config.viewMatrixRegister >= 0) &&
                        (slotResolvedStructurally[MatrixSlot_Projection] || slotResolvedByOverride[MatrixSlot_Projection] || g_config.projMatrixRegister >= 0) &&
                        (slotResolvedStructurally[MatrixSlot_World] || slotResolvedByOverride[MatrixSlot_World] || g_config.worldMatrixRegister >= 0);
                    if (allSlotsResolved) goto done_scanning;
                }
            }
        }
        done_scanning:

        if (g_config.logAllConstants && m_constantLogThrottle == 0 && Vector4fCount >= 4) {
            LogMsg("SetVertexShaderConstantF: c%d-%d (%d vectors)",
                   StartRegister, StartRegister + Vector4fCount - 1, Vector4fCount);
            for (UINT i = 0; i < Vector4fCount && i < 4; i++) {
                LogMsg("  c%d: [%.3f, %.3f, %.3f, %.3f]",
                       StartRegister + i,
                       effectiveConstantData[i*4+0], effectiveConstantData[i*4+1],
                       effectiveConstantData[i*4+2], effectiveConstantData[i*4+3]);
            }
        }

        return m_real->SetVertexShaderConstantF(StartRegister, effectiveConstantData, Vector4fCount);
    }

    // Present - good place to do per-frame logging throttle
    HRESULT STDMETHODCALLTYPE Present(const RECT* pSourceRect, const RECT* pDestRect,
                                       HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override {
        g_frameCount++;

        // Reset per-frame source locks. Locks set during SetVertexShaderConstantF calls
        // this frame will be validated against these — after Present they reset for the
        // next frame. This ensures correct behavior regardless of BeginScene call count.
        if (g_activeGameProfile == GameProfile_None) {
            m_viewLockedShader = 0;
            m_viewLockedRegister = -1;
            m_projLockedShader = 0;
            m_projLockedRegister = -1;
        }

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
        }

        if (!g_imguiInitialized) {
            InitializeImGui(m_real, m_hwnd);
        }
        UpdateHotkeys();
        if (g_imguiInitialized) {
            ImGui::GetIO().MouseDrawCursor = g_showImGui;
        }
        g_imguiMgrrUseAutoProjection = m_mgrrUseAutoProjection;
        g_imguiBarnyardUseGameSetTransformsForViewProjection = g_config.barnyardUseGameSetTransformsForViewProjection;
        RenderImGuiOverlay();
        m_mgrrUseAutoProjection = g_imguiMgrrUseAutoProjection;
        g_config.barnyardUseGameSetTransformsForViewProjection = g_imguiBarnyardUseGameSetTransformsForViewProjection;
        if (g_requestManualEmit) {
            EmitFixedFunctionTransforms();
            g_requestManualEmit = false;
            if (g_activeGameProfile == GameProfile_Barnyard) {
                snprintf(g_manualEmitStatus, sizeof(g_manualEmitStatus),
                         g_config.barnyardUseGameSetTransformsForViewProjection
                             ? "Sent cached World/View/Projection matrices to RTX Remix via SetTransform()."
                             : "Sent cached World matrix to RTX Remix via SetTransform().");
            } else {
                snprintf(g_manualEmitStatus, sizeof(g_manualEmitStatus),
                         "Sent cached World/View/Projection matrices to RTX Remix via SetTransform().");
            }
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
            // Do NOT call ImGui_ImplWin32_Shutdown here.
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
    HRESULT STDMETHODCALLTYPE BeginScene() override {
        g_combinedMvpDebug = {};
        if (g_activeGameProfile == GameProfile_MetalGearRising) {
            // MGR frame lifecycle: keep projection/view persistent across draws and frames,
            // but require a fresh world upload for each frame.
            CreateIdentityMatrix(&m_currentWorld);
            m_hasWorld = false;
            g_mgrWorldCapturedForDraw = false;
            g_mgrProjCapturedThisFrame = false;
            g_mgrViewCapturedThisFrame = false;
            g_mgrProjectionRegisterValid = false;
        } else if (g_activeGameProfile == GameProfile_None) {
            m_viewLockedShader = 0;
            m_viewLockedRegister = -1;
            m_projLockedShader = 0;
            m_projLockedRegister = -1;
        }
        return m_real->BeginScene();
    }
    HRESULT STDMETHODCALLTYPE EndScene() override { return m_real->EndScene(); }
    HRESULT STDMETHODCALLTYPE Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override { return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) override {
        int transformIdx = -1;
        if (State == D3DTS_WORLD) transformIdx = 0;
        if (State == D3DTS_VIEW) transformIdx = 1;
        if (State == D3DTS_PROJECTION) transformIdx = 2;
        if (transformIdx >= 0 && pMatrix) {
            g_gameSetTransformSeen[transformIdx] = true;
            g_gameSetTransformAnySeen = true;

            if (g_config.setTransformBypassProxyWhenGameProvides) {
                if (State == D3DTS_WORLD) {
                    m_currentWorld = *pMatrix;
                    m_hasWorld = true;
                    m_worldLastFrame = g_frameCount;
                    StoreWorldMatrix(m_currentWorld, 0, -1, 4, false, true,
                                     "game SetTransform(World) direct passthrough");
                } else if (State == D3DTS_VIEW) {
                    m_currentView = *pMatrix;
                    m_hasView = true;
                    m_viewLastFrame = g_frameCount;
                    StoreViewMatrix(m_currentView, 0, -1, 4, false, true,
                                    "game SetTransform(View) direct passthrough");
                } else if (State == D3DTS_PROJECTION) {
                    m_currentProj = *pMatrix;
                    m_hasProj = true;
                    m_projLastFrame = g_frameCount;
                    StoreProjectionMatrix(m_currentProj, 0, -1, 4, false, true,
                                          "game SetTransform(Projection) direct passthrough");
                }
                return m_real->SetTransform(State, pMatrix);
            }

            if (g_config.setTransformRoundTripCompatibilityMode) {
                const HRESULT setHr = m_real->SetTransform(State, pMatrix);
                if (FAILED(setHr)) {
                    return setHr;
                }
                D3DMATRIX roundTrip = *pMatrix;
                if (SUCCEEDED(m_real->GetTransform(State, &roundTrip))) {
                    m_real->SetTransform(State, &roundTrip);
                }
                if (State == D3DTS_WORLD) {
                    m_currentWorld = roundTrip;
                    m_hasWorld = true;
                    m_worldLastFrame = g_frameCount;
                    StoreWorldMatrix(m_currentWorld, 0, -1, 4, false, true,
                                     "game SetTransform(World)+GetTransform compatibility");
                } else if (State == D3DTS_VIEW) {
                    m_currentView = roundTrip;
                    m_hasView = true;
                    m_viewLastFrame = g_frameCount;
                    StoreViewMatrix(m_currentView, 0, -1, 4, false, true,
                                    "game SetTransform(View)+GetTransform compatibility");
                } else if (State == D3DTS_PROJECTION) {
                    m_currentProj = roundTrip;
                    m_hasProj = true;
                    m_projLastFrame = g_frameCount;
                    StoreProjectionMatrix(m_currentProj, 0, -1, 4, false, true,
                                          "game SetTransform(Projection)+GetTransform compatibility");
                }
                return setHr;
            }
        }

        if (g_activeGameProfile == GameProfile_Barnyard &&
            (State == D3DTS_VIEW || State == D3DTS_PROJECTION)) {
            if (!g_config.barnyardUseGameSetTransformsForViewProjection || !pMatrix) {
                return D3D_OK;
            }

            const HRESULT setHr = m_real->SetTransform(State, pMatrix);
            D3DMATRIX captured = *pMatrix;
            D3DMATRIX roundTrip = {};
            const HRESULT getHr = m_real->GetTransform(State, &roundTrip);
            if (SUCCEEDED(getHr)) {
                captured = roundTrip;
            }

            if (State == D3DTS_VIEW) {
                m_currentView = captured;
                m_hasView = true;
                m_viewLastFrame = g_frameCount;
                g_profileCoreRegistersSeen[0] = true;
                StoreViewMatrix(m_currentView, 0, -1, 4, false, true,
                                SUCCEEDED(getHr)
                                    ? "Barnyard intercepted game SetTransform(View)+GetTransform"
                                    : "Barnyard intercepted game SetTransform(View)");
            } else {
                m_currentProj = captured;
                m_hasProj = true;
                m_projLastFrame = g_frameCount;
                g_profileCoreRegistersSeen[1] = true;
                g_projectionDetectedByNumericStructure = false;
                g_projectionDetectedRegister = -1;
                g_projectionDetectedHandedness = ProjectionHandedness_Unknown;
                g_projectionDetectedFovRadians = ExtractFOV(captured);
                StoreProjectionMatrix(m_currentProj, 0, -1, 4, false, true,
                                      SUCCEEDED(getHr)
                                          ? "Barnyard intercepted game SetTransform(Projection)+GetTransform"
                                          : "Barnyard intercepted game SetTransform(Projection)");
            }

            snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                     "Barnyard intercepted game %s transform and cached for draw-time forwarding.",
                     State == D3DTS_VIEW ? "VIEW" : "PROJECTION");
            return setHr;
        }
        return m_real->SetTransform(State, pMatrix);
    }
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
        EmitFixedFunctionTransforms();
        return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override {
        if ((g_pauseRendering || IsCurrentShaderDrawDisabled(m_currentVertexShader)) && !g_isRenderingImGui) {
            return D3D_OK;
        }
        EmitFixedFunctionTransforms();
        return m_real->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override {
        if ((g_pauseRendering || IsCurrentShaderDrawDisabled(m_currentVertexShader)) && !g_isRenderingImGui) {
            return D3D_OK;
        }
        EmitFixedFunctionTransforms();
        return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override {
        if ((g_pauseRendering || IsCurrentShaderDrawDisabled(m_currentVertexShader)) && !g_isRenderingImGui) {
            return D3D_OK;
        }
        EmitFixedFunctionTransforms();
        return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override { return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override { return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override { return m_real->SetVertexDeclaration(pDecl); }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override { return m_real->GetVertexDeclaration(ppDecl); }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { return m_real->SetFVF(FVF); }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override { return m_real->GetFVF(pFVF); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) override {
        if (!ppShader) {
            return D3DERR_INVALIDCALL;
        }
        IDirect3DVertexShader9* realShader = nullptr;
        HRESULT hr = m_real->CreateVertexShader(pFunction, &realShader);
        if (FAILED(hr) || !realShader) {
            *ppShader = nullptr;
            return hr;
        }
        *ppShader = new WrappedVertexShader9(realShader);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override {
        m_currentVertexShader = pShader;
        g_activeShaderKey = reinterpret_cast<uintptr_t>(pShader);
        GetShaderState(g_activeShaderKey, true);

        IDirect3DVertexShader9* realShader = nullptr;
        if (pShader) {
            WrappedVertexShader9* wrapped = static_cast<WrappedVertexShader9*>(pShader);
            realShader = wrapped->GetReal();
            uint32_t shaderHash = ComputeShaderBytecodeHash(realShader);
            if (shaderHash != 0) {
                g_shaderBytecodeHashes[g_activeShaderKey] = shaderHash;
            }
        }

        return m_real->SetVertexShader(realShader);
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
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override {
        m_currentPixelShader = pShader;
        return m_real->SetPixelShader(pShader);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override { return m_real->GetPixelShader(ppShader); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) override {
        uintptr_t shaderKey = reinterpret_cast<uintptr_t>(m_currentPixelShader);
        if (g_constantUploadRecordingEnabled) {
            RecordConstantUpload(ConstantUploadStage_Pixel, shaderKey, StartRegister, Vector4fCount);
        }
        return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override { return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) override { return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override { return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) override { return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override { return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) override { return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) override { return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return m_real->DeletePatch(Handle); }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override { return m_real->CreateQuery(Type, ppQuery); }

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns) override {
        return m_realEx ? m_realEx->SetConvolutionMonoKernel(width, height, rows, columns) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs,
                                           UINT numRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP op, int xoffset, int yoffset) override {
        return m_realEx ? m_realEx->ComposeRects(pSrc, pDst, pSrcRectDescs, numRects, pDstRectDescs, op, xoffset, yoffset) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE PresentEx(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride,
                                        CONST RGNDATA* pDirtyRegion, DWORD dwFlags) override {
        return m_realEx ? m_realEx->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags) : Present(pSourceRect,pDestRect,hDestWindowOverride,pDirtyRegion);
    }
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override { return m_realEx ? m_realEx->GetGPUThreadPriority(pPriority) : D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override { return m_realEx ? m_realEx->SetGPUThreadPriority(Priority) : D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT iSwapChain) override { return m_realEx ? m_realEx->WaitForVBlank(iSwapChain) : D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) override { return m_realEx ? m_realEx->CheckResourceResidency(pResourceArray, NumResources) : D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { return m_realEx ? m_realEx->SetMaximumFrameLatency(MaxLatency) : D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override { return m_realEx ? m_realEx->GetMaximumFrameLatency(pMaxLatency) : D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND hDestinationWindow) override { return m_realEx ? m_realEx->CheckDeviceState(hDestinationWindow) : D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                                   DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface,
                                                   HANDLE* pSharedHandle, DWORD Usage) override {
        return m_realEx ? m_realEx->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle, Usage) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                                            IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) override {
        return m_realEx ? m_realEx->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                                          DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface,
                                                          HANDLE* pSharedHandle, DWORD Usage) override {
        return m_realEx ? m_realEx->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle, Usage) : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) override {
        return m_realEx ? m_realEx->ResetEx(pPresentationParameters, pFullscreenDisplayMode) : Reset(pPresentationParameters);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) override {
        return m_realEx ? m_realEx->GetDisplayModeEx(iSwapChain, pMode, pRotation) : D3DERR_INVALIDCALL;
    }
};

class WrappedD3D9DeviceEx : public WrappedD3D9Device {
public:
    explicit WrappedD3D9DeviceEx(IDirect3DDevice9Ex* real)
        : WrappedD3D9Device(real) {}
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
            LogMsg("CreateDeviceEx succeeded, wrapping device");
            *ppReturnedDeviceInterface = new WrappedD3D9DeviceEx(realDevice);
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

    g_config.viewMatrixRegister = GetPrivateProfileIntA("CameraProxy", "ViewMatrixRegister", -1, path);
    g_config.projMatrixRegister = GetPrivateProfileIntA("CameraProxy", "ProjMatrixRegister", -1, path);
    g_config.worldMatrixRegister = GetPrivateProfileIntA("CameraProxy", "WorldMatrixRegister", -1, path);
    g_iniViewMatrixRegister = g_config.viewMatrixRegister;
    g_iniProjMatrixRegister = g_config.projMatrixRegister;
    g_iniWorldMatrixRegister = g_config.worldMatrixRegister;
    g_config.enableLogging = GetPrivateProfileIntA("CameraProxy", "EnableLogging", 1, path) != 0;
    g_config.logAllConstants = GetPrivateProfileIntA("CameraProxy", "LogAllConstants", 0, path) != 0;
    g_config.autoDetectMatrices = GetPrivateProfileIntA("CameraProxy", "AutoDetectMatrices", 0, path) != 0;
    g_config.enableMemoryScanner = GetPrivateProfileIntA("CameraProxy", "EnableMemoryScanner", 0, path) != 0;
    g_config.memoryScannerIntervalSec = GetPrivateProfileIntA("CameraProxy", "MemoryScannerIntervalSec", 0, path);
    g_config.memoryScannerMaxResults = GetPrivateProfileIntA("CameraProxy", "MemoryScannerMaxResults", 25, path);
    GetPrivateProfileStringA("CameraProxy", "MemoryScannerModule", "", g_config.memoryScannerModule,
                             MAX_PATH, path);
    g_config.useRemixRuntime = GetPrivateProfileIntA("CameraProxy", "UseRemixRuntime", 1, path) != 0;
    g_config.emitFixedFunctionTransforms = GetPrivateProfileIntA("CameraProxy", "EmitFixedFunctionTransforms", 1, path) != 0;
    GetPrivateProfileStringA("CameraProxy", "GameProfile", "", g_config.gameProfile,
                             static_cast<DWORD>(sizeof(g_config.gameProfile)), path);
    g_activeGameProfile = ParseGameProfile(g_config.gameProfile);
    ConfigureActiveProfileLayout();
    g_profileCoreRegistersSeen[0] = g_profileCoreRegistersSeen[1] = g_profileCoreRegistersSeen[2] = false;
    g_profileOptionalRegistersSeen[0] = g_profileOptionalRegistersSeen[1] = false;
    g_profileViewDerivedFromInverse = false;
    g_profileStatusMessage[0] = '\0';
    g_profileDisableStructuralDetection = false;
    g_barnyardForceWorldFromC0 = GetPrivateProfileIntA("CameraProxy", "BarnyardForceWorldFromC0", 0, path) != 0;
    g_config.barnyardUseGameSetTransformsForViewProjection =
        GetPrivateProfileIntA("CameraProxy", "BarnyardUseGameSetTransformsForViewProjection", 1, path) != 0;
    g_config.disableGameInputWhileMenuOpen =
        GetPrivateProfileIntA("CameraProxy", "DisableGameInputWhileMenuOpen", 0, path) != 0;
    g_config.setTransformBypassProxyWhenGameProvides =
        GetPrivateProfileIntA("CameraProxy", "SetTransformBypassProxyWhenGameProvides", 0, path) != 0;
    g_config.setTransformRoundTripCompatibilityMode =
        GetPrivateProfileIntA("CameraProxy", "SetTransformRoundTripCompatibilityMode", 0, path) != 0;
    g_imguiBarnyardUseGameSetTransformsForViewProjection = g_config.barnyardUseGameSetTransformsForViewProjection;
    g_imguiDisableGameInputWhileMenuOpen = g_config.disableGameInputWhileMenuOpen;
    if (g_config.gameProfile[0] != '\0' && g_activeGameProfile == GameProfile_None) {
        snprintf(g_profileStatusMessage, sizeof(g_profileStatusMessage),
                 "Unknown GameProfile='%s'. Falling back to structural detection.", g_config.gameProfile);
    }
    g_config.imguiScale = GetPrivateProfileIntA("CameraProxy", "ImGuiScalePercent", 100, path) / 100.0f;
    if (g_config.imguiScale < 0.5f) g_config.imguiScale = 0.5f;
    if (g_config.imguiScale > 3.0f) g_config.imguiScale = 3.0f;
    GetPrivateProfileStringA("CameraProxy", "RemixDllName", "d3d9_remix.dll", g_config.remixDllName,
                             MAX_PATH, path);
    g_probeTransposedLayouts = GetPrivateProfileIntA("CameraProxy", "ProbeTransposedLayouts", 1, path) != 0;
    g_probeInverseView = GetPrivateProfileIntA("CameraProxy", "ProbeInverseView", 1, path) != 0;
    g_overrideScopeMode = GetPrivateProfileIntA("CameraProxy", "OverrideScopeMode", Override_Sticky, path);
    g_overrideNFrames = GetPrivateProfileIntA("CameraProxy", "OverrideNFrames", 3, path);
    g_config.hotkeyToggleMenuVk = GetPrivateProfileIntA("CameraProxy", "HotkeyToggleMenuVK", VK_F10, path);
    g_config.hotkeyTogglePauseVk = GetPrivateProfileIntA("CameraProxy", "HotkeyTogglePauseVK", VK_F9, path);
    g_config.hotkeyEmitMatricesVk = GetPrivateProfileIntA("CameraProxy", "HotkeyEmitMatricesVK", VK_F8, path);
    g_config.hotkeyResetMatrixOverridesVk = GetPrivateProfileIntA("CameraProxy", "HotkeyResetMatrixOverridesVK", VK_F7, path);
    g_config.enableCombinedMVP = GetPrivateProfileIntA("CameraProxy", "EnableCombinedMVP", 0, path) != 0;
    g_config.combinedMVPRequireWorld = GetPrivateProfileIntA("CameraProxy", "CombinedMVPRequireWorld", 0, path) != 0;
    g_config.combinedMVPAssumeIdentityWorld = GetPrivateProfileIntA("CameraProxy", "CombinedMVPAssumeIdentityWorld", 1, path) != 0;
    g_config.combinedMVPForceDecomposition = GetPrivateProfileIntA("CameraProxy", "CombinedMVPForceDecomposition", 0, path) != 0;
    g_config.combinedMVPLogDecomposition = GetPrivateProfileIntA("CameraProxy", "CombinedMVPLogDecomposition", 0, path) != 0;

    g_config.experimentalCustomProjectionEnabled =
        GetPrivateProfileIntA("CameraProxy", "ExperimentalCustomProjectionEnabled", 0, path) != 0;
    int rawMode = GetPrivateProfileIntA("CameraProxy", "ExperimentalCustomProjectionMode", 2, path);
    g_config.experimentalCustomProjectionMode =
        (rawMode == CustomProjectionMode_Manual) ? CustomProjectionMode_Manual : CustomProjectionMode_Auto;
    g_config.experimentalCustomProjectionOverrideDetectedProjection =
        GetPrivateProfileIntA("CameraProxy", "ExperimentalCustomProjectionOverrideDetectedProjection", 0, path) != 0;
    g_config.experimentalCustomProjectionOverrideCombinedMVP =
        GetPrivateProfileIntA("CameraProxy", "ExperimentalCustomProjectionOverrideCombinedMVP", 0, path) != 0;
    g_config.experimentalInverseViewAsWorld =
        GetPrivateProfileIntA("CameraProxy", "ExperimentalInverseViewAsWorld", 0, path) != 0;
    g_config.experimentalInverseViewAsWorldAllowUnverified =
        GetPrivateProfileIntA("CameraProxy", "ExperimentalInverseViewAsWorldAllowUnverified", 0, path) != 0;
    g_config.experimentalInverseViewAsWorldFast =
        GetPrivateProfileIntA("CameraProxy", "ExperimentalInverseViewAsWorldFast", 0, path) != 0;
    g_config.mgrrUseAutoProjectionWhenC4Invalid =
        GetPrivateProfileIntA("CameraProxy", "MGRRUseAutoProjectionWhenC4Invalid", 0, path) != 0;

    char customBuf[64] = {};
    GetPrivateProfileStringA("CameraProxy", "ExperimentalCustomProjectionAutoFovDeg", "60.0", customBuf, sizeof(customBuf), path);
    g_config.experimentalCustomProjectionAutoFovDeg = (float)atof(customBuf);
    GetPrivateProfileStringA("CameraProxy", "ExperimentalCustomProjectionAutoNearZ", "0.1", customBuf, sizeof(customBuf), path);
    g_config.experimentalCustomProjectionAutoNearZ = (float)atof(customBuf);
    GetPrivateProfileStringA("CameraProxy", "ExperimentalCustomProjectionAutoFarZ", "1000.0", customBuf, sizeof(customBuf), path);
    g_config.experimentalCustomProjectionAutoFarZ = (float)atof(customBuf);
    GetPrivateProfileStringA("CameraProxy", "ExperimentalCustomProjectionAutoAspectFallback", "1.7777778", customBuf, sizeof(customBuf), path);
    g_config.experimentalCustomProjectionAutoAspectFallback = (float)atof(customBuf);
    int rawHandedness =
        GetPrivateProfileIntA("CameraProxy", "ExperimentalCustomProjectionAutoHandedness", ProjectionHandedness_Left, path);
    g_config.experimentalCustomProjectionAutoHandedness =
        (rawHandedness == ProjectionHandedness_Right) ? ProjectionHandedness_Right : ProjectionHandedness_Left;

    D3DMATRIX defaultManualProjection = {};
    CreateProjectionMatrixWithHandedness(&defaultManualProjection,
                                         60.0f * (3.14159265f / 180.0f),
                                         16.0f / 9.0f,
                                         0.1f,
                                         1000.0f,
                                         ProjectionHandedness_Left);
    g_config.experimentalCustomProjectionManualMatrix = defaultManualProjection;
    float* manualValues = reinterpret_cast<float*>(&g_config.experimentalCustomProjectionManualMatrix);
    const float* defaultManualValues = reinterpret_cast<const float*>(&defaultManualProjection);
    for (int i = 0; i < 16; ++i) {
        int row = (i / 4) + 1;
        int col = (i % 4) + 1;
        char key[64] = {};
        snprintf(key, sizeof(key), "ExperimentalCustomProjectionM%d%d", row, col);
        snprintf(customBuf, sizeof(customBuf), "%.7g", defaultManualValues[i]);
        GetPrivateProfileStringA("CameraProxy", key, customBuf, customBuf, sizeof(customBuf), path);
        manualValues[i] = (float)atof(customBuf);
    }

    char buf[64];
    GetPrivateProfileStringA("CameraProxy", "MinFOV", "0.1", buf, sizeof(buf), path);
    g_config.minFOV = (float)atof(buf);
    GetPrivateProfileStringA("CameraProxy", "MaxFOV", "2.5", buf, sizeof(buf), path);
    g_config.maxFOV = (float)atof(buf);
    snprintf(g_iniPath, sizeof(g_iniPath), "%s", path);
}

static void EnsureProxyInitialized() {
    std::call_once(g_initOnce, []() {
        LoadConfig();
        if (g_config.enableLogging) {
            g_logFile = fopen("camera_proxy.log", "w");
            LogMsg("=== DMC4 Camera Proxy for D3D9 ===");
        }
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
        }
    });
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    (void)lpvReserved;
    if (fdwReason == DLL_PROCESS_ATTACH) {
        g_moduleInstance = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
    } else if (fdwReason == DLL_PROCESS_DETACH) {
        g_moduleInstance = nullptr;
        if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
        if (g_hD3D9) { FreeLibrary(g_hD3D9); g_hD3D9 = nullptr; }
    }
    return TRUE;
}

// Exported functions - these are what the game calls
// Named with Proxy_ prefix to avoid conflict with SDK declarations
// The .def file maps these to the real export names

extern "C" {
    __declspec(dllexport) const CameraMatrices* WINAPI Proxy_GetCameraMatrices() {
        static CameraMatrices snapshot = {};
        std::lock_guard<std::mutex> lock(g_cameraMatricesMutex);
        snapshot = g_cameraMatrices;
        return &snapshot;
    }

    IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT SDKVersion) {
        EnsureProxyInitialized();
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
        EnsureProxyInitialized();
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
