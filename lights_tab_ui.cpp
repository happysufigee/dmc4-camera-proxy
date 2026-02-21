#include "lights_tab_ui.h"

#include "remix_lighting_manager.h"
#include "imgui/imgui.h"

#include <vector>
#include <cstdio>

static const char* LightTypeName(RemixLightType t) {
    switch (t) {
    case RemixLightType::Directional: return "Directional";
    case RemixLightType::Point: return "Point";
    case RemixLightType::Spot: return "Spot";
    case RemixLightType::Ambient: return "Ambient";
    default: return "Unknown";
    }
}

void DrawRemixLightsTab(RemixLightingManager& manager) {
    static uint64_t selectedSignature = 0;
    static char dumpPath[260] = "lights_dump.json";

    auto& settings = manager.Settings();
    const auto& active = manager.ActiveLights();
    int dir = 0, point = 0, spot = 0, ambient = 0;
    for (const auto& kv : active) {
        switch (kv.second.type) {
        case RemixLightType::Directional: dir++; break;
        case RemixLightType::Point: point++; break;
        case RemixLightType::Spot: spot++; break;
        case RemixLightType::Ambient: ambient++; break;
        }
    }

    ImGui::Columns(3, "RemixLightsCols", true);

    ImGui::Text("Active Lights");
    ImGui::Separator();
    ImGui::Text("Total: %d", static_cast<int>(active.size()));
    ImGui::Text("Directional: %d", dir);
    ImGui::Text("Point: %d", point);
    ImGui::Text("Spot: %d", spot);
    ImGui::Text("Ambient: %d", ambient);
    ImGui::BeginChild("LightsList", ImVec2(0, 320), true);
    for (const auto& kv : active) {
        const ManagedLight& l = kv.second;
        char label[256];
        snprintf(label, sizeof(label), "H:%llu %s I:%.2f###sig_%llu",
                 static_cast<unsigned long long>(l.handle), LightTypeName(l.type), l.intensity,
                 static_cast<unsigned long long>(l.signatureHash));
        if (ImGui::Selectable(label, selectedSignature == l.signatureHash)) {
            selectedSignature = l.signatureHash;
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();
    ImGui::Text("Light Details");
    ImGui::Separator();
    auto it = active.find(selectedSignature);
    if (it != active.end()) {
        const ManagedLight& l = it->second;
        ImGui::Text("Handle: %llu", static_cast<unsigned long long>(l.handle));
        ImGui::Text("Type: %s", LightTypeName(l.type));
        ImGui::Text("Color: %.3f %.3f %.3f", l.color[0], l.color[1], l.color[2]);
        ImGui::Text("World direction: %.3f %.3f %.3f", l.direction[0], l.direction[1], l.direction[2]);
        ImGui::Text("World position: %.3f %.3f %.3f", l.position[0], l.position[1], l.position[2]);
        ImGui::Text("Intensity: %.3f", l.intensity);
        ImGui::Text("Cone angle: %.3f", l.coneAngle);
        ImGui::Text("Range: %.3f", l.range);
        ImGui::Text("Signature hash: %llu", static_cast<unsigned long long>(l.signatureHash));
        ImGui::Text("Frames alive: %u", l.framesAlive);
        ImGui::Text("framesSinceUpdate: %u", l.framesSinceUpdate);
        ImGui::Text("Updated this frame: %s", l.updatedThisFrame ? "Yes" : "No");
        ImGui::Separator();
        ImGui::Text("Raw constants c%d-c%d", l.rawRegisterBase, l.rawRegisterBase + l.rawRegisterCount - 1);
        for (int i = 0; i < l.rawRegisterCount; ++i) {
            ImGui::Text("c%d: [%.3f %.3f %.3f %.3f]", l.rawRegisterBase + i,
                        l.rawRegisters[i][0], l.rawRegisters[i][1], l.rawRegisters[i][2], l.rawRegisters[i][3]);
        }
    } else {
        ImGui::TextDisabled("Select a light to inspect details.");
    }

    ImGui::NextColumn();
    ImGui::Text("Controls");
    ImGui::Separator();
    ImGui::Checkbox("Enable Remix Lighting Forwarding", &settings.enabled);
    ImGui::SliderFloat("Intensity Multiplier", &settings.intensityMultiplier, 0.0f, 10.0f, "%.2f");
    ImGui::SliderInt("Grace Period", &settings.graceThreshold, 0, 10);
    ImGui::Checkbox("Directional", &settings.enableDirectional);
    ImGui::Checkbox("Point", &settings.enablePoint);
    ImGui::Checkbox("Spot", &settings.enableSpot);
    ImGui::Checkbox("Ambient", &settings.enableAmbient);
    if (ImGui::Button("Force Destroy All Lights")) {
        manager.DestroyAllLights();
    }
    ImGui::Checkbox("Debug: Disable Deduplication", &settings.disableDeduplication);
    ImGui::Checkbox("Debug: Freeze Light Updates", &settings.freezeLightUpdates);
    ImGui::InputText("Dump Path", dumpPath, sizeof(dumpPath));
    if (ImGui::Button("Dump Lights To JSON")) {
        manager.DumpLightsToJson(dumpPath);
    }
    ImGui::TextWrapped("Runtime: %s", manager.RuntimeStatus());

    ImGui::Columns(1);
}
