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
    std::string GetEditorID(RE::TESForm* form);
    std::string NormalizeFormID(RE::TESForm* form);
    rapidjson::Value MakeFormRef(RE::TESForm* form, rapidjson::Document::AllocatorType& allocator);
    std::string FormRefDebugString(const rapidjson::Value& value);
    RE::FormID FormIDFromString(const std::string& str);

    template <typename T>
    T* ResolveForm(const rapidjson::Value& value)
    {
        if (value.IsObject()) {
            if (value.HasMember("editorID") && value["editorID"].IsString() && value["editorID"].GetStringLength() > 0) {
                if (auto form = RE::TESForm::LookupByEditorID(value["editorID"].GetString())) {
                    if (auto typed = form->As<T>()) {
                        return typed;
                    }
                }
            }

            const char* fallbackKeys[] = { "formID", "form", "id" };
            for (const auto* key : fallbackKeys) {
                if (value.HasMember(key) && value[key].IsString()) {
                    if (auto form = RE::TESForm::LookupByID<T>(FormIDFromString(value[key].GetString()))) {
                        return form;
                    }
                }
            }
            return nullptr;
        }

        if (value.IsString()) {
            const std::string raw = value.GetString();
            if (!raw.empty()) {
                if (auto form = RE::TESForm::LookupByEditorID(raw)) {
                    if (auto typed = form->As<T>()) {
                        return typed;
                    }
                }
                if (auto form = RE::TESForm::LookupByID<T>(FormIDFromString(raw))) {
                    return form;
                }
            }
            return nullptr;
        }

        if (value.IsUint()) {
            return RE::TESForm::LookupByID<T>(value.GetUint());
        }

        return nullptr;
    }
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
    void ClearAffectedNPCs();
    bool IsNPCAffected(RE::FormID baseID, std::string& outNifPath);

    void PopulateAllLists(bool forceRefresh = false);
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
    static void DumpFaceDiagnostics(RE::Actor* a_actor, RE::TESNPC* a_npc, const std::string& a_context);
    void ClearFaceGenGeometryIndex();
    void IndexFaceGenNif(const std::string& nifPath, RE::FormID originFormID);
    bool FindIndexedFaceGenGeometry(const std::string& geometryName, FaceGenGeometrySource& outSource) const;
    std::size_t GetFaceGenGeometryIndexSize() const;
    std::size_t GetFaceGenGeometryDuplicateCount() const;
    bool _isPopulated = false;
private:
    Manager() = default;

    template <typename T>
    void PopulateList(const std::string& a_typeName, std::function<bool(T*)> a_filter = nullptr);

    
    std::map<std::string, std::vector<InternalFormInfo>> _dataStore;
    std::vector<std::function<void()>> _readyCallbacks;
    std::map<RE::FormID, std::string> _affectedNPCs;
    std::map<std::string, FaceGenGeometrySource> _faceGenGeometryIndex;
    std::size_t _faceGenGeometryDuplicates = 0;
};

