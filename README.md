# Camera Proxy for RTX Remix

(copied over from mencelot/dmc4-camera-proxy)A D3D9 proxy DLL that intercepts Direct3D 9 calls from Devil May Cry 4, extracts camera matrices from vertex shader constants, and provides them to RTX Remix via `SetTransform()`.

DMC4 uses a fully programmable shader pipeline and never calls the fixed-function `SetTransform()` API that Remix relies on for camera information. This proxy sits between the game and Remix's `d3d9.dll`, intercepts `SetVertexShaderConstantF` calls, identifies the MVP matrix at shader registers `c0-c3`, and forwards the extracted view/projection matrices to Remix.

## Setup

1. Install [RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix) into your DMC4 game folder
2. Rename Remix's `d3d9.dll` to `d3d9_remix.dll`
3. Copy `d3d9.dll` (this proxy) into the game folder
4. Copy `camera_proxy.ini` into the game folder
5. Optionally copy `rtx.conf` for tuned Remix settings
6. Run the game

The proxy can chain to either `d3d9_remix.dll` (default) or the system `d3d9.dll`, controlled by `camera_proxy.ini`.

## Building

Requires Visual Studio with the C++ workload installed.

**Option A** - From a VS Developer Command Prompt:
```
build.bat
```

**Option B** - From any terminal (auto-detects VS):
```
do_build.bat
```

Both produce a 32-bit `d3d9.dll` (DMC4 is a 32-bit application).

## Configuration

Edit `camera_proxy.ini` to adjust behavior:

| Setting | Default | Description |
|---------|---------|-------------|
| `ViewMatrixRegister` | 4 | Shader constant register for the view matrix |
| `ProjMatrixRegister` | 8 | Shader constant register for the projection matrix |
| `UseRemixRuntime` | 1 | Load Remix runtime DLL (`RemixDllName`) instead of system `d3d9.dll` |
| `RemixDllName` | d3d9_remix.dll | Runtime DLL name/path used when `UseRemixRuntime=1` |
| `EmitFixedFunctionTransforms` | 1 | Emit `SetTransform` for WORLD/VIEW/PROJECTION as matrices are extracted |
| `EnableLogging` | 1 | Write diagnostic output to `camera_proxy.log` |
| `AutoDetectMatrices` | 0 | Scan all constants for view/projection matrices |
| `LogAllConstants` | 0 | Log all shader constant updates (very verbose) |
| `LayoutStrategyMode` | 0 | Heuristic layout strategy (0=Auto, 1=4x4, 2=4x3, 3=VP, 4=MVP) |
| `ProbeTransposedLayouts` | 1 | Probe transposed row/column-major matrix conventions |
| `ProbeInverseView` | 1 | Probe inverse-view interpretation for view candidates |
| `AutoPickCandidates` | 1 | Auto-apply top view/projection candidates in Heuristics tab |
| `OverrideScopeMode` | 0 | Shader override lifetime (0=Sticky, 1=One-frame, 2=N-frames) |
| `OverrideNFrames` | 3 | Lifetime in frames when `OverrideScopeMode=2` |

You can also open the in-game constants view and enable **shader constant editing** to override individual `c#` registers live; overrides are injected into the final `SetVertexShaderConstantF` call (so changes affect rendering) and can be reset per-register or globally from the UI.

The ImGui overlay is now organized into tabs for **Camera**, **Constants**, **Heuristics**, **Memory Scanner**, and **Logs** so matrix candidates, memory-scan results, and runtime logs can be reviewed in one place.

Heuristic profiles can be saved per shader (hashed by shader bytecode) and persisted in `camera_proxy_profiles.ini` to avoid repeating setup across runs.

## Troubleshooting

- Check `camera_proxy.log` in the game folder for diagnostic output
- If the camera isn't working, try setting `AutoDetectMatrices=1` in the ini
- For manual analysis, set `LogAllConstants=1` and inspect the log to find which registers contain camera data
