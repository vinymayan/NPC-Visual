#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include "ClibUtil/editorID.hpp"
#include "rapidjson/document.h"
#include "BSFaceGenBaseMorphExtraData.h"
#include <thread>
#include <chrono>

namespace FormUtil {
    const RE::TESFile* GetMasterFile(RE::TESForm* ref);
    std::string NormalizeFormID(RE::TESForm* form);
    RE::FormID FormIDFromString(const std::string& str);
}

struct InternalFormInfo {
    RE::FormID formID;
    std::string editorID;
    std::string name;
    std::string pluginName;
    std::string formType;
    std::string cachedDisplayName; // CACHE PARA UI RÁPIDA

    void UpdateDisplayName() {
        std::string base = !name.empty() ? name : (!editorID.empty() ? editorID : "Unknown");
        cachedDisplayName = std::format("{} [{:08X}]", base, formID);
    }
    // Helper for UI
    std::string GetDisplayName() const {
        if (!name.empty()) return name;
        if (!editorID.empty()) return editorID;
        return std::to_string(formID);
    }
};

struct FaceGenGeometrySource {
    std::string nifPath;
    std::string geometryName;
    RE::FormID originFormID = 0;
    std::string kind;
};

class Manager {
public:
    static Manager* GetSingleton() {
        static Manager singleton;
        return &singleton;
    }

    void RegisterAffectedNPC(RE::FormID baseID, const std::string& nifPath);
    void UnregisterAffectedNPC(RE::FormID baseID);
    bool IsNPCAffected(RE::FormID baseID, std::string& outNifPath);

    void PopulateAllLists();
    static std::string ToUTF8(std::string_view a_str);
    // Data Store: Map of "TypeName" -> List of InternalFormInfo
    // We use this to feed the UI
    const std::vector<InternalFormInfo>& GetList(const std::string& typeName);

    // Register callback for when population is done
    void RegisterReadyCallback(std::function<void()> callback);
    static void ApplyNPCCustomizationFromJSON(RE::TESNPC* a_npc, const rapidjson::Document& doc);
    static RE::NiPointer<RE::NiAVObject> LoadNifFromFile(const std::string& a_path);
    static void DeformFaceToMatchNif(RE::Actor* a_actor, const std::string& a_nifPath);
    static void ScheduleFaceDeform(RE::FormID actorID, const std::string& nifPath, int retries = 40);
    static RE::BGSHeadPart* ExtractHeadPartFromNif(const std::string& a_nifPath);
    void ClearFaceGenGeometryIndex();
    void IndexFaceGenNif(const std::string& nifPath, RE::FormID originFormID);
    bool FindIndexedFaceGenGeometry(const std::string& geometryName, FaceGenGeometrySource& outSource) const;
    std::size_t GetFaceGenGeometryIndexSize() const;
    std::size_t GetFaceGenGeometryDuplicateCount() const;

private:
    Manager() = default;

    template <typename T>
    void PopulateList(const std::string& a_typeName, std::function<bool(T*)> a_filter = nullptr);

    bool _isPopulated = false;
    std::map<std::string, std::vector<InternalFormInfo>> _dataStore;
    std::vector<std::function<void()>> _readyCallbacks;
    std::map<RE::FormID, std::string> _affectedNPCs;
    std::map<std::string, FaceGenGeometrySource> _faceGenGeometryIndex;
    std::size_t _faceGenGeometryDuplicates = 0;
};

