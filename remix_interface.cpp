#include "remix_interface.h"

#include "remixapi/bridge_remix_api.h"

#include <cmath>
#include <cstdio>

void RemixInterface::WriteStatus(const char* msg) {
    if (!msg) return;
    std::snprintf(m_lastStatus, sizeof(m_lastStatus), "%s", msg);
}

bool RemixInterface::BuildLightInfo(const RemixLightDesc& desc,
                                    remixapi_LightInfo* outInfo,
                                    remixapi_LightInfoSphereEXT* outSphere,
                                    remixapi_LightInfoDistantEXT* outDistant) const {
    if (!outInfo || !outSphere || !outDistant) return false;

    *outInfo = {};
    *outSphere = {};
    *outDistant = {};

    outInfo->sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    outInfo->pNext = nullptr;
    outInfo->hash = 0;
    outInfo->radiance = {
        desc.color[0] * desc.intensity,
        desc.color[1] * desc.intensity,
        desc.color[2] * desc.intensity
    };

    if (desc.type == RemixLightType::Directional) {
        outDistant->sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
        outDistant->direction = { desc.direction[0], desc.direction[1], desc.direction[2] };
        outDistant->angularDiameterDegrees = 0.5f;
        outDistant->volumetricRadianceScale = 1.0f;
        outInfo->pNext = outDistant;
        return true;
    }

    outSphere->sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    outSphere->position = { desc.position[0], desc.position[1], desc.position[2] };
    outSphere->radius = (desc.type == RemixLightType::Ambient) ? 100000.0f : (desc.range > 0.01f ? desc.range : 1.0f);
    outSphere->volumetricRadianceScale = 1.0f;
    outSphere->shaping_hasvalue = 0;

    if (desc.type == RemixLightType::Spot) {
        outSphere->shaping_hasvalue = 1;
        outSphere->shaping_value.direction = { desc.direction[0], desc.direction[1], desc.direction[2] };
        outSphere->shaping_value.coneAngleDegrees = desc.coneAngle;
        outSphere->shaping_value.coneSoftness = 0.0f;
        outSphere->shaping_value.focusExponent = 1.0f;
    }

    outInfo->pNext = outSphere;
    return true;
}

bool RemixInterface::Initialize(const char* /*remixDllName*/) {
    remixapi_Interface api = {};
    const remixapi_ErrorCode status = remixapi::bridge_initRemixApi(&api);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
        WriteStatus("Remix bridge init failed; lighting forwarding in safe fallback mode.");
        m_runtimeReady = false;
        return true;
    }

    m_api = api;
    m_runtimeReady = (m_api.CreateLight && m_api.DestroyLight && m_api.DrawLightInstance);
    WriteStatus(m_runtimeReady ? "Remix API initialized via bridge." : "Remix API incomplete; fallback mode.");
    return true;
}

void RemixInterface::BeginFrame() {
}

void RemixInterface::EndFrame() {
}

RemixLightHandle RemixInterface::CreateLight(const RemixLightDesc& desc) {
    RemixLightHandle logical = m_mockHandleCounter++;

    if (!m_runtimeReady) {
        return logical;
    }

    remixapi_LightInfo info = {};
    remixapi_LightInfoSphereEXT sphere = {};
    remixapi_LightInfoDistantEXT distant = {};
    if (!BuildLightInfo(desc, &info, &sphere, &distant)) {
        return 0;
    }

    remixapi_LightHandle nativeHandle = nullptr;
    if (m_api.CreateLight(&info, &nativeHandle) != REMIXAPI_ERROR_CODE_SUCCESS || !nativeHandle) {
        return 0;
    }

    m_liveHandles[logical] = nativeHandle;
    return logical;
}

bool RemixInterface::UpdateLight(RemixLightHandle handle, const RemixLightDesc& desc) {
    if (handle == 0) return false;
    if (!m_runtimeReady) return true;

    auto it = m_liveHandles.find(handle);
    if (it == m_liveHandles.end()) return false;

    m_api.DestroyLight(it->second);
    remixapi_LightInfo info = {};
    remixapi_LightInfoSphereEXT sphere = {};
    remixapi_LightInfoDistantEXT distant = {};
    if (!BuildLightInfo(desc, &info, &sphere, &distant)) return false;

    remixapi_LightHandle newHandle = nullptr;
    if (m_api.CreateLight(&info, &newHandle) != REMIXAPI_ERROR_CODE_SUCCESS || !newHandle) {
        m_liveHandles.erase(it);
        return false;
    }

    it->second = newHandle;
    return true;
}

bool RemixInterface::DestroyLight(RemixLightHandle handle) {
    if (handle == 0) return false;
    if (!m_runtimeReady) return true;

    auto it = m_liveHandles.find(handle);
    if (it == m_liveHandles.end()) return true;

    const bool ok = (m_api.DestroyLight(it->second) == REMIXAPI_ERROR_CODE_SUCCESS);
    m_liveHandles.erase(it);
    return ok;
}

bool RemixInterface::DrawLight(RemixLightHandle handle) {
    if (handle == 0) return false;
    if (!m_runtimeReady) return true;

    auto it = m_liveHandles.find(handle);
    if (it == m_liveHandles.end()) return false;
    return m_api.DrawLightInstance(it->second) == REMIXAPI_ERROR_CODE_SUCCESS;
}
