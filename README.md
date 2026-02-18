# camera-proxy (experimental branch)

Experimental Direct3D9 proxy DLL for RTX Remix focused on deterministic matrix extraction and draw-time state emission for programmable DX9 games.

## Branch status at a glance

This **experimental** branch now includes:

- deterministic matrix classification from vertex shader constants,
- draw-time fixed-function transform emission for Remix compatibility,
- game-specific extraction profiles (Metal Gear Rising, Barnyard),
- transposed/inverse probing safeguards,
- expanded input compatibility with frame-polled hotkeys,
- ImGui overlay scaling controls,
- experimental mesh attribute forwarding (normal/tangent/binormal generation), and
- diagnostics-rich overlay tabs for camera state, constants, normals, memory scanning, and logs.

The main goal remains: keep camera/transform extraction stable across engines that do not reliably use fixed-function `SetTransform()` timing.

---

## Why this branch exists

Many DX9 titles are heavily shader-driven and never issue fixed-function transform calls in the way RTX Remix expects. Forwarding transforms at constant-upload time (`SetVertexShaderConstantF`) is often temporally wrong for the draw that eventually consumes those constants.

This proxy resolves that mismatch by:

1. observing and caching candidate transform matrices from constant uploads,
2. classifying them with deterministic structural checks,
3. emitting `D3DTS_WORLD`, `D3DTS_VIEW`, and `D3DTS_PROJECTION` immediately before intercepted draw calls.

---

## Core extraction pipeline

### 1) Matrix state caching

The proxy caches:

- World
- View
- Projection

State updates happen on constant upload analysis; forwarding is deferred until draw interception.

### 2) Deterministic structural classification

Observed constant windows are interpreted as `4x4` / `4x3` candidates.

Classification logic:

- **Projection**: strict perspective structure + `MinFOV` / `MaxFOV` validation.
- **View**: strict orthonormal affine camera-style matrix.
- **World**: affine matrix that is not strict View.
- **Perspective fallback**: if perspective-like but not strict Projection, World and View are set to identity and the matrix is forwarded as Projection.

No probabilistic ranking or MVP decomposition is used in this path.

### 3) Layout probing

When `ProbeTransposedLayouts=1`, failed candidates are transposed once and re-validated deterministically.

### 4) Register override controls

`ViewMatrixRegister`, `ProjMatrixRegister`, `WorldMatrixRegister`:

- `>= 0` → only that base register is accepted for the corresponding type.
- `-1` → classify from all observed constant ranges.

### 5) Draw-time emission

When `EmitFixedFunctionTransforms=1`, cached WORLD/VIEW/PROJECTION are emitted before each intercepted draw call:

- `DrawPrimitive`
- `DrawIndexedPrimitive`
- `DrawPrimitiveUP`
- `DrawIndexedPrimitiveUP`

Unknown matrix types fall back to identity to preserve valid fixed-function state.

---

## Game profile support

Enable with `GameProfile` in `camera_proxy.ini`.

### Metal Gear Rising (`GameProfile=MetalGearRising`)

Mapped registers:

- `c4-c7` → Projection
- `c12-c15` → View Inverse (deterministically inverted to derive View)
- `c16-c19` → World

Optional tracking-only ranges:

- `c8-c11` (ViewProjection)
- `c20-c23` (WorldView)

If expected uploads are missing or inverse-view inversion fails (non-invertible), the proxy logs status/warnings and falls back to structural auto-detection.

### Barnyard (`GameProfile=Barnyard` or `Barnyard2006`)

Behavior:

- Intercepts game `SetTransform(VIEW/PROJECTION)` calls.
- Optionally round-trips through `GetTransform()` to capture final matrices.
- Caches VIEW/PROJECTION and re-emits at draw time from proxy-controlled timing.
- Detects WORLD from vertex shader constants using deterministic structural classification.

Profile toggles:

- `BarnyardForceWorldFromC0=1` forces `c0-c3` uploads as WORLD.
- `BarnyardUseGameSetTransformsForViewProjection=1` enables VIEW/PROJECTION capture from game transform calls.

---

## Experimental normals/TBN forwarding

When `EnableTBNForwarding=1`, the proxy inspects vertex input state from:

- `SetVertexDeclaration`
- `SetFVF`
- `SetStreamSource`
- `SetStreamSourceFreq`
- `SetIndices`

For supported indexed triangle-list draws with missing attributes, the proxy can:

1. decode positions (and UV0 when available),
2. generate deterministic normals (+ optional tangents/binormals),
3. append generated data via an extra stream,
4. bind a patched vertex declaration for that draw,
5. restore original declaration/stream state afterward.

Safety behavior:

- Unsupported primitive types, instancing, missing requirements, decode failures, and size-limit violations are safely skipped.
- On any failure path, the original draw is still forwarded unchanged.
- Current implementation is focused on indexed triangle lists; non-indexed and UP paths are diagnostics-only.

Detailed counters, skip reasons, and timings are exposed in the **Normals** overlay tab.

---

## Overlay and runtime controls

### Overlay tabs

- **Camera**: live World/View/Projection/MVP state and source metadata.
- **Constants**: captured constant registers + editing tools.
- **Normals**: TBN forwarding toggles, counters, skip reasons, timing stats.
- **Memory Scanner**: optional background scan and matrix assignment actions.
- **Logs**: in-overlay logging stream.

### Input compatibility hotkeys (frame-polled)

To support titles where ImGui input routing is unreliable, hotkeys are polled every frame:

- `HotkeyToggleMenuVK` (default `F10`)
- `HotkeyTogglePauseVK` (default `F9`)
- `HotkeyEmitMatricesVK` (default `F8`)
- `HotkeyResetMatrixOverridesVK` (default `F7`)

Resetting matrix overrides sets `View/Proj/WorldMatrixRegister=-1`; if `AutoDetectMatrices=1`, deterministic structural auto-detection resumes immediately.

### UI scaling

- `ImGuiScalePercent` in `camera_proxy.ini`
- runtime scale slider in overlay

Scaling applies to style metrics and fonts.

---

## Setup

1. Install RTX Remix runtime files in your game directory.
2. Rename Remix runtime DLL to `d3d9_remix.dll` (or set `RemixDllName`).
3. Copy this project’s built `d3d9.dll` into the game directory.
4. Copy `camera_proxy.ini` into the same directory.
5. Launch the game and toggle overlay with `F10` (configurable).

## Build

### Option A: Visual Studio Developer Command Prompt

```bat
build.bat
```

### Option B: Generic shell (script locates VS toolchain)

```bat
do_build.bat
```

Build output: 32-bit `d3d9.dll`.

---

## Key configuration options

See `camera_proxy.ini` for full comments. Frequently used:

- `UseRemixRuntime`
- `RemixDllName`
- `EmitFixedFunctionTransforms`
- `ViewMatrixRegister`
- `ProjMatrixRegister`
- `WorldMatrixRegister`
- `AutoDetectMatrices`
- `ProbeTransposedLayouts`
- `ProbeInverseView`
- `EnableTBNForwarding`
- `ImGuiScalePercent`
- `EnableLogging`
- `GameProfile`

---

## Notes

- This branch is intentionally iterative and compatibility-driven.
- Deterministic behavior is favored over heuristic guessing wherever possible.

## Credits

- mencelot (original DMC4 camera proxy basis)
- cobalticarus92 (camera-proxy project)
