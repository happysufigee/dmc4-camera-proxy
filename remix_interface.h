#pragma once

#include <cstdint>
#include <unordered_map>

#include "remix/remix_c.h"

using RemixLightHandle = uint64_t;

enum class RemixLightType {
    Directional = 0,
    Point,
    Spot,
    Ambient
};

struct RemixLightDesc {
    RemixLightType type = RemixLightType::Point;
    float position[3] = {};
    float direction[3] = {0.0f, -1.0f, 0.0f};
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 1.0f;
    float coneAngle = 45.0f;
};

class RemixInterface {
public:
    bool Initialize(const char* remixDllName);
    void BeginFrame();
    void EndFrame();

    RemixLightHandle CreateLight(const RemixLightDesc& desc);
    bool UpdateLight(RemixLightHandle handle, const RemixLightDesc& desc);
    bool DestroyLight(RemixLightHandle handle);
    bool DrawLight(RemixLightHandle handle);

    bool IsRuntimeReady() const { return m_runtimeReady; }
    const char* LastStatus() const { return m_lastStatus; }

private:
    void WriteStatus(const char* msg);
    bool BuildLightInfo(const RemixLightDesc& desc,
                        remixapi_LightInfo* outInfo,
                        remixapi_LightInfoSphereEXT* outSphere,
                        remixapi_LightInfoDistantEXT* outDistant) const;

    bool m_runtimeReady = false;
    RemixLightHandle m_mockHandleCounter = 1;
    char m_lastStatus[256] = "uninitialized";

    remixapi_Interface m_api = {};
    std::unordered_map<RemixLightHandle, remixapi_LightHandle> m_liveHandles;
};
