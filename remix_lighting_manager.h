#pragma once

#include <d3d9.h>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>

#include "remix_interface.h"

enum class LightingSpace {
    World = 0,
    View,
    Object
};

struct ShaderLightingMetadata {
    bool isFFPLighting = false;
    int lightDirectionRegister = -1;
    int lightColorRegister = -1;
    int materialColorRegister = -1;
    int attenuationRegister = -1;
    int positionRegister = -1;
    int coneAngleRegister = -1;
    int lightingConstantBase = -1;
    LightingSpace lightSpace = LightingSpace::World;
    const bool* constantUsage = nullptr;
    int constantCount = 0;
};

struct ManagedLight {
    uint64_t signatureHash = 0;
    RemixLightType type = RemixLightType::Point;
    float direction[3] = {};
    float position[3] = {};
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 1.0f;
    float coneAngle = 45.0f;
    RemixLightHandle handle = 0;
    uint32_t framesAlive = 0;
    uint32_t framesSinceUpdate = 0;
    bool updatedThisFrame = false;
    int rawRegisterBase = -1;
    int rawRegisterCount = 0;
    float rawRegisters[4][4] = {};
};

struct RemixLightingSettings {
    bool enabled = true;
    float intensityMultiplier = 1.0f;
    int graceThreshold = 2;
    bool enableDirectional = true;
    bool enablePoint = true;
    bool enableSpot = true;
    bool enableAmbient = true;
    bool disableDeduplication = false;
    bool freezeLightUpdates = false;
};

class RemixLightingManager {
public:
    bool Initialize(const char* remixDllName);
    void BeginFrame();
    void EndFrame();
    void ProcessDrawCall(const ShaderLightingMetadata& meta,
                         const float constants[][4],
                         const D3DMATRIX& world,
                         const D3DMATRIX& view,
                         bool hasWorld,
                         bool hasView);

    void DestroyAllLights();
    bool DumpLightsToJson(const char* path) const;

    RemixLightingSettings& Settings() { return m_settings; }
    const RemixLightingSettings& Settings() const { return m_settings; }
    const std::unordered_map<uint64_t, ManagedLight>& ActiveLights() const { return m_activeLights; }
    const char* RuntimeStatus() const { return m_remix.LastStatus(); }

private:
    bool InvertMatrix(const D3DMATRIX& m, D3DMATRIX* out) const;
    void TransformPosition(const D3DMATRIX& m, const float in[3], float out[3]) const;
    void TransformDirection(const D3DMATRIX& m, const float in[3], float out[3]) const;
    void Normalize(float v[3]) const;
    bool IsFinite3(const float v[3]) const;
    float ComputeIntensity(const float color[3]) const;
    uint64_t ComputeSignature(const ManagedLight& l) const;
    void SubmitManagedLight(ManagedLight& candidate);
    void FillRawRegisters(ManagedLight& light, int base, const float constants[][4]);

    RemixInterface m_remix;
    RemixLightingSettings m_settings;
    std::unordered_map<uint64_t, ManagedLight> m_activeLights;
    bool m_ambientSubmittedThisFrame = false;
};
