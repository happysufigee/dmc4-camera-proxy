#include "remix_lighting_manager.h"

#include <algorithm>
#include <cmath>
#include <fstream>

static float ClampSafe(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool RemixLightingManager::Initialize(const char* remixDllName) {
    return m_remix.Initialize(remixDllName);
}

void RemixLightingManager::BeginFrame() {
    m_remix.BeginFrame();
    m_ambientSubmittedThisFrame = false;
    for (auto& kv : m_activeLights) {
        kv.second.updatedThisFrame = false;
        kv.second.framesAlive++;
    }
}

void RemixLightingManager::EndFrame() {
    std::vector<uint64_t> stale;
    for (auto& kv : m_activeLights) {
        ManagedLight& l = kv.second;
        if (!l.updatedThisFrame) {
            l.framesSinceUpdate++;
            if (l.framesSinceUpdate > static_cast<uint32_t>((std::max)(0, m_settings.graceThreshold))) {
                m_remix.DestroyLight(l.handle);
                stale.push_back(kv.first);
            }
        } else {
            l.framesSinceUpdate = 0;
        }
    }
    for (uint64_t key : stale) m_activeLights.erase(key);
    m_remix.EndFrame();
}

void RemixLightingManager::Normalize(float v[3]) const {
    float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-6f) {
        v[0] /= len; v[1] /= len; v[2] /= len;
    }
}

bool RemixLightingManager::IsFinite3(const float v[3]) const {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float RemixLightingManager::ComputeIntensity(const float color[3]) const {
    float i = std::sqrt(color[0]*color[0] + color[1]*color[1] + color[2]*color[2]);
    return ClampSafe(i * m_settings.intensityMultiplier, 0.0f, 50000.0f);
}

bool RemixLightingManager::InvertMatrix(const D3DMATRIX& m, D3DMATRIX* out) const {
    if (!out) return false;
    // affine inverse only
    float det =
        m._11*(m._22*m._33 - m._23*m._32) -
        m._12*(m._21*m._33 - m._23*m._31) +
        m._13*(m._21*m._32 - m._22*m._31);
    if (std::fabs(det) < 1e-8f) return false;
    float invDet = 1.0f / det;
    out->_11 =  (m._22*m._33 - m._23*m._32) * invDet;
    out->_12 = -(m._12*m._33 - m._13*m._32) * invDet;
    out->_13 =  (m._12*m._23 - m._13*m._22) * invDet;
    out->_21 = -(m._21*m._33 - m._23*m._31) * invDet;
    out->_22 =  (m._11*m._33 - m._13*m._31) * invDet;
    out->_23 = -(m._11*m._23 - m._13*m._21) * invDet;
    out->_31 =  (m._21*m._32 - m._22*m._31) * invDet;
    out->_32 = -(m._11*m._32 - m._12*m._31) * invDet;
    out->_33 =  (m._11*m._22 - m._12*m._21) * invDet;
    out->_14 = out->_24 = out->_34 = 0.0f; out->_44 = 1.0f;
    out->_41 = -(m._41*out->_11 + m._42*out->_21 + m._43*out->_31);
    out->_42 = -(m._41*out->_12 + m._42*out->_22 + m._43*out->_32);
    out->_43 = -(m._41*out->_13 + m._42*out->_23 + m._43*out->_33);
    return true;
}

void RemixLightingManager::TransformPosition(const D3DMATRIX& m, const float in[3], float out[3]) const {
    out[0] = in[0]*m._11 + in[1]*m._21 + in[2]*m._31 + m._41;
    out[1] = in[0]*m._12 + in[1]*m._22 + in[2]*m._32 + m._42;
    out[2] = in[0]*m._13 + in[1]*m._23 + in[2]*m._33 + m._43;
}

void RemixLightingManager::TransformDirection(const D3DMATRIX& m, const float in[3], float out[3]) const {
    out[0] = in[0]*m._11 + in[1]*m._21 + in[2]*m._31;
    out[1] = in[0]*m._12 + in[1]*m._22 + in[2]*m._32;
    out[2] = in[0]*m._13 + in[1]*m._23 + in[2]*m._33;
}

uint64_t RemixLightingManager::ComputeSignature(const ManagedLight& l) const {
    auto q = [](float v) { return static_cast<int64_t>(std::llround(v * 1000.0f)); };
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) { h ^= x; h *= 1099511628211ull; };
    mix(static_cast<uint64_t>(l.type));
    mix(static_cast<uint64_t>(q(l.position[0]))); mix(static_cast<uint64_t>(q(l.position[1]))); mix(static_cast<uint64_t>(q(l.position[2])));
    mix(static_cast<uint64_t>(q(l.direction[0]))); mix(static_cast<uint64_t>(q(l.direction[1]))); mix(static_cast<uint64_t>(q(l.direction[2])));
    mix(static_cast<uint64_t>(q(l.color[0]))); mix(static_cast<uint64_t>(q(l.color[1]))); mix(static_cast<uint64_t>(q(l.color[2])));
    mix(static_cast<uint64_t>(q(l.intensity)));
    mix(static_cast<uint64_t>(q(l.coneAngle)));
    return h;
}

void RemixLightingManager::FillRawRegisters(ManagedLight& light, int base, const float constants[][4]) {
    light.rawRegisterBase = base;
    light.rawRegisterCount = 4;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) light.rawRegisters[i][j] = constants[base + i][j];
    }
}

void RemixLightingManager::SubmitManagedLight(ManagedLight& candidate) {
    if (!m_settings.enabled || m_settings.freezeLightUpdates) return;
    if (candidate.type == RemixLightType::Directional && !m_settings.enableDirectional) return;
    if (candidate.type == RemixLightType::Point && !m_settings.enablePoint) return;
    if (candidate.type == RemixLightType::Spot && !m_settings.enableSpot) return;
    if (candidate.type == RemixLightType::Ambient && !m_settings.enableAmbient) return;

    candidate.signatureHash = ComputeSignature(candidate);

    if (!m_settings.disableDeduplication) {
        auto it = m_activeLights.find(candidate.signatureHash);
        if (it != m_activeLights.end()) {
            it->second.updatedThisFrame = true;
            it->second.color[0] = candidate.color[0]; it->second.color[1] = candidate.color[1]; it->second.color[2] = candidate.color[2];
            it->second.intensity = candidate.intensity;
            it->second.range = candidate.range;
            it->second.coneAngle = candidate.coneAngle;
            it->second.position[0] = candidate.position[0]; it->second.position[1] = candidate.position[1]; it->second.position[2] = candidate.position[2];
            it->second.direction[0] = candidate.direction[0]; it->second.direction[1] = candidate.direction[1]; it->second.direction[2] = candidate.direction[2];
            RemixLightDesc desc = {};
            desc.type = it->second.type;
            for (int i = 0; i < 3; ++i) {
                desc.position[i] = it->second.position[i];
                desc.direction[i] = it->second.direction[i];
                desc.color[i] = it->second.color[i];
            }
            desc.intensity = it->second.intensity;
            desc.range = it->second.range;
            desc.coneAngle = it->second.coneAngle;
            m_remix.UpdateLight(it->second.handle, desc);
            m_remix.DrawLight(it->second.handle);
            return;
        }
    }

    RemixLightDesc desc = {};
    desc.type = candidate.type;
    for (int i = 0; i < 3; ++i) {
        desc.position[i] = candidate.position[i];
        desc.direction[i] = candidate.direction[i];
        desc.color[i] = candidate.color[i];
    }
    desc.intensity = candidate.intensity;
    desc.range = candidate.range;
    desc.coneAngle = candidate.coneAngle;
    candidate.handle = m_remix.CreateLight(desc);
    candidate.updatedThisFrame = true;
    if (candidate.handle != 0) {
        m_activeLights[candidate.signatureHash] = candidate;
        m_remix.DrawLight(candidate.handle);
    }
}

void RemixLightingManager::ProcessDrawCall(const ShaderLightingMetadata& meta,
                                           const float constants[][4],
                                           const D3DMATRIX& world,
                                           const D3DMATRIX& view,
                                           bool hasWorld,
                                           bool hasView) {
    if (!meta.isFFPLighting || !m_settings.enabled) return;

    int base = meta.lightingConstantBase >= 0 ? meta.lightingConstantBase : 0;
    int lightCount = 1;
    if (meta.constantUsage && meta.constantCount > 0) {
        int run = 0;
        for (int i = base; i < meta.constantCount; ++i) {
            if (meta.constantUsage[i]) run++; else if (run > 0) break;
        }
        lightCount = (std::max)(1, run / 4);
        lightCount = (std::min)(lightCount, 8);
    }

    D3DMATRIX toWorld = {};
    bool canTransform = true;
    if (meta.lightSpace == LightingSpace::View) {
        if (!hasView || !InvertMatrix(view, &toWorld)) canTransform = false;
    } else if (meta.lightSpace == LightingSpace::Object) {
        if (!hasWorld) canTransform = false;
        else toWorld = world;
    }

    for (int i = 0; i < lightCount; ++i) {
        ManagedLight l = {};
        int reg = base + i * 4;
        float dir[3] = { constants[reg][0], constants[reg][1], constants[reg][2] };
        float color[3] = { constants[reg + 1][0], constants[reg + 1][1], constants[reg + 1][2] };
        float pos[3] = { constants[reg + 2][0], constants[reg + 2][1], constants[reg + 2][2] };
        float atten = constants[reg + 3][0];
        float cone = constants[reg + 3][1];

        bool hasDirection = std::fabs(dir[0]) + std::fabs(dir[1]) + std::fabs(dir[2]) > 0.0001f;
        bool hasPosition = std::fabs(pos[0]) + std::fabs(pos[1]) + std::fabs(pos[2]) > 0.0001f;
        bool hasAttenuation = std::fabs(atten) > 0.0001f;

        if (!hasDirection && !hasPosition) {
            if (m_ambientSubmittedThisFrame) continue;
            l.type = RemixLightType::Ambient;
            m_ambientSubmittedThisFrame = true;
        } else if (hasDirection && hasPosition && cone > 0.001f) {
            l.type = RemixLightType::Spot;
        } else if (hasPosition && hasAttenuation) {
            l.type = RemixLightType::Point;
        } else {
            l.type = RemixLightType::Directional;
        }

        l.color[0] = ClampSafe(color[0], 0.0f, 1000.0f);
        l.color[1] = ClampSafe(color[1], 0.0f, 1000.0f);
        l.color[2] = ClampSafe(color[2], 0.0f, 1000.0f);
        l.intensity = ComputeIntensity(l.color);
        l.range = ClampSafe(hasAttenuation ? (1.0f / (std::max)(0.001f, std::fabs(atten))) : 20.0f, 0.01f, 100000.0f);
        const float coneRad = ClampSafe(cone > 0.001f ? cone : 0.785398f, 0.01f, 3.12f);
        l.coneAngle = coneRad * 57.2957795f;

        for (int c = 0; c < 3; ++c) { l.direction[c] = dir[c]; l.position[c] = pos[c]; }
        Normalize(l.direction);

        if (canTransform) {
            if (meta.lightSpace != LightingSpace::World) {
                TransformPosition(toWorld, l.position, l.position);
                TransformDirection(toWorld, l.direction, l.direction);
                Normalize(l.direction);
            }
        }

        if (!IsFinite3(l.color) || !IsFinite3(l.position) || !IsFinite3(l.direction)) continue;
        FillRawRegisters(l, reg, constants);
        SubmitManagedLight(l);
    }
}

void RemixLightingManager::DestroyAllLights() {
    for (auto& kv : m_activeLights) {
        m_remix.DestroyLight(kv.second.handle);
    }
    m_activeLights.clear();
}

bool RemixLightingManager::DumpLightsToJson(const char* path) const {
    if (!path || !path[0]) return false;
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) return false;
    f << "{\n  \"activeLights\": [\n";
    bool first = true;
    for (const auto& kv : m_activeLights) {
        const ManagedLight& l = kv.second;
        if (!first) f << ",\n";
        first = false;
        f << "    {\"handle\": " << l.handle
          << ", \"signature\": " << l.signatureHash
          << ", \"type\": " << static_cast<int>(l.type)
          << ", \"intensity\": " << l.intensity
          << ", \"framesAlive\": " << l.framesAlive << "}";
    }
    f << "\n  ]\n}\n";
    return true;
}
