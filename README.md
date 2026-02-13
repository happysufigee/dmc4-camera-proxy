# camera-proxy (experimental branch)

Experimental Direct3D9 proxy DLL for RTX Remix.

This branch is focused on **deterministic matrix extraction** for arbitrary DX9 engines instead of DMC4-only assumptions. The proxy wraps `IDirect3D9` / `IDirect3DDevice9`, observes vertex shader constant uploads, caches current transform state, and forwards fixed-function transforms immediately before draw calls.

## Why this exists

Many programmable DX9 games never call fixed-function `SetTransform()` while RTX Remix depends on those transform states for scene reconstruction. A naive approach that forwards transforms during `SetVertexShaderConstantF()` is often out-of-sync with the actual draw that consumes those constants.

This proxy solves that by:

- tracking matrix state from constant uploads,
- and emitting `SetTransform(D3DTS_WORLD/VIEW/PROJECTION)` **per draw call** (`DrawPrimitive`, `DrawIndexedPrimitive`, `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`).

## Current behavior (experimental)

### 1) Matrix state caching

The proxy maintains three cached matrices:

- current World
- current View
- current Projection

State is updated when constant uploads match structural rules, but forwarding is deferred until draw time.

### 2) Deterministic structural classification

Constant upload windows are interpreted as potential `4x4` and `4x3` matrices.

- **Projection**: strict perspective structure + FOV validation (`MinFOV`, `MaxFOV`).
- **View**: strict orthonormal affine camera-style matrix.
- **World**: affine matrix that is not strict View.
- **Combined perspective fallback**: if perspective-like but not strict Projection, World/View are set to identity and the matrix is forwarded as Projection.

No probabilistic ranking or MVP decomposition is performed in this path.

### 3) Transpose probing

If enabled (`ProbeTransposedLayouts=1`), a failing candidate is transposed once and re-checked deterministically.

### 4) Register overrides

`ViewMatrixRegister`, `ProjMatrixRegister`, and `WorldMatrixRegister`:

- `>= 0` means only that base register is accepted for that type.
- `-1` means classify from all observed constant ranges.

### 5) Draw-time emission

When `EmitFixedFunctionTransforms=1`, cached WORLD/VIEW/PROJECTION are emitted before each intercepted draw call.

If a matrix type is unknown, identity is used as fallback to keep fixed-function state valid.

### 6) ImGui overlay scaling

This branch includes configurable UI scaling:

- `ImGuiScalePercent` in `camera_proxy.ini`, and
- runtime slider in the overlay.

Scaling is applied to both style metrics and fonts.

---

## Setup

1. Install RTX Remix runtime files in your game directory.
2. Rename Remix's runtime DLL to `d3d9_remix.dll` (or update `RemixDllName`).
3. Copy this project’s built `d3d9.dll` into the game directory.
4. Copy `camera_proxy.ini` into the same directory.
5. Launch the game. Toggle the overlay with **Alt+M**.

## Build

### Option A (VS Developer Command Prompt)

```bat
build.bat
```

### Option B (generic shell; script locates VS toolchain)

```bat
do_build.bat
```

Output is a 32-bit `d3d9.dll`.

## Key config options

See `camera_proxy.ini` for full comments. Most important keys:

- `UseRemixRuntime`
- `RemixDllName`
- `EmitFixedFunctionTransforms`
- `ViewMatrixRegister`
- `ProjMatrixRegister`
- `WorldMatrixRegister`
- `AutoDetectMatrices`
- `ProbeTransposedLayouts`
- `ImGuiScalePercent`
- `EnableLogging`

## Overlay overview

- **Camera**: live World/View/Projection/MVP display and source metadata.
- **Constants**: captured registers and shader-constant editing tools.
- **Heuristics / diagnostics tabs**: legacy/experimental controls retained for compatibility work.
- **Logs**: in-overlay log stream.

## Notes

- This is an **experimental branch** intended for iterative engine compatibility testing.
- Some legacy keys and panels are still present for compatibility/debugging even though deterministic matrix classification is the primary path.

## Credits

- mencelot (original DMC4 camera proxy basis)
- cobalticarus92 (camera-proxy project)
