#include "Manager.h"
#include "DelayedDispatcher.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace {
    std::string LowerCopy(std::string_view a_value)
    {
        std::string lower(a_value);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return lower;
    }

    bool HeadPartHasUsableModel(RE::BGSHeadPart* a_headPart)
    {
        const auto* model = a_headPart ? a_headPart->GetModel() : nullptr;
        return model && model[0] != '\0';
    }

    bool IsHeadPartAllowedForRaceSex(RE::BGSHeadPart* a_headPart, RE::TESRace* a_race, bool a_isFemale)
    {
        if (!a_headPart) {
            return false;
        }

        const bool hpFemale = a_headPart->flags.all(RE::BGSHeadPart::Flag::kFemale);
        const bool hpMale = a_headPart->flags.all(RE::BGSHeadPart::Flag::kMale);
        if (a_isFemale && hpMale && !hpFemale) {
            return false;
        }
        if (!a_isFemale && hpFemale && !hpMale) {
            return false;
        }

        if (!a_race || !a_headPart->validRaces) {
            return true;
        }
        if (a_headPart->validRaces->HasForm(a_race)) {
            return true;
        }
        if (a_race->armorParentRace && a_headPart->validRaces->HasForm(a_race->armorParentRace)) {
            return true;
        }

        return a_headPart->validRaces->forms.empty();
    }

    bool IsSafeFaceHeadPart(RE::BGSHeadPart* a_headPart, RE::TESRace* a_race = nullptr, bool a_isFemale = true)
    {
        return a_headPart &&
               a_headPart->type == RE::BGSHeadPart::HeadPartType::kFace &&
               HeadPartHasUsableModel(a_headPart) &&
               (!a_race || IsHeadPartAllowedForRaceSex(a_headPart, a_race, a_isFemale));
    }

    RE::BGSHeadPart* FindCurrentSafeFaceHeadPart(RE::TESNPC* a_npc)
    {
        if (!a_npc || !a_npc->headParts) {
            return nullptr;
        }

        const bool isFemale = a_npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kFemale);
        for (int i = 0; i < a_npc->numHeadParts; ++i) {
            auto* hp = a_npc->headParts[i];
            if (IsSafeFaceHeadPart(hp, a_npc->race, isFemale)) {
                return hp;
            }
        }

        return nullptr;
    }
}

namespace FormUtil {
    const RE::TESFile* GetMasterFile(RE::TESForm* ref) {
        if (!ref) return nullptr;

        uint32_t formID = ref->GetFormID();
        uint8_t modIndex = static_cast<uint8_t>(formID >> 24);

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;

        if (modIndex == 0xFE) {
            uint16_t eslIndex = (formID >> 12) & 0xFFF;
            return dataHandler->LookupLoadedLightModByIndex(eslIndex);
        }

        return dataHandler->LookupLoadedModByIndex(modIndex);
    }

    std::string NormalizeFormID(RE::TESForm* form) {
        if (!form) return {};

        RE::FormID formID = form->GetFormID();
        uint8_t modIndex = (formID >> 24) & 0xFF;

        if (modIndex == 0xFF) {
            return std::format("{:X}", formID);
        }

        auto file = GetMasterFile(form);
        if (!file) return std::format("{:X}", formID);

        uint32_t localID = formID & 0x00FFFFFF;

        if (modIndex == 0xFE) {
            uint32_t eslID = localID & 0xFFF;
            return std::format("{}|{:X}", file->GetFilename(), eslID);
        }

        return std::format("{}|{:X}", file->GetFilename(), localID);
    }

    rapidjson::Value MakeFormRef(RE::TESForm* form, rapidjson::Document::AllocatorType& allocator)
    {
        rapidjson::Value ref(rapidjson::kObjectType);
        auto editorID = clib_util::editorID::get_editorID(form);
        auto formID = NormalizeFormID(form);
        ref.AddMember("editorID", rapidjson::Value(editorID.c_str(), allocator), allocator);
        ref.AddMember("formID", rapidjson::Value(formID.c_str(), allocator), allocator);
        return ref;
    }

    std::string FormRefDebugString(const rapidjson::Value& value)
    {
        if (value.IsString()) {
            return value.GetString();
        }
        if (value.IsObject()) {
            std::string editorID;
            std::string formID;
            if (value.HasMember("editorID") && value["editorID"].IsString()) {
                editorID = value["editorID"].GetString();
            }
            if (value.HasMember("formID") && value["formID"].IsString()) {
                formID = value["formID"].GetString();
            }
            if (!editorID.empty() && !formID.empty()) {
                return std::format("{} ({})", editorID, formID);
            }
            if (!editorID.empty()) {
                return editorID;
            }
            if (!formID.empty()) {
                return formID;
            }
        }
        if (value.IsUint()) {
            return std::format("{:08X}", value.GetUint());
        }
        return "<invalid form ref>";
    }


    RE::FormID FormIDFromString(const std::string& str) {
        try {
            auto pos = str.find('|');
            if (pos != std::string::npos) {
                std::string plugin = str.substr(0, pos);
                std::string idStr = str.substr(pos + 1);
                RE::FormID localId = std::stoul(idStr, nullptr, 16);
                auto dataHandler = RE::TESDataHandler::GetSingleton();
                return dataHandler ? dataHandler->LookupFormID(localId, plugin) : 0;
            }
            return str.empty() ? 0 : std::stoul(str, nullptr, 16);
        } catch (...) {
            return 0;
        }
    }
}

void Manager::ApplyNPCCustomizationFromJSON(RE::TESNPC* npc, const rapidjson::Document& doc) {
    if (!npc) {
        logger::error("[ApplyJSON] Falha: Ponteiro de NPC nulo recebido.");
        return;
    }
    if (!doc.IsObject()) {
        logger::error("[ApplyJSON] Falha: JSON invalido para NPC {:08X}; documento nao eh objeto.", npc->GetFormID());
        return;
    }

    std::string npcName = npc->GetFullName() ? npc->GetFullName() : "Unnamed";
    const auto headPartJsonCount = doc.HasMember("headParts") && doc["headParts"].IsArray() ? doc["headParts"].Size() : 0;
    const auto tintJsonCount = doc.HasMember("tintLayers") && doc["tintLayers"].IsArray() ? doc["tintLayers"].Size() : 0;
    const auto morphJsonCount = doc.HasMember("faceMorphs") && doc["faceMorphs"].IsArray() ? doc["faceMorphs"].Size() : 0;
    const char* customFaceNif = doc.HasMember("customFaceNif") && doc["customFaceNif"].IsString() ? doc["customFaceNif"].GetString() : "";
    logger::debug("[ApplyJSON] === BEGIN NPC '{}' [{:08X}] npcPtr={:X} jsonMembers={} customFaceNif='{}' hpJson={} tintJson={} morphJson={} ===",
        npcName,
        npc->GetFormID(),
        reinterpret_cast<std::uintptr_t>(npc),
        doc.MemberCount(),
        customFaceNif,
        headPartJsonCount,
        tintJsonCount,
        morphJsonCount);

    try {
        logger::debug("[ApplyJSON] [{:08X}] Estado inicial: race={:X} skin={:X} headPartsPtr={:X} numHeadParts={} tintLayers={:X} faceData={:X}",
            npc->GetFormID(),
            reinterpret_cast<std::uintptr_t>(npc->race),
            reinterpret_cast<std::uintptr_t>(npc->farSkin),
            reinterpret_cast<std::uintptr_t>(npc->headParts),
            static_cast<int>(npc->numHeadParts),
            reinterpret_cast<std::uintptr_t>(npc->tintLayers),
            reinterpret_cast<std::uintptr_t>(npc->faceData));

        npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kIsChargenFacePreset);

        // 1. Atributos B?sicos
        logger::debug("[ApplyJSON] [{:08X}] Passo 1: Aplicando atributos basicos...", npc->GetFormID());
        if (doc.HasMember("height") && doc["height"].IsFloat()) {
            npc->height = doc["height"].GetFloat();
        }

        if (doc.HasMember("weight") && doc["weight"].IsFloat()) {
            npc->weight = doc["weight"].GetFloat();
        }

        if (doc.HasMember("isFemale") && doc["isFemale"].IsBool()) {
            if (doc["isFemale"].GetBool()) npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
            else npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
        }

        if (doc.HasMember("oppositeGenderAnim") && doc["oppositeGenderAnim"].IsBool()) {
            if (doc["oppositeGenderAnim"].GetBool()) npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kOppositeGenderAnims);
            else npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kOppositeGenderAnims);
        }

        if (doc.HasMember("bodyTintColor")) {
            auto& c = doc["bodyTintColor"];
            npc->bodyTintColor.red = c.HasMember("r") ? c["r"].GetInt() : 255;
            npc->bodyTintColor.green = c.HasMember("g") ? c["g"].GetInt() : 255;
            npc->bodyTintColor.blue = c.HasMember("b") ? c["b"].GetInt() : 255;
            npc->bodyTintColor.alpha = c.HasMember("a") ? c["a"].GetInt() : 255;
        }
        logger::debug("[ApplyJSON] [{:08X}] Passo 1 OK: height={} weight={} female={} oppositeGenderAnim={} bodyTint=({}, {}, {}, {})",
            npc->GetFormID(),
            npc->height,
            npc->weight,
            npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kFemale),
            npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kOppositeGenderAnims),
            npc->bodyTintColor.red,
            npc->bodyTintColor.green,
            npc->bodyTintColor.blue,
            npc->bodyTintColor.alpha);

        // 2. Associa??es de IDs
        logger::debug("[ApplyJSON] [{:08X}] Passo 2: Resolvendo FormIDs de Race, Skin e Outfits...", npc->GetFormID());
        if (doc.HasMember("race")) {
            if (auto race = FormUtil::ResolveForm<RE::TESRace>(doc["race"])) {
                npc->race = race;
            }
            else {
                logger::warn("[ApplyJSON] [{:08X}] Falha ao encontrar Race ID.", npc->GetFormID());
            }
        }

        if (doc.HasMember("skin")) {
            if (auto skin = FormUtil::ResolveForm<RE::TESObjectARMO>(doc["skin"])) {
                npc->farSkin = skin;
            }
            else {
                npc->farSkin = nullptr;
            }
        }

        if (doc.HasMember("defaultOutfit")) {
            if (auto out = FormUtil::ResolveForm<RE::BGSOutfit>(doc["defaultOutfit"])) {
                npc->defaultOutfit = out;
            }
            else npc->defaultOutfit = nullptr;
        }

        if (doc.HasMember("sleepOutfit")) {
            if (auto out = FormUtil::ResolveForm<RE::BGSOutfit>(doc["sleepOutfit"])) {
                npc->sleepOutfit = out;
            }
            else npc->sleepOutfit = nullptr;
        }

        /*if (doc.HasMember("voice")) {
            if (auto voice = FormUtil::ResolveForm<RE::BGSVoiceType>(doc["voice"])) {
                npc->SetObjectVoiceType(voice);
            }
            else {
                npc->SetObjectVoiceType(nullptr); 
            }
        }*/

        if (doc.HasMember("hairColor")) {
            if (auto hc = FormUtil::ResolveForm<RE::BGSColorForm>(doc["hairColor"])) {
                if (!npc->headRelatedData) {
                    logger::debug("[ApplyJSON] [{:08X}] Alocando HeadRelatedData...", npc->GetFormID());
                    npc->headRelatedData = new RE::TESNPC::HeadRelatedData();
                }
                npc->headRelatedData->hairColor = hc;
            }
        }
        logger::debug("[ApplyJSON] [{:08X}] Passo 2 OK: race={:X} skin={:X} outfit={:X} sleepOutfit={:X} hairColor={:X}",
            npc->GetFormID(),
            reinterpret_cast<std::uintptr_t>(npc->race),
            reinterpret_cast<std::uintptr_t>(npc->farSkin),
            reinterpret_cast<std::uintptr_t>(npc->defaultOutfit),
            reinterpret_cast<std::uintptr_t>(npc->sleepOutfit),
            npc->headRelatedData ? reinterpret_cast<std::uintptr_t>(npc->headRelatedData->hairColor) : 0);

        // 3. HeadParts
        logger::debug("[ApplyJSON] [{:08X}] Passo 3: Processando HeadParts... oldPtr={:X} oldCount={} jsonCount={}",
            npc->GetFormID(),
            reinterpret_cast<std::uintptr_t>(npc->headParts),
            static_cast<int>(npc->numHeadParts),
            headPartJsonCount);
        if (doc.HasMember("headParts") && doc["headParts"].IsArray()) {
            std::vector<RE::BGSHeadPart*> parts;
            std::set<RE::BGSHeadPart*> processed;
            const auto* customFaceNifString = customFaceNif;
            const bool hasCustomFaceNif = customFaceNifString && customFaceNifString[0] != '\0';
            auto* fallbackFaceHeadPart = FindCurrentSafeFaceHeadPart(npc);
            bool hasFaceHeadPart = false;

            std::function<void(RE::BGSHeadPart*)> AddPartAndExtras = [&](RE::BGSHeadPart* hp) {
                if (!hp || processed.contains(hp)) return;
                if (hp->type == RE::BGSHeadPart::HeadPartType::kFace) {
                    hasFaceHeadPart = true;
                    if (hasCustomFaceNif && !IsSafeFaceHeadPart(hp, npc->race, npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kFemale))) {
                        logger::warn("[ApplyJSON] [{:08X}] Face HeadPart '{}' aplicada por override apesar de incompatibilidade: type={} model='{}' race/sex valid={}.",
                            npc->GetFormID(),
                            hp->GetFormEditorID() ? hp->GetFormEditorID() : "",
                            hp->type.underlying(),
                            hp->GetModel() ? hp->GetModel() : "",
                            IsHeadPartAllowedForRaceSex(hp, npc->race, npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kFemale)));
                    }
                }
                processed.insert(hp);
                logger::debug("[ApplyJSON] [{:08X}] HeadPart final add: form={:08X} editorID={} type={} model={} extras={}",
                    npc->GetFormID(),
                    hp->GetFormID(),
                    hp->GetFormEditorID() ? hp->GetFormEditorID() : "",
                    hp->type.underlying(),
                    hp->GetModel() ? hp->GetModel() : "",
                    hp->extraParts.size());
                parts.push_back(hp);
                for (auto* extra : hp->extraParts) {
                    if (extra) AddPartAndExtras(extra);
                }
                };

            for (auto& hpJson : doc["headParts"].GetArray()) {
                const auto hpRef = FormUtil::FormRefDebugString(hpJson);
                logger::debug("[ApplyJSON] [{:08X}] Resolvendo HeadPart JSON '{}'.", npc->GetFormID(), hpRef);
                if (auto hp = FormUtil::ResolveForm<RE::BGSHeadPart>(hpJson)) {
                    logger::debug("[ApplyJSON] [{:08X}] HeadPart JSON OK: form={:08X} editorID={} type={} ptr={:X}",
                        npc->GetFormID(),
                        hp->GetFormID(),
                        hp->GetFormEditorID() ? hp->GetFormEditorID() : "",
                        hp->type.underlying(),
                        reinterpret_cast<std::uintptr_t>(hp));
                    AddPartAndExtras(hp);
                }
                else {
                    logger::warn("[ApplyJSON] [{:08X}] HeadPart ID {} nao encontrado e sera ignorado.", npc->GetFormID(), hpRef);
                }
            }

            if (hasCustomFaceNif && !hasFaceHeadPart && fallbackFaceHeadPart) {
                logger::warn("[ApplyJSON] [{:08X}] Nenhuma Face HeadPart no JSON; preservando fallback '{}'.",
                    npc->GetFormID(),
                    fallbackFaceHeadPart->GetFormEditorID() ? fallbackFaceHeadPart->GetFormEditorID() : "");
                AddPartAndExtras(fallbackFaceHeadPart);
            }

            if (npc->headParts) {
                logger::debug("[ApplyJSON] [{:08X}] Liberando memoria das HeadParts originais...", npc->GetFormID());
                RE::free(npc->headParts);
                npc->headParts = nullptr;
            }

            if (!parts.empty()) {
                logger::debug("[ApplyJSON] [{:08X}] Alocando {} novas HeadParts...", npc->GetFormID(), parts.size());
                auto newHeadParts = RE::calloc<RE::BGSHeadPart*>(parts.size());
                if (!newHeadParts) {
                    logger::error("[ApplyJSON] [{:08X}] calloc falhou para {} HeadParts.", npc->GetFormID(), parts.size());
                    return;
                }
                for (size_t i = 0; i < parts.size(); ++i) newHeadParts[i] = parts[i];
                npc->headParts = newHeadParts;
                npc->numHeadParts = static_cast<int8_t>(parts.size());
            }
            else {
                npc->numHeadParts = 0;
            }
            logger::debug("[ApplyJSON] [{:08X}] Passo 3 OK: newPtr={:X} newCount={}",
                npc->GetFormID(),
                reinterpret_cast<std::uintptr_t>(npc->headParts),
                static_cast<int>(npc->numHeadParts));
        }

        // 4. Tint Layers
        logger::debug("[ApplyJSON] [{:08X}] Passo 4: Processando Tint Layers...", npc->GetFormID());
        if (doc.HasMember("tintLayers") && doc["tintLayers"].IsArray()) {
            if (!npc->tintLayers) {
                logger::debug("[ApplyJSON] [{:08X}] Criando novo array de TintLayers...", npc->GetFormID());
                npc->tintLayers = new RE::BSTArray<RE::TESNPC::Layer*>();
            }
            else {
                logger::debug("[ApplyJSON] [{:08X}] Limpando {} TintLayers antigos...", npc->GetFormID(), npc->tintLayers->size());
                npc->tintLayers->clear();
            }

            int idx = 0;
            for (auto& l : doc["tintLayers"].GetArray()) {
                auto layer = new RE::TESNPC::Layer();
                layer->tintIndex = static_cast<uint16_t>(l["index"].GetInt());
                auto& c = l["color"];
                layer->tintColor.red = c.HasMember("r") ? c["r"].GetInt() : 0;
                layer->tintColor.green = c.HasMember("g") ? c["g"].GetInt() : 0;
                layer->tintColor.blue = c.HasMember("b") ? c["b"].GetInt() : 0;
                layer->tintColor.alpha = c.HasMember("a") ? c["a"].GetInt() : 255;
                layer->interpolationValue = static_cast<uint16_t>(l["interpolation"].GetFloat() * 100.0f);
                layer->preset = static_cast<uint16_t>(l["preset"].GetInt());

                npc->tintLayers->push_back(layer);
                idx++;
            }
            logger::debug("[ApplyJSON] [{:08X}] {} Tint Layers injetadas.", npc->GetFormID(), idx);
        }

        // 5. Face Morphs
        logger::debug("[ApplyJSON] [{:08X}] Passo 5: Processando Face Morphs...", npc->GetFormID());
        if (doc.HasMember("faceMorphs") && doc["faceMorphs"].IsArray()) {
            if (!npc->faceData) {
                logger::debug("[ApplyJSON] [{:08X}] Alocando memoria para FaceData...", npc->GetFormID());
                npc->faceData = new RE::TESNPC::FaceData();
            }
            auto mArray = doc["faceMorphs"].GetArray();
            for (rapidjson::SizeType i = 0; i < mArray.Size() && i < 19; i++) {
                npc->faceData->morphs[i] = mArray[i].GetFloat();
            }
            logger::debug("[ApplyJSON] [{:08X}] Passo 5 OK: faceData={:X} morphsAplicados={}",
                npc->GetFormID(),
                reinterpret_cast<std::uintptr_t>(npc->faceData),
                std::min<std::size_t>(mArray.Size(), 19));
        }

        logger::debug("[ApplyJSON] === END OK NPC {:08X}: headPartsPtr={:X} count={} tintLayers={:X} faceData={:X} ===",
            npc->GetFormID(),
            reinterpret_cast<std::uintptr_t>(npc->headParts),
            static_cast<int>(npc->numHeadParts),
            reinterpret_cast<std::uintptr_t>(npc->tintLayers),
            reinterpret_cast<std::uintptr_t>(npc->faceData));

    }
    catch (const std::exception& e) {
        logger::error("[ApplyJSON] CRITICAL EXCEPTION no NPC {:08X}: {}", npc->GetFormID(), e.what());
    }
    catch (...) {
        logger::error("[ApplyJSON] UNKNOWN EXCEPTION (Hard Crash evitado) no NPC {:08X}", npc->GetFormID());
    }
}

void Manager::PopulateAllLists(bool forceRefresh) {
    if (_isPopulated && !forceRefresh) return;

    logger::debug("Iniciando escaneamento de FormTypes...");
    _dataStore.clear();

    PopulateList<RE::BGSHeadPart>("Hair", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kHair;
        });
    PopulateList<RE::BGSHeadPart>("Facial Hair", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kFacialHair;
        });
    PopulateList<RE::BGSHeadPart>("Eye Brows", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kEyebrows;
        });
    PopulateList<RE::BGSHeadPart>("Eye", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kEyes;
        });
    PopulateList<RE::BGSHeadPart>("Face", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kFace;
        });
    PopulateList<RE::BGSHeadPart>("Misc", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kMisc;
        });
    PopulateList<RE::BGSHeadPart>("Scar", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kScar;
        });
    PopulateList<RE::TESRace>("Race");
    PopulateList<RE::BGSOutfit>("Outfit");
    PopulateList<RE::BGSColorForm>("ColorForm");
    PopulateList<RE::TESNPC>("NPC");
    PopulateList<RE::TESObjectARMO>("Armor");
    PopulateList<RE::BGSVoiceType>("Voice");

    const bool wasPopulated = _isPopulated;
    _isPopulated = true;
    if (!wasPopulated) {
        for (auto cb : _readyCallbacks) {
            if (cb) cb();
        }
        _readyCallbacks.clear();
    }
}

const std::vector<InternalFormInfo>& Manager::GetList(const std::string& typeName) {
    static std::vector<InternalFormInfo> empty;
    auto it = _dataStore.find(typeName);
    if (it != _dataStore.end()) {
        return it->second;
    }
    return empty;
}

void Manager::RegisterReadyCallback(std::function<void()> callback) {
    if (_isPopulated) {
        callback();
    }
    else {
        _readyCallbacks.push_back(callback);
    }
}


std::string Manager::ToUTF8(std::string_view a_str) {
    if (a_str.empty()) return "";

    // 1. Testa se a string j? ? um UTF-8 v?lido
    int u8Test = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_str.data(), static_cast<int>(a_str.size()), nullptr, 0);
    if (u8Test > 0) {
        // ? UTF-8 v?lido (Skyrim SE nativo), retorna sem corromper
        return std::string(a_str);
    }

    // 2. Se falhou, a string ? ANSI (Mod antigo ou locale espec?fico do Windows).
    // Precisamos converter de ANSI (CP_ACP) para UTF-16, e depois para UTF-8.
    int wlen = MultiByteToWideChar(CP_ACP, 0, a_str.data(), static_cast<int>(a_str.size()), nullptr, 0);
    if (wlen <= 0) return std::string(a_str);

    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, a_str.data(), static_cast<int>(a_str.size()), &wstr[0], wlen);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return std::string(a_str);

    std::string u8str(u8len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, &u8str[0], u8len, nullptr, nullptr);

    return u8str;
}

template <typename T>
void Manager::PopulateList(const std::string& a_typeName, std::function<bool(T*)> a_filter) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    auto& list = _dataStore[a_typeName];
    list.clear();

    const auto& forms = dataHandler->GetFormArray<T>();
    list.reserve(forms.size());

    for (const auto& form : forms) {
        if (!form) continue;

        if (a_filter && !a_filter(form)) {
            continue;
        }
        // Vari?veis de aux?lio para o log de erro caso o catch seja acionado
        RE::FormID currentID = 0;
        std::string currentPlugin = "Unknown";

        try {
            currentID = form->GetFormID();

            // Obt?m o nome do plugin de origem antes de qualquer processamento complexo
            if (auto file = form->GetFile(0)) {
                currentPlugin = std::string(file->GetFilename());
            }
            else {
                currentPlugin = "Dynamic";
            }

            InternalFormInfo info;
            info.formID = currentID;
            info.formType = a_typeName;
            info.pluginName = ToUTF8(currentPlugin);

            // EditorID: clib_util pode lan?ar exce??es em contextos raros de mem?ria
            std::string rawEditorID = clib_util::editorID::get_editorID(form);
            info.editorID = ToUTF8(rawEditorID);

            std::string rawName = "";
            if (form->Is(RE::FormType::NPC)) {
                if (auto npc = form->As<RE::TESNPC>()) {
                    rawName = npc->fullName.c_str();
                }
            }
            else if (auto fullName = form->As<RE::TESFullName>()) {
                rawName = fullName->fullName.c_str();
            }

            // A convers?o UTF-8 ? um ponto comum de falha se a string estiver corrompida
            info.name = ToUTF8(rawName);
            info.UpdateDisplayName();
            list.push_back(info);
        }
        catch (const std::exception& e) {
            // Log detalhado com FormID em Hexadecimal e o erro espec?fico
            logger::error("[PopulateList] Critical error on item {:08X} of plugin '{}' (Type: {}). Error: {}",
                currentID, currentPlugin, a_typeName, e.what());
        }
        catch (...) {
            // Captura erros desconhecidos que n?o herdam de std::exception
            logger::error("[PopulateList] Uknown error on item {:08X} of plugin '{}' (Type: {})",
                currentID, currentPlugin, a_typeName);
        }
    }
    logger::debug("Carregados {} itens do tipo {}", list.size(), a_typeName);
}

RE::NiPointer<RE::NiAVObject> Manager::LoadNifFromFile(const std::string& a_path) {
    RE::NiPointer<RE::NiNode> loadedNode;
    RE::BSModelDB::DBTraits::ArgsType args;
    auto result = RE::BSModelDB::Demand(a_path.c_str(), loadedNode, args);
    if (result != RE::BSResource::ErrorCode::kNone || !loadedNode) return nullptr;
    return RE::NiPointer<RE::NiAVObject>(loadedNode.get());
}

RE::BGSHeadPart* Manager::ExtractHeadPartFromNif(const std::string& a_nifPath) {
    auto nifRoot = LoadNifFromFile(a_nifPath);
    if (!nifRoot) {
        logger::error("[Head Swap] Falha ao carregar NIF: {}", a_nifPath);
        return nullptr;
    }

    std::string headNodeName = "";
    RE::BSVisit::TraverseScenegraphGeometries(nifRoot.get(), [&](RE::BSGeometry* a_geom) {
        if (a_geom && a_geom->name.c_str()) {
            std::string name(a_geom->name.c_str());
            const auto lowerName = LowerCopy(name);
            const bool looksLikeHead = lowerName.find("head") != std::string::npos || lowerName.find("face") != std::string::npos;
            const bool excluded =
                lowerName.find("hair") != std::string::npos ||
                lowerName.find("hairline") != std::string::npos ||
                lowerName.find("headband") != std::string::npos ||
                lowerName.find("brow") != std::string::npos ||
                lowerName.find("eye") != std::string::npos ||
                lowerName.find("mouth") != std::string::npos ||
                lowerName.find("beard") != std::string::npos ||
                lowerName.find("scar") != std::string::npos ||
                lowerName.find("mark") != std::string::npos ||
                lowerName.find("gash") != std::string::npos;
            if (looksLikeHead && !excluded) {
                headNodeName = name;
                return RE::BSVisit::BSVisitControl::kStop;
            }
        }
        return RE::BSVisit::BSVisitControl::kContinue;
        });

    if (headNodeName.empty()) {
        logger::error("[Head Swap] Nenhuma malha de cabeca encontrada no NIF.");
        return nullptr;
    }

    if (auto form = RE::TESForm::LookupByEditorID(headNodeName)) {
        auto* headPart = form->As<RE::BGSHeadPart>();
        if (headPart &&
            headPart->type == RE::BGSHeadPart::HeadPartType::kFace &&
            HeadPartHasUsableModel(headPart)) {
            if (!IsSafeFaceHeadPart(headPart)) {
                logger::warn("[Head Swap] HeadPart '{}' aceito por override apesar de incompatibilidade de race/sex.", headNodeName);
            }
            logger::debug("[Head Swap] HeadPart identificado com sucesso: {}", headNodeName);
            return headPart;
        }

        logger::warn("[Head Swap] Form '{}' encontrado, mas nao eh Face HeadPart com modelo valido: form={:08X} type={} model='{}'.",
            headNodeName,
            form->GetFormID(),
            headPart ? headPart->type.underlying() : -1,
            headPart && headPart->GetModel() ? headPart->GetModel() : "");
        return nullptr;
    }

    logger::error("[Head Swap] BGSHeadPart '{}' nao encontrado no jogo! Instale o mod necessario.", headNodeName);
    return nullptr;
}

namespace {
    struct DynVertex {
        float x;
        float y;
        float z;
        float w;
    };

    struct ClonedFaceGeometry {
        RE::NiPointer<RE::BSGeometry> geometry;
        RE::NiNode* parent = nullptr;
    };

    struct NeckSeamPatch {
        std::vector<DynVertex> vertices;
        std::vector<std::uint8_t> mask;
        float minZ = 0.0f;
        float preserveEnd = 0.0f;
        float blendEnd = 0.0f;
        std::size_t preserved = 0;
        std::size_t blended = 0;

        [[nodiscard]] bool empty() const
        {
            return vertices.empty() || mask.empty() || (preserved == 0 && blended == 0);
        }
    };

    std::string NormalizeGeometryKey(std::string_view a_name)
    {
        std::string key(a_name);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return key;
    }

    RE::NiPointer<RE::BSFaceGenAnimationData> CreateFreshFaceGenAnimationData()
    {
        RE::NiPointer<RE::BSFaceGenAnimationData> animData(RE::NiExtraData::Create<RE::BSFaceGenAnimationData>());
        if (animData) {
            animData->Reset(0.0f, true, true, true, true);
        }
        return animData;
    }

    void UpdateFaceNodeBounds(RE::NiAVObject* a_object)
    {
        if (!a_object) {
            return;
        }

        if (auto node = a_object->AsNode()) {
            for (auto& child : node->GetChildren()) {
                if (child) {
                    UpdateFaceNodeBounds(child.get());
                }
            }
        }

#ifndef SKYRIM_CROSS_VR
        a_object->UpdateWorldBound();
#endif
    }

    RE::BSDismemberSkinInstance* GetDismemberSkin(RE::NiSkinInstance* a_skin)
    {
        return a_skin ? netimmerse_cast<RE::BSDismemberSkinInstance*>(a_skin) : nullptr;
    }

    void EnableDismemberPartitions(RE::NiSkinInstance* a_skin)
    {
        auto* dismember = GetDismemberSkin(a_skin);
        if (!dismember) {
            return;
        }

        auto& runtime = dismember->GetRuntimeData();
        if (runtime.numPartitions <= 0 || !runtime.partitions) {
            return;
        }

        std::unordered_set<std::uint16_t> slots;
        for (std::int32_t i = 0; i < runtime.numPartitions; ++i) {
            auto& partition = runtime.partitions[i];
            partition.editorVisible = true;
            slots.insert(partition.slot);
        }

        for (const auto slot : slots) {
            dismember->UpdateDismemberPartion(slot, true);
        }
    }

    RE::NiAVObject* MapObjectByName(RE::NiAVObject* a_sourceObject, RE::NiAVObject* a_sourceRoot, RE::NiAVObject* a_targetRoot)
    {
        if (!a_sourceObject || !a_targetRoot) {
            return nullptr;
        }

        if (a_sourceObject == a_sourceRoot) {
            return a_targetRoot;
        }

        if (a_sourceObject->name.empty()) {
            return nullptr;
        }

        return a_targetRoot->GetObjectByName(a_sourceObject->name);
    }

    bool IsBaseHeadGeometry(RE::BSGeometry* a_geometry)
    {
        if (!a_geometry || a_geometry->name.empty()) {
            return false;
        }

        std::string lowerName = a_geometry->name.c_str();
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        const bool looksLikeHead = lowerName.find("head") != std::string::npos || lowerName.find("face") != std::string::npos;
        if (!looksLikeHead) {
            return false;
        }

        static constexpr std::array excludedTokens{
            "hair", "hairline", "headband", "brow", "eye", "mouth", "beard", "scar", "mark", "gash", "teeth", "tongue"
        };

        for (const auto token : excludedTokens) {
            if (lowerName.find(token) != std::string::npos) {
                return false;
            }
        }

        return true;
    }

    std::string GetFacePartKind(RE::BSGeometry* a_geometry)
    {
        if (!a_geometry || a_geometry->name.empty()) {
            return {};
        }

        std::string lowerName = a_geometry->name.c_str();
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (lowerName.find("hair") != std::string::npos || lowerName.find("hairline") != std::string::npos ||
            lowerName.find("headband") != std::string::npos) {
            return {};
        }

        if (IsBaseHeadGeometry(a_geometry)) {
            return "head";
        }
        if (lowerName.find("mouth") != std::string::npos) {
            return "mouth";
        }
        if (lowerName.find("eye") != std::string::npos) {
            return "eyes";
        }
        if (lowerName.find("brow") != std::string::npos) {
            return "brows";
        }
        if (lowerName.find("beard") != std::string::npos) {
            return "beard";
        }
        if (lowerName.find("scar") != std::string::npos || lowerName.find("mark") != std::string::npos ||
            lowerName.find("gash") != std::string::npos) {
            return "scar";
        }

        return {};
    }

    bool IsBakedExtraCandidate(RE::BSGeometry* a_geometry)
    {
        if (!a_geometry || a_geometry->name.empty() || IsBaseHeadGeometry(a_geometry)) {
            return false;
        }

        std::string lowerName = a_geometry->name.c_str();
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        static constexpr std::array bakedTokens{
            "hair", "hairline", "eye", "brow", "beard", "scar", "mark", "gash"
        };

        for (const auto token : bakedTokens) {
            if (lowerName.find(token) != std::string::npos) {
                return true;
            }
        }

        return false;
    }

    std::string GetBakedExtraKind(RE::BSGeometry* a_geometry)
    {
        if (!a_geometry || a_geometry->name.empty()) {
            return {};
        }

        std::string lowerName = NormalizeGeometryKey(a_geometry->name.c_str());
        if (lowerName.find("hairline") != std::string::npos) {
            return "hairline";
        }
        if (lowerName.find("hair") != std::string::npos) {
            return "hair";
        }
        if (lowerName.find("eye") != std::string::npos) {
            return "eyes";
        }
        if (lowerName.find("brow") != std::string::npos) {
            return "brows";
        }
        if (lowerName.find("beard") != std::string::npos) {
            return "beard";
        }
        if (lowerName.find("scar") != std::string::npos || lowerName.find("mark") != std::string::npos ||
            lowerName.find("gash") != std::string::npos) {
            return "scar";
        }

        return "baked";
    }

    bool ShouldUseRuntimeHeadMaterial(RE::BSGeometry* a_geometry)
    {
        if (!a_geometry || a_geometry->name.empty()) {
            return false;
        }

        const auto lowerName = NormalizeGeometryKey(a_geometry->name.c_str());
        if (lowerName.find("hair") != std::string::npos || lowerName.find("hairline") != std::string::npos ||
            lowerName.find("brow") != std::string::npos || lowerName.find("eye") != std::string::npos ||
            lowerName.find("mouth") != std::string::npos || lowerName.find("beard") != std::string::npos) {
            return false;
        }

        return IsBaseHeadGeometry(a_geometry) || lowerName.find("scar") != std::string::npos ||
               lowerName.find("mark") != std::string::npos || lowerName.find("gash") != std::string::npos;
    }

    bool CopyRuntimeShaderProperty(RE::BSGeometry* a_targetGeometry, RE::BSGeometry* a_donorGeometry, const std::string& a_context)
    {
        if (!a_targetGeometry || !a_donorGeometry) {
            return false;
        }

        auto& donorRuntime = a_donorGeometry->GetGeometryRuntimeData();
        auto* donorShader = donorRuntime.shaderProperty.get();
        if (!donorShader) {
            logger::debug("[Face Swap] Sem shader runtime donor para {}.", a_context);
            return false;
        }

        auto& targetRuntime = a_targetGeometry->GetGeometryRuntimeData();
        RE::NiPointer<RE::NiObject> clonedShaderObject(donorShader->Clone());
        auto* clonedShader = clonedShaderObject ? netimmerse_cast<RE::BSShaderProperty*>(clonedShaderObject.get()) : nullptr;
        if (clonedShader) {
            targetRuntime.shaderProperty = RE::NiPointer<RE::BSShaderProperty>(clonedShader);
            clonedShader->SetupGeometry(a_targetGeometry);
            logger::debug("[Face Swap] Shader/material runtime da head aplicado em {}.", a_context);
            return true;
        }

        targetRuntime.shaderProperty = donorRuntime.shaderProperty;
        donorShader->SetupGeometry(a_targetGeometry);
        logger::debug("[Face Swap] Shader/material runtime da head compartilhado em {}.", a_context);
        return true;
    }

    bool HasRuntimeGeometryWithSameName(RE::NiAVObject* a_root, RE::BSGeometry* a_sourceGeometry)
    {
        if (!a_root || !a_sourceGeometry || a_sourceGeometry->name.empty()) {
            return false;
        }

        bool found = false;
        RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) {
            if (a_geometry && !a_geometry->name.empty() && std::string_view(a_geometry->name.c_str()) == a_sourceGeometry->name.c_str()) {
                found = true;
                return RE::BSVisit::BSVisitControl::kStop;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });

        return found;
    }

    bool HasRuntimeGeometryWithName(RE::NiAVObject* a_root, std::string_view a_name)
    {
        if (!a_root || a_name.empty()) {
            return false;
        }

        const auto targetKey = NormalizeGeometryKey(a_name);
        bool found = false;
        RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) {
            if (a_geometry && !a_geometry->name.empty() && NormalizeGeometryKey(a_geometry->name.c_str()) == targetKey) {
                found = true;
                return RE::BSVisit::BSVisitControl::kStop;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });

        return found;
    }

    struct AppliedHeadPartInfo {
        std::unordered_set<std::string> allEditorIDs;
        std::unordered_set<std::string> primaryEditorIDs;
        std::unordered_map<std::string, std::string> extraOwnerByID;
    };

    std::string GetHeadPartEditorID(RE::BGSHeadPart* a_headPart)
    {
        if (!a_headPart) {
            return {};
        }

        if (auto editorID = a_headPart->GetFormEditorID(); editorID && editorID[0] != '\0') {
            return editorID;
        }

        if (!a_headPart->formEditorID.empty()) {
            return a_headPart->formEditorID.c_str();
        }

        return {};
    }

    void CollectOwnedExtraParts(RE::BGSHeadPart* a_headPart, std::set<RE::BGSHeadPart*>& a_ownedExtras)
    {
        if (!a_headPart) {
            return;
        }

        for (auto* extra : a_headPart->extraParts) {
            if (!extra || a_ownedExtras.contains(extra)) {
                continue;
            }

            a_ownedExtras.insert(extra);
            CollectOwnedExtraParts(extra, a_ownedExtras);
        }
    }

    void AddExtraHeadPartInfo(RE::BGSHeadPart* a_headPart, AppliedHeadPartInfo& a_info, const std::string& a_ownerEditorID)
    {
        if (!a_headPart) {
            return;
        }

        const auto editorID = GetHeadPartEditorID(a_headPart);
        if (!editorID.empty()) {
            a_info.allEditorIDs.emplace(editorID);
            if (!a_ownerEditorID.empty()) {
                a_info.extraOwnerByID.try_emplace(editorID, a_ownerEditorID);
            }
        }

        for (auto* extra : a_headPart->extraParts) {
            AddExtraHeadPartInfo(extra, a_info, a_ownerEditorID);
        }
    }

    void AddPrimaryHeadPartInfo(RE::BGSHeadPart* a_headPart, AppliedHeadPartInfo& a_info)
    {
        if (!a_headPart) {
            return;
        }

        const auto editorID = GetHeadPartEditorID(a_headPart);
        if (!editorID.empty()) {
            a_info.allEditorIDs.emplace(editorID);
            a_info.primaryEditorIDs.emplace(editorID);
        }

        for (auto* extra : a_headPart->extraParts) {
            AddExtraHeadPartInfo(extra, a_info, editorID);
        }
    }

    AppliedHeadPartInfo GetAppliedHeadPartInfo(RE::TESNPC* a_npc)
    {
        AppliedHeadPartInfo info;
        if (!a_npc || !a_npc->headParts) {
            return info;
        }

        std::set<RE::BGSHeadPart*> ownedExtras;
        for (int i = 0; i < a_npc->numHeadParts; ++i) {
            CollectOwnedExtraParts(a_npc->headParts[i], ownedExtras);
        }

        for (int i = 0; i < a_npc->numHeadParts; ++i) {
            auto* headPart = a_npc->headParts[i];
            if (!headPart) {
                continue;
            }

            if (ownedExtras.contains(headPart)) {
                const auto editorID = GetHeadPartEditorID(headPart);
                if (!editorID.empty()) {
                    info.allEditorIDs.emplace(editorID);
                }
                continue;
            }

            AddPrimaryHeadPartInfo(headPart, info);
        }

        if (info.primaryEditorIDs.empty()) {
            for (int i = 0; i < a_npc->numHeadParts; ++i) {
                AddPrimaryHeadPartInfo(a_npc->headParts[i], info);
            }
        }

        return info;
    }

    std::string FindAppliedHeadPartIDForName(std::string_view a_name, const AppliedHeadPartInfo& a_info)
    {
        if (a_name.empty()) {
            return {};
        }

        const std::string rawName(a_name);
        if (a_info.allEditorIDs.contains(rawName)) {
            return rawName;
        }

        const auto targetKey = NormalizeGeometryKey(a_name);
        for (const auto& editorID : a_info.allEditorIDs) {
            if (NormalizeGeometryKey(editorID) == targetKey) {
                return editorID;
            }
        }

        return {};
    }

    bool IsExtraHeadPartSatisfiedByOwner(std::string_view a_headPartID, const AppliedHeadPartInfo& a_info, RE::NiAVObject* a_runtimeRoot)
    {
        const auto ownerIt = a_info.extraOwnerByID.find(std::string(a_headPartID));
        if (ownerIt == a_info.extraOwnerByID.end() || ownerIt->second.empty()) {
            return false;
        }

        return HasRuntimeGeometryWithName(a_runtimeRoot, ownerIt->second);
    }

    std::string GetExtraHeadPartOwner(std::string_view a_headPartID, const AppliedHeadPartInfo& a_info)
    {
        const auto ownerIt = a_info.extraOwnerByID.find(std::string(a_headPartID));
        return ownerIt != a_info.extraOwnerByID.end() ? ownerIt->second : std::string{};
    }

    bool RebindSkinToTargetSkeleton(RE::BSGeometry* a_sourceGeometry, RE::BSGeometry* a_cloneGeometry, RE::NiAVObject* a_sourceRoot, RE::NiAVObject* a_targetRoot)
    {
        if (!a_sourceGeometry || !a_cloneGeometry || !a_sourceRoot || !a_targetRoot) {
            return false;
        }

        auto* sourceSkin = a_sourceGeometry->GetGeometryRuntimeData().skinInstance.get();
        auto* cloneSkin = a_cloneGeometry->GetGeometryRuntimeData().skinInstance.get();
        if (!sourceSkin || !cloneSkin || !sourceSkin->skinData || !sourceSkin->bones || !cloneSkin->bones) {
            return false;
        }

        const auto boneCount = sourceSkin->skinData->GetBoneCount();
        for (std::uint32_t i = 0; i < boneCount; ++i) {
            auto* sourceBone = sourceSkin->bones[i];
            if (!sourceBone || sourceBone->name.empty()) {
                return false;
            }

            auto* targetBone = a_targetRoot->GetObjectByName(sourceBone->name);
            if (!targetBone) {
                logger::warn("[Face Swap] Pulando '{}': bone '{}' nao existe no skeleton do ator.",
                    a_sourceGeometry->name.c_str(), sourceBone->name.c_str());
                return false;
            }

            cloneSkin->bones[i] = targetBone;
            if (cloneSkin->boneWorldTransforms) {
                cloneSkin->boneWorldTransforms[i] = std::addressof(targetBone->world);
            }
        }

        cloneSkin->rootParent = MapObjectByName(sourceSkin->rootParent, a_sourceRoot, a_targetRoot);
        if (!cloneSkin->rootParent) {
            cloneSkin->rootParent = a_targetRoot;
        }
        cloneSkin->frameID = std::numeric_limits<std::uint32_t>::max();

        EnableDismemberPartitions(cloneSkin);
        return true;
    }

    void RememberReplacementGeometry(RE::BSGeometry* a_geometry, std::unordered_set<std::string>& a_names, std::unordered_set<std::uint16_t>& a_slots)
    {
        if (!a_geometry) {
            return;
        }

        if (!a_geometry->name.empty()) {
            a_names.emplace(a_geometry->name.c_str());
        }

        if (auto* dismember = GetDismemberSkin(a_geometry->GetGeometryRuntimeData().skinInstance.get())) {
            auto& runtime = dismember->GetRuntimeData();
            if (runtime.numPartitions > 0 && runtime.partitions) {
                for (std::int32_t i = 0; i < runtime.numPartitions; ++i) {
                    a_slots.insert(runtime.partitions[i].slot);
                }
            }
        }
    }

    bool ShouldHideOriginalGeometry(RE::BSGeometry* a_geometry, const std::unordered_set<std::string>& a_names)
    {
        if (!a_geometry) {
            return false;
        }

        return IsBaseHeadGeometry(a_geometry) || (!a_geometry->name.empty() && a_names.contains(a_geometry->name.c_str()));
    }

    bool CopyVerticesToRuntimeGeometry(RE::BSGeometry* a_targetGeometry, RE::BSGeometry* a_sourceGeometry, const std::string& a_context, NeckSeamPatch* a_outNeckPatch = nullptr)
    {
        if (a_outNeckPatch) {
            *a_outNeckPatch = {};
        }

        auto* targetDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(a_targetGeometry);
        auto* sourceDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(a_sourceGeometry);
        if (!targetDynShape || !sourceDynShape) {
            logger::error("[Face Swap] {} geometry nao eh BSDynamicTriShape.", a_context);
            return false;
        }

        const auto targetVertCount = targetDynShape->GetTrishapeRuntimeData().vertexCount;
        const auto sourceVertCount = sourceDynShape->GetTrishapeRuntimeData().vertexCount;
        if (targetVertCount != sourceVertCount) {
            logger::warn("[Face Swap] {} vertices incompativeis: actor={} source={}.", a_context, targetVertCount, sourceVertCount);
            return false;
        }

        auto* targetVerts = reinterpret_cast<DynVertex*>(targetDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
        auto* sourceVerts = reinterpret_cast<DynVertex*>(sourceDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
        if (!targetVerts || !sourceVerts) {
            logger::error("[Face Swap] Dynamic vertex buffer ausente em {}.", a_context);
            return false;
        }

        std::vector<DynVertex> finalVerts(targetVertCount);
        bool usedNeckBlend = false;
        std::size_t preservedNeckVertices = 0;
        std::size_t blendedNeckVertices = 0;
        float minZ = std::numeric_limits<float>::max();
        float maxZ = std::numeric_limits<float>::lowest();
        if (a_context == "head") {
            for (std::uint32_t i = 0; i < targetVertCount; ++i) {
                minZ = std::min(minZ, targetVerts[i].z);
                maxZ = std::max(maxZ, targetVerts[i].z);
            }
        }

        const float zRange = maxZ - minZ;
        const float preserveHeight = std::min(zRange * 0.22f, 5.0f);
        const float blendHeight = std::min(zRange * 0.42f, 9.0f);
        const float preserveEnd = minZ + preserveHeight;
        const float blendEnd = minZ + std::max(blendHeight, preserveHeight + 0.001f);

        if (a_context == "head" && zRange > 0.001f && a_outNeckPatch) {
            a_outNeckPatch->vertices.resize(targetVertCount);
            a_outNeckPatch->mask.assign(targetVertCount, 0);
            a_outNeckPatch->minZ = minZ;
            a_outNeckPatch->preserveEnd = preserveEnd;
            a_outNeckPatch->blendEnd = blendEnd;
        }

        for (std::uint32_t i = 0; i < targetVertCount; ++i) {
            finalVerts[i] = sourceVerts[i];

            if (a_context == "head" && zRange > 0.001f) {
                const float z = targetVerts[i].z;
                if (z <= preserveEnd) {
                    finalVerts[i].x = targetVerts[i].x;
                    finalVerts[i].y = targetVerts[i].y;
                    finalVerts[i].z = targetVerts[i].z;
                    usedNeckBlend = true;
                    ++preservedNeckVertices;
                    if (a_outNeckPatch) {
                        a_outNeckPatch->vertices[i] = finalVerts[i];
                        a_outNeckPatch->mask[i] = 1;
                    }
                } else if (z < blendEnd) {
                    const float rawT = (z - preserveEnd) / (blendEnd - preserveEnd);
                    const float t = rawT * rawT * (3.0f - (2.0f * rawT));
                    finalVerts[i].x = targetVerts[i].x + ((sourceVerts[i].x - targetVerts[i].x) * t);
                    finalVerts[i].y = targetVerts[i].y + ((sourceVerts[i].y - targetVerts[i].y) * t);
                    finalVerts[i].z = targetVerts[i].z + ((sourceVerts[i].z - targetVerts[i].z) * t);
                    usedNeckBlend = true;
                    ++blendedNeckVertices;
                    if (a_outNeckPatch) {
                        a_outNeckPatch->vertices[i] = finalVerts[i];
                        a_outNeckPatch->mask[i] = 1;
                    }
                }
            }

            targetVerts[i].x = finalVerts[i].x;
            targetVerts[i].y = finalVerts[i].y;
            targetVerts[i].z = finalVerts[i].z;
        }

        if (usedNeckBlend) {
            if (a_outNeckPatch) {
                a_outNeckPatch->preserved = preservedNeckVertices;
                a_outNeckPatch->blended = blendedNeckVertices;
            }
            logger::debug("[Face Swap] Neck seam patch gerado na head: minZ={:.4f} preserveEnd={:.4f} blendEnd={:.4f} preserved={} blended={}.",
                minZ, preserveEnd, blendEnd, preservedNeckVertices, blendedNeckVertices);
        }

        if (targetDynShape->GetExtraData("FOD")) {
            targetDynShape->RemoveExtraData("FOD");
        }

        auto* newFod = RE::BSFaceGenBaseMorphExtraData::Create(nullptr, false);
        if (newFod) {
            newFod->vertexCount = targetVertCount;
            newFod->modelVertexCount = targetVertCount;
            newFod->vertexData = static_cast<RE::NiPoint3*>(
                RE::MemoryManager::GetSingleton()->Allocate(sizeof(RE::NiPoint3) * targetVertCount, 0, false));

            for (std::uint32_t i = 0; i < targetVertCount; ++i) {
                newFod->vertexData[i].x = finalVerts[i].x;
                newFod->vertexData[i].y = finalVerts[i].y;
                newFod->vertexData[i].z = finalVerts[i].z;
            }
            targetDynShape->AddExtraData(newFod);
        }

        if (auto* fmdExtra = a_targetGeometry->GetExtraData("FMD")) {
            if (auto* fmd = static_cast<RE::BSFaceGenModelExtraData*>(fmdExtra)) {
                if (fmd->m_model && fmd->m_model->modelMeshData && fmd->m_model->modelMeshData->faceNode) {
                    auto* modelRoot = fmd->m_model->modelMeshData->faceNode->AsNode();
                    if (modelRoot) {
                        RE::BSVisit::TraverseScenegraphGeometries(modelRoot, [&](RE::BSGeometry* a_modelGeometry) {
                            auto* modelDynShape = a_modelGeometry ? netimmerse_cast<RE::BSDynamicTriShape*>(a_modelGeometry) : nullptr;
                            if (!modelDynShape || modelDynShape->GetTrishapeRuntimeData().vertexCount != targetVertCount) {
                                return RE::BSVisit::BSVisitControl::kContinue;
                            }

                            auto* modelVerts = reinterpret_cast<DynVertex*>(modelDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
                            if (modelVerts) {
                                for (std::uint32_t i = 0; i < targetVertCount; ++i) {
                                    modelVerts[i].x = finalVerts[i].x;
                                    modelVerts[i].y = finalVerts[i].y;
                                    modelVerts[i].z = finalVerts[i].z;
                                }
                            }
                            return RE::BSVisit::BSVisitControl::kStop;
                        });
                    }
                }
            }
        }

#ifndef SKYRIM_CROSS_VR
        targetDynShape->UpdateWorldBound();
#endif
        return true;
    }

    bool ApplyNeckSeamPatchToGeometry(RE::BSGeometry* a_targetGeometry, const NeckSeamPatch& a_patch, const std::string& a_context)
    {
        if (!a_targetGeometry || a_patch.empty()) {
            return false;
        }

        auto* targetDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(a_targetGeometry);
        if (!targetDynShape) {
            logger::warn("[Face Swap] Neck seam patch ignorado em {}: target nao eh BSDynamicTriShape.", a_context);
            return false;
        }

        const auto targetVertCount = targetDynShape->GetTrishapeRuntimeData().vertexCount;
        if (targetVertCount != a_patch.vertices.size() || targetVertCount != a_patch.mask.size()) {
            logger::warn("[Face Swap] Neck seam patch ignorado em {}: vertices actor={} patch={}/{}.",
                a_context, targetVertCount, a_patch.vertices.size(), a_patch.mask.size());
            return false;
        }

        auto* targetVerts = reinterpret_cast<DynVertex*>(targetDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
        if (!targetVerts) {
            logger::warn("[Face Swap] Neck seam patch ignorado em {}: dynamicData ausente.", a_context);
            return false;
        }

        std::size_t patchedVertices = 0;
        for (std::uint32_t i = 0; i < targetVertCount; ++i) {
            if (!a_patch.mask[i]) {
                continue;
            }

            targetVerts[i].x = a_patch.vertices[i].x;
            targetVerts[i].y = a_patch.vertices[i].y;
            targetVerts[i].z = a_patch.vertices[i].z;
            ++patchedVertices;
        }

        if (auto* fodExtra = targetDynShape->GetExtraData("FOD")) {
            if (auto* fod = static_cast<RE::BSFaceGenBaseMorphExtraData*>(fodExtra);
                fod && fod->vertexData && fod->vertexCount == targetVertCount) {
                for (std::uint32_t i = 0; i < targetVertCount; ++i) {
                    if (!a_patch.mask[i]) {
                        continue;
                    }

                    fod->vertexData[i].x = a_patch.vertices[i].x;
                    fod->vertexData[i].y = a_patch.vertices[i].y;
                    fod->vertexData[i].z = a_patch.vertices[i].z;
                }
            }
        }

        if (auto* fmdExtra = a_targetGeometry->GetExtraData("FMD")) {
            if (auto* fmd = static_cast<RE::BSFaceGenModelExtraData*>(fmdExtra)) {
                if (fmd->m_model && fmd->m_model->modelMeshData && fmd->m_model->modelMeshData->faceNode) {
                    auto* modelRoot = fmd->m_model->modelMeshData->faceNode->AsNode();
                    if (modelRoot) {
                        RE::BSVisit::TraverseScenegraphGeometries(modelRoot, [&](RE::BSGeometry* a_modelGeometry) {
                            auto* modelDynShape = a_modelGeometry ? netimmerse_cast<RE::BSDynamicTriShape*>(a_modelGeometry) : nullptr;
                            if (!modelDynShape || modelDynShape->GetTrishapeRuntimeData().vertexCount != targetVertCount) {
                                return RE::BSVisit::BSVisitControl::kContinue;
                            }

                            auto* modelVerts = reinterpret_cast<DynVertex*>(modelDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
                            if (modelVerts) {
                                for (std::uint32_t i = 0; i < targetVertCount; ++i) {
                                    if (!a_patch.mask[i]) {
                                        continue;
                                    }

                                    modelVerts[i].x = a_patch.vertices[i].x;
                                    modelVerts[i].y = a_patch.vertices[i].y;
                                    modelVerts[i].z = a_patch.vertices[i].z;
                                }
                            }
                            return RE::BSVisit::BSVisitControl::kStop;
                        });
                    }
                }
            }
        }

#ifndef SKYRIM_CROSS_VR
        targetDynShape->UpdateWorldBound();
#endif
        logger::debug("[Face Swap] Neck seam patch reaplicado em {}: vertices={} preserveEnd={:.4f} blendEnd={:.4f}.",
            a_context, patchedVertices, a_patch.preserveEnd, a_patch.blendEnd);
        return patchedVertices > 0;
    }

    bool BuildMismatchedTopologyNeckPatch(RE::BSGeometry* a_referenceHead, RE::BSGeometry* a_targetHead, NeckSeamPatch& a_outPatch)
    {
        a_outPatch = {};

        auto* referenceDyn = netimmerse_cast<RE::BSDynamicTriShape*>(a_referenceHead);
        auto* targetDyn = netimmerse_cast<RE::BSDynamicTriShape*>(a_targetHead);
        if (!referenceDyn || !targetDyn) {
            return false;
        }

        const auto referenceVertCount = referenceDyn->GetTrishapeRuntimeData().vertexCount;
        const auto targetVertCount = targetDyn->GetTrishapeRuntimeData().vertexCount;
        auto* referenceVerts = reinterpret_cast<DynVertex*>(referenceDyn->GetDynamicTrishapeRuntimeData().dynamicData);
        auto* targetVerts = reinterpret_cast<DynVertex*>(targetDyn->GetDynamicTrishapeRuntimeData().dynamicData);
        if (!referenceVerts || !targetVerts || referenceVertCount == 0 || targetVertCount == 0) {
            return false;
        }

        float referenceMinZ = std::numeric_limits<float>::max();
        float referenceMaxZ = std::numeric_limits<float>::lowest();
        for (std::uint32_t i = 0; i < referenceVertCount; ++i) {
            referenceMinZ = std::min(referenceMinZ, referenceVerts[i].z);
            referenceMaxZ = std::max(referenceMaxZ, referenceVerts[i].z);
        }

        float targetMinZ = std::numeric_limits<float>::max();
        float targetMaxZ = std::numeric_limits<float>::lowest();
        for (std::uint32_t i = 0; i < targetVertCount; ++i) {
            targetMinZ = std::min(targetMinZ, targetVerts[i].z);
            targetMaxZ = std::max(targetMaxZ, targetVerts[i].z);
        }

        const float referenceRange = referenceMaxZ - referenceMinZ;
        const float targetRange = targetMaxZ - targetMinZ;
        if (referenceRange <= 0.001f || targetRange <= 0.001f) {
            return false;
        }

        const float referenceBandEnd = referenceMinZ + std::min(referenceRange * 0.45f, 9.5f);
        const float preserveEnd = targetMinZ + std::min(targetRange * 0.20f, 4.5f);
        const float blendEnd = targetMinZ + std::min(targetRange * 0.38f, 8.0f);

        std::vector<std::uint32_t> referenceCandidates;
        referenceCandidates.reserve(referenceVertCount / 3);
        for (std::uint32_t i = 0; i < referenceVertCount; ++i) {
            if (referenceVerts[i].z <= referenceBandEnd) {
                referenceCandidates.push_back(i);
            }
        }
        if (referenceCandidates.empty()) {
            return false;
        }

        a_outPatch.vertices.resize(targetVertCount);
        a_outPatch.mask.assign(targetVertCount, 0);
        a_outPatch.minZ = targetMinZ;
        a_outPatch.preserveEnd = preserveEnd;
        a_outPatch.blendEnd = blendEnd;

        for (std::uint32_t i = 0; i < targetVertCount; ++i) {
            const float z = targetVerts[i].z;
            if (z > blendEnd) {
                continue;
            }

            std::uint32_t nearestIndex = referenceCandidates.front();
            float nearestDistance = std::numeric_limits<float>::max();
            for (const auto candidateIndex : referenceCandidates) {
                const float dx = targetVerts[i].x - referenceVerts[candidateIndex].x;
                const float dy = targetVerts[i].y - referenceVerts[candidateIndex].y;
                const float dz = targetVerts[i].z - referenceVerts[candidateIndex].z;
                const float distance = (dx * dx) + (dy * dy) + (dz * dz * 0.25f);
                if (distance < nearestDistance) {
                    nearestDistance = distance;
                    nearestIndex = candidateIndex;
                }
            }

            DynVertex patched = targetVerts[i];
            if (z <= preserveEnd) {
                patched.x = referenceVerts[nearestIndex].x;
                patched.y = referenceVerts[nearestIndex].y;
                patched.z = referenceVerts[nearestIndex].z;
                ++a_outPatch.preserved;
            } else {
                const float rawT = (z - preserveEnd) / std::max(blendEnd - preserveEnd, 0.001f);
                const float t = rawT * rawT * (3.0f - (2.0f * rawT));
                patched.x = referenceVerts[nearestIndex].x + ((targetVerts[i].x - referenceVerts[nearestIndex].x) * t);
                patched.y = referenceVerts[nearestIndex].y + ((targetVerts[i].y - referenceVerts[nearestIndex].y) * t);
                patched.z = referenceVerts[nearestIndex].z + ((targetVerts[i].z - referenceVerts[nearestIndex].z) * t);
                ++a_outPatch.blended;
            }

            a_outPatch.vertices[i] = patched;
            a_outPatch.mask[i] = 1;
        }

        if (!a_outPatch.empty()) {
            logger::debug("[Face Swap] Neck seam patch gerado para topologia diferente: refVerts={} targetVerts={} targetMinZ={:.4f} preserveEnd={:.4f} blendEnd={:.4f} preserved={} blended={}.",
                referenceVertCount,
                targetVertCount,
                targetMinZ,
                preserveEnd,
                blendEnd,
                a_outPatch.preserved,
                a_outPatch.blended);
            return true;
        }

        return false;
    }

    RE::BSGeometry* FindGeometryByName(RE::NiAVObject* a_root, std::string_view a_name)
    {
        if (!a_root || a_name.empty()) {
            return nullptr;
        }

        const auto targetKey = NormalizeGeometryKey(a_name);
        RE::BSGeometry* found = nullptr;
        RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) {
            if (a_geometry && !a_geometry->name.empty() && NormalizeGeometryKey(a_geometry->name.c_str()) == targetKey) {
                found = a_geometry;
                return RE::BSVisit::BSVisitControl::kStop;
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });

        return found;
    }

    const char* SafeName(const RE::BSFixedString& a_name)
    {
        return a_name.empty() ? "SemNome" : a_name.c_str();
    }

    const char* SafeRTTIName(RE::NiObject* a_object)
    {
        if (!a_object) {
            return "null";
        }

        auto* rtti = a_object->GetRTTI();
        return rtti ? rtti->GetName() : "SemRTTI";
    }

    void LogObjectExtraData(RE::NiObjectNET* a_object, const std::string& a_prefix)
    {
        if (!a_object) {
            logger::debug("[FaceDiag] {} extraData: object=null", a_prefix);
            return;
        }

        const auto extraCount = a_object->GetExtraDataSize();
        logger::debug("[FaceDiag] {} extraDataCount={}", a_prefix, extraCount);
        for (std::uint16_t i = 0; i < extraCount; ++i) {
            auto* extra = a_object->GetExtraDataAt(i);
            logger::debug("[FaceDiag] {} extra[{}] ptr={:X} rtti={} name={} streamable={} cloneable={}",
                a_prefix,
                i,
                reinterpret_cast<std::uintptr_t>(extra),
                SafeRTTIName(extra),
                extra ? SafeName(extra->GetName()) : "null",
                extra ? extra->IsStreamable() : false,
                extra ? extra->IsCloneable() : false);
        }
    }

    void LogTextureSet(RE::BSTextureSet* a_textureSet, const std::string& a_prefix)
    {
        if (!a_textureSet) {
            logger::debug("[FaceDiag] {} textureSet=null", a_prefix);
            return;
        }

        logger::debug("[FaceDiag] {} textureSet ptr={:X} rtti={}",
            a_prefix,
            reinterpret_cast<std::uintptr_t>(a_textureSet),
            SafeRTTIName(a_textureSet));

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::BSTextureSet::Texture::kTotal); ++i) {
            const auto texture = static_cast<RE::BSTextureSet::Texture>(i);
            const char* path = a_textureSet->GetTexturePath(texture);
            logger::debug("[FaceDiag] {} texture[{}]={}", a_prefix, i, path ? path : "");
        }
    }

    void LogShaderProperty(RE::BSGeometry* a_geometry, const std::string& a_prefix)
    {
        if (!a_geometry) {
            logger::debug("[FaceDiag] {} shader: geometry=null", a_prefix);
            return;
        }

        auto& runtime = a_geometry->GetGeometryRuntimeData();
        auto* shader = runtime.shaderProperty.get();
        logger::debug("[FaceDiag] {} shader ptr={:X} rtti={} alpha={} flags={:016X} materialPtr={:X}",
            a_prefix,
            reinterpret_cast<std::uintptr_t>(shader),
            SafeRTTIName(shader),
            shader ? shader->alpha : -1.0f,
            shader ? shader->flags.underlying() : 0,
            shader ? reinterpret_cast<std::uintptr_t>(shader->GetBaseMaterial()) : 0);

        if (!shader || !shader->GetBaseMaterial()) {
            return;
        }

        auto* material = shader->GetBaseMaterial();
        logger::debug("[FaceDiag] {} material ptr={:X} type={} feature={}",
            a_prefix,
            reinterpret_cast<std::uintptr_t>(material),
            static_cast<std::uint32_t>(material->GetType()),
            static_cast<std::uint32_t>(material->GetFeature()));

        if (material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
            logger::debug("[FaceDiag] {} material nao eh BSLightingShaderMaterialBase", a_prefix);
            return;
        }

        auto* lightingMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(material);
        logger::debug("[FaceDiag] {} lightingMaterial diffuseRT={} clamp={} alpha={} specPower={} specScale={} subsurface={} rim={}",
            a_prefix,
            lightingMaterial->diffuseRenderTargetSourceIndex,
            lightingMaterial->textureClampMode,
            lightingMaterial->materialAlpha,
            lightingMaterial->specularPower,
            lightingMaterial->specularColorScale,
            lightingMaterial->subSurfaceLightRolloff,
            lightingMaterial->rimLightPower);

        LogTextureSet(lightingMaterial->GetTextureSet().get(), a_prefix);
    }

    void LogSkinInfo(RE::BSGeometry* a_geometry, const std::string& a_prefix)
    {
        if (!a_geometry) {
            return;
        }

        auto* skin = a_geometry->GetGeometryRuntimeData().skinInstance.get();
        const auto* dismember = GetDismemberSkin(skin);
        logger::debug("[FaceDiag] {} skin ptr={:X} rtti={} skinData={:X} rootParent={:X} bones={:X} dismember={} partitions={}",
            a_prefix,
            reinterpret_cast<std::uintptr_t>(skin),
            SafeRTTIName(skin),
            skin ? reinterpret_cast<std::uintptr_t>(skin->skinData.get()) : 0,
            skin ? reinterpret_cast<std::uintptr_t>(skin->rootParent) : 0,
            skin ? reinterpret_cast<std::uintptr_t>(skin->bones) : 0,
            dismember != nullptr,
            dismember ? dismember->GetRuntimeData().numPartitions : 0);

        if (dismember && dismember->GetRuntimeData().partitions) {
            auto& runtime = dismember->GetRuntimeData();
            for (std::int32_t i = 0; i < runtime.numPartitions; ++i) {
                logger::debug("[FaceDiag] {} partition[{}] slot={} visible={}",
                    a_prefix,
                    i,
                    runtime.partitions[i].slot,
                    runtime.partitions[i].editorVisible);
            }
        }
    }

    void LogGeometryDiagnostics(RE::BSGeometry* a_geometry, std::uint32_t a_index)
    {
        if (!a_geometry) {
            return;
        }

        auto* dyn = netimmerse_cast<RE::BSDynamicTriShape*>(a_geometry);
        auto& modelData = a_geometry->GetModelData();
        const auto kind = GetFacePartKind(a_geometry);
        const bool baked = IsBakedExtraCandidate(a_geometry);
        const bool baseHead = IsBaseHeadGeometry(a_geometry);
        const std::string prefix = std::format("geom[{}] '{}'", a_index, SafeName(a_geometry->name));

        logger::debug("[FaceDiag] {} ptr={:X} rtti={} kind={} baseHead={} bakedExtra={} appCulled={} localT=({:.4f},{:.4f},{:.4f}) worldT=({:.4f},{:.4f},{:.4f}) boundCenter=({:.4f},{:.4f},{:.4f}) boundRadius={:.4f} vertexCount={}",
            prefix,
            reinterpret_cast<std::uintptr_t>(a_geometry),
            SafeRTTIName(a_geometry),
            kind.empty() ? "none" : kind,
            baseHead,
            baked,
            a_geometry->GetAppCulled(),
            a_geometry->local.translate.x,
            a_geometry->local.translate.y,
            a_geometry->local.translate.z,
            a_geometry->world.translate.x,
            a_geometry->world.translate.y,
            a_geometry->world.translate.z,
            modelData.modelBound.center.x,
            modelData.modelBound.center.y,
            modelData.modelBound.center.z,
            modelData.modelBound.radius,
            dyn ? dyn->GetTrishapeRuntimeData().vertexCount : 0);

        LogObjectExtraData(a_geometry, prefix);
        LogShaderProperty(a_geometry, prefix);
        LogSkinInfo(a_geometry, prefix);
    }
}

void Manager::DumpFaceDiagnostics(RE::Actor* a_actor, RE::TESNPC* a_npc, const std::string& a_context)
{
    logger::debug("[FaceDiag] ===== BEGIN {} =====", a_context);
    logger::debug("[FaceDiag] actor={:X} npc={:X} npcForm={:08X} name={} bodyTint=({}, {}, {}, {}) height={} weight={} race={:X} farSkin={:X} hairColor={:X} headParts={}",
        reinterpret_cast<std::uintptr_t>(a_actor),
        reinterpret_cast<std::uintptr_t>(a_npc),
        a_npc ? a_npc->GetFormID() : 0,
        a_npc && a_npc->GetFullName() ? a_npc->GetFullName() : "null",
        a_npc ? a_npc->bodyTintColor.red : 0,
        a_npc ? a_npc->bodyTintColor.green : 0,
        a_npc ? a_npc->bodyTintColor.blue : 0,
        a_npc ? a_npc->bodyTintColor.alpha : 0,
        a_npc ? a_npc->height : 0.0f,
        a_npc ? a_npc->weight : 0.0f,
        a_npc && a_npc->race ? reinterpret_cast<std::uintptr_t>(a_npc->race) : 0,
        a_npc && a_npc->farSkin ? reinterpret_cast<std::uintptr_t>(a_npc->farSkin) : 0,
        a_npc && a_npc->headRelatedData && a_npc->headRelatedData->hairColor ? reinterpret_cast<std::uintptr_t>(a_npc->headRelatedData->hairColor) : 0,
        a_npc ? static_cast<int>(a_npc->numHeadParts) : 0);

    if (a_npc && a_npc->headParts) {
        for (int i = 0; i < a_npc->numHeadParts; ++i) {
            auto* hp = a_npc->headParts[i];
            logger::debug("[FaceDiag] headPart[{}] ptr={:X} form={:08X} editorID={} type={} model={} extraParts={}",
                i,
                reinterpret_cast<std::uintptr_t>(hp),
                hp ? hp->GetFormID() : 0,
                hp && hp->GetFormEditorID() ? hp->GetFormEditorID() : "",
                hp ? hp->type.underlying() : 0,
                hp && hp->GetModel() ? hp->GetModel() : "",
                hp ? hp->extraParts.size() : 0);
        }
    }

    auto* actor3D = a_actor ? a_actor->Get3D(false) : nullptr;
    logger::debug("[FaceDiag] actor3D={:X} is3DLoaded={}", reinterpret_cast<std::uintptr_t>(actor3D), a_actor ? a_actor->Is3DLoaded() : false);
    if (!actor3D) {
        logger::debug("[FaceDiag] ===== END {} no actor3D =====", a_context);
        return;
    }

    auto* faceNode = netimmerse_cast<RE::BSFaceGenNiNode*>(actor3D->GetObjectByName("BSFaceGenNiNodeSkinned"));
    logger::debug("[FaceDiag] faceNode={:X} parent={:X} localT=({:.4f},{:.4f},{:.4f}) worldT=({:.4f},{:.4f},{:.4f}) appCulled={}",
        reinterpret_cast<std::uintptr_t>(faceNode),
        faceNode ? reinterpret_cast<std::uintptr_t>(faceNode->parent) : 0,
        faceNode ? faceNode->local.translate.x : 0.0f,
        faceNode ? faceNode->local.translate.y : 0.0f,
        faceNode ? faceNode->local.translate.z : 0.0f,
        faceNode ? faceNode->world.translate.x : 0.0f,
        faceNode ? faceNode->world.translate.y : 0.0f,
        faceNode ? faceNode->world.translate.z : 0.0f,
        faceNode ? faceNode->GetAppCulled() : false);
    if (!faceNode) {
        logger::debug("[FaceDiag] ===== END {} no faceNode =====", a_context);
        return;
    }

    LogObjectExtraData(faceNode, "faceNode");
    const auto& baseRotation = faceNode->GetRuntimeData().baseRotation;
    logger::debug("[FaceDiag] faceNode runtime animationData={:X} lastTime={} baseRotation=[{:.4f},{:.4f},{:.4f};{:.4f},{:.4f},{:.4f};{:.4f},{:.4f},{:.4f}]",
        reinterpret_cast<std::uintptr_t>(faceNode->GetRuntimeData().animationData.get()),
        faceNode->GetRuntimeData().lastTime,
        baseRotation.entry[0][0],
        baseRotation.entry[0][1],
        baseRotation.entry[0][2],
        baseRotation.entry[1][0],
        baseRotation.entry[1][1],
        baseRotation.entry[1][2],
        baseRotation.entry[2][0],
        baseRotation.entry[2][1],
        baseRotation.entry[2][2]);

    std::uint32_t geometryIndex = 0;
    RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_geometry) {
        LogGeometryDiagnostics(a_geometry, geometryIndex++);
        return RE::BSVisit::BSVisitControl::kContinue;
    });

    logger::debug("[FaceDiag] totalGeometries={}", geometryIndex);
    logger::debug("[FaceDiag] ===== END {} =====", a_context);
}

void Manager::ClearFaceGenGeometryIndex()
{
    _faceGenGeometryIndex.clear();
    _faceGenGeometryDuplicates = 0;
    logger::debug("[FaceGen Index] Cleared baked geometry index.");
}

void Manager::IndexFaceGenNif(const std::string& nifPath, RE::FormID originFormID)
{
    auto nifRoot = LoadNifFromFile(nifPath);
    if (!nifRoot) {
        logger::warn("[FaceGen Index] Failed to load FaceGen NIF for indexing: {}", nifPath);
        return;
    }

    auto* sourceFaceNode = netimmerse_cast<RE::NiNode*>(nifRoot->GetObjectByName("BSFaceGenNiNodeSkinned"));
    if (!sourceFaceNode) {
        logger::debug("[FaceGen Index] NIF has no BSFaceGenNiNodeSkinned, skipping: {}", nifPath);
        return;
    }

    std::size_t indexedInFile = 0;
    RE::BSVisit::TraverseScenegraphGeometries(sourceFaceNode, [&](RE::BSGeometry* a_geometry) {
        if (!IsBakedExtraCandidate(a_geometry)) {
            return RE::BSVisit::BSVisitControl::kContinue;
        }

        const std::string geometryName = a_geometry->name.c_str();
        const auto key = NormalizeGeometryKey(geometryName);
        if (_faceGenGeometryIndex.contains(key)) {
            ++_faceGenGeometryDuplicates;
            logger::debug("[FaceGen Index] Duplicate baked geometry '{}' ignored from '{}'; first source is '{}'.",
                geometryName, nifPath, _faceGenGeometryIndex[key].nifPath);
            return RE::BSVisit::BSVisitControl::kContinue;
        }

        _faceGenGeometryIndex.emplace(key, FaceGenGeometrySource{
            nifPath,
            geometryName,
            originFormID,
            GetBakedExtraKind(a_geometry)
        });
        ++indexedInFile;
        return RE::BSVisit::BSVisitControl::kContinue;
    });

    logger::debug("[FaceGen Index] Indexed {} baked geometries from '{}'.", indexedInFile, nifPath);
}

bool Manager::FindIndexedFaceGenGeometry(const std::string& geometryName, FaceGenGeometrySource& outSource) const
{
    const auto it = _faceGenGeometryIndex.find(NormalizeGeometryKey(geometryName));
    if (it == _faceGenGeometryIndex.end()) {
        return false;
    }

    outSource = it->second;
    return true;
}

std::size_t Manager::GetFaceGenGeometryIndexSize() const
{
    return _faceGenGeometryIndex.size();
}

std::size_t Manager::GetFaceGenGeometryDuplicateCount() const
{
    return _faceGenGeometryDuplicates;
}

void Manager::DeformFaceToMatchNif(RE::Actor* a_actor, const std::string& a_nifPath) {
    logger::debug("[Face Swap] === Start ===");

    if (!a_actor || !a_actor->Get3D(false)) {
        logger::error("[Face Swap] Ator invalido ou sem modelo 3D.");
        return;
    }

    RE::NiAVObject* actor3D = a_actor->Get3D(false);
    logger::debug("[Face Swap] Actor 3D encontrado: {:X}", reinterpret_cast<std::uintptr_t>(actor3D));

    auto faceNode = netimmerse_cast<RE::BSFaceGenNiNode*>(actor3D->GetObjectByName("BSFaceGenNiNodeSkinned"));
    logger::debug("[Face Swap] Face node runtime: {:X}", reinterpret_cast<std::uintptr_t>(faceNode));
    if (!faceNode || !faceNode->parent) {
        logger::error("[Face Swap] Ator nao possui BSFaceGenNiNodeSkinned.");
        return;
    }

    auto* npc = a_actor->GetActorBase() ? a_actor->GetActorBase()->As<RE::TESNPC>() : nullptr;
    const auto appliedHeadParts = GetAppliedHeadPartInfo(npc);
    logger::debug("[Face Swap] HeadParts aplicadas no NPC para baked extras: all={} primary={} extrasComOwner={}",
        appliedHeadParts.allEditorIDs.size(),
        appliedHeadParts.primaryEditorIDs.size(),
        appliedHeadParts.extraOwnerByID.size());
    DumpFaceDiagnostics(a_actor, npc, "Face Swap Before");

    logger::debug("[Face Swap] Carregando custom NIF: {}", a_nifPath);
    auto nifRoot = LoadNifFromFile(a_nifPath);
    logger::debug("[Face Swap] NIF root carregado: {:X}", reinterpret_cast<std::uintptr_t>(nifRoot.get()));
    if (!nifRoot) {
        logger::error("[Face Swap] Falha ao carregar a raiz do NIF novo.");
        return;
    }

    logger::debug("[Face Swap] Procurando NiNode BSFaceGenNiNodeSkinned na custom NIF...");
    auto sourceFaceNode = netimmerse_cast<RE::NiNode*>(nifRoot->GetObjectByName("BSFaceGenNiNodeSkinned"));
    logger::debug("[Face Swap] Source face node: {:X}", reinterpret_cast<std::uintptr_t>(sourceFaceNode));
    if (!sourceFaceNode) {
        logger::error("[Face Swap] Custom NIF nao possui NiNode BSFaceGenNiNodeSkinned valido: {}", a_nifPath);
        return;
    }

    logger::debug("[FaceDiag] ===== BEGIN Custom NIF Source {} =====", a_nifPath);
    LogObjectExtraData(sourceFaceNode, "customSourceFaceNode");
    std::uint32_t sourceDiagIndex = 0;
    RE::BSVisit::TraverseScenegraphGeometries(sourceFaceNode, [&](RE::BSGeometry* a_geometry) {
        LogGeometryDiagnostics(a_geometry, sourceDiagIndex++);
        return RE::BSVisit::BSVisitControl::kContinue;
    });
    logger::debug("[FaceDiag] customSource totalGeometries={}", sourceDiagIndex);
    logger::debug("[FaceDiag] ===== END Custom NIF Source {} =====", a_nifPath);

    std::vector<RE::NiPointer<RE::BSGeometry>> sourceAllGeometries;
    std::vector<RE::NiPointer<RE::BSGeometry>> sourceGeometries;
    logger::debug("[Face Swap] Coletando geometrias da custom NIF...");
    RE::BSVisit::TraverseScenegraphGeometries(sourceFaceNode, [&](RE::BSGeometry* a_geometry) {
        if (a_geometry) {
            sourceAllGeometries.emplace_back(a_geometry);
        }

        const auto kind = GetFacePartKind(a_geometry);
        if (!kind.empty()) {
            logger::debug("[Face Swap] Source geometry selecionada [{}]: '{}' {:X}",
                kind,
                a_geometry && !a_geometry->name.empty() ? a_geometry->name.c_str() : "SemNome",
                reinterpret_cast<std::uintptr_t>(a_geometry));
            sourceGeometries.emplace_back(a_geometry);
        } else if (a_geometry) {
            logger::debug("[Face Swap] Source geometry ignorada: '{}'",
                a_geometry->name.empty() ? "SemNome" : a_geometry->name.c_str());
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });
    logger::debug("[Face Swap] Geometrias faciais selecionadas na custom NIF: {}", sourceGeometries.size());

    if (sourceGeometries.empty()) {
        logger::error("[Face Swap] Custom NIF nao possui geometrias de face compativeis: {}", a_nifPath);
        return;
    }

    auto sourceHeadIt = std::find_if(sourceGeometries.begin(), sourceGeometries.end(), [](const auto& a_geometry) {
        return IsBaseHeadGeometry(a_geometry.get());
    });
    if (sourceHeadIt == sourceGeometries.end()) {
        logger::error("[Face Swap] Custom NIF nao possui geometria base de head/face.");
        return;
    }

    std::vector<RE::NiPointer<RE::BSGeometry>> targetHeadGeometries;
    RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_geometry) {
        if (IsBaseHeadGeometry(a_geometry)) {
            logger::debug("[Face Swap] Target head geometry selecionada: '{}'",
                a_geometry && !a_geometry->name.empty() ? a_geometry->name.c_str() : "SemNome");
            targetHeadGeometries.emplace_back(a_geometry);
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });

    if (targetHeadGeometries.empty()) {
        logger::error("[Face Swap] Ator nao possui geometria base de head/face para receber morph.");
        return;
    }

    auto* targetHeadGeometry = targetHeadGeometries.front().get();
    NeckSeamPatch neckSeamPatch;
    if (CopyVerticesToRuntimeGeometry(targetHeadGeometry, sourceHeadIt->get(), "head", &neckSeamPatch)) {
        std::size_t adjustedParts = 0;
        std::unordered_set<RE::BSGeometry*> consumedSources;
        consumedSources.insert(sourceHeadIt->get());

        RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_targetGeometry) {
            const auto targetKind = GetFacePartKind(a_targetGeometry);
            if (targetKind.empty() || targetKind == "head") {
                return RE::BSVisit::BSVisitControl::kContinue;
            }

            const auto sourceIt = std::find_if(sourceGeometries.begin(), sourceGeometries.end(), [&](const auto& a_sourceGeometry) {
                return GetFacePartKind(a_sourceGeometry.get()) == targetKind;
            });
            if (sourceIt == sourceGeometries.end()) {
                return RE::BSVisit::BSVisitControl::kContinue;
            }

            if (CopyVerticesToRuntimeGeometry(a_targetGeometry, sourceIt->get(), targetKind)) {
                logger::debug("[Face Swap] {} reposicionado: '{}' -> '{}'",
                    targetKind,
                    (*sourceIt)->name.empty() ? "SemNome" : (*sourceIt)->name.c_str(),
                    a_targetGeometry->name.empty() ? "SemNome" : a_targetGeometry->name.c_str());
                ++adjustedParts;
                consumedSources.insert(sourceIt->get());
            }

            return RE::BSVisit::BSVisitControl::kContinue;
        });

        std::vector<ClonedFaceGeometry> bakedExtras;
        std::unordered_set<std::string> sourceBakedExtraNames;
        std::unordered_set<std::string> attachedBakedExtraNames;
        auto* runtimeHeadMaterialSource = targetHeadGeometries.front().get();
        auto cloneBakedExtra = [&](RE::BSGeometry* source, RE::NiAVObject* sourceRoot, RE::NiNode* defaultTargetParent, const std::string& sourceLabel) {
            if (!source || source->name.empty()) {
                return false;
            }

            RE::NiPointer<RE::NiObject> clonedObject(source->Clone());
            auto* clonedGeometry = clonedObject ? clonedObject->AsGeometry() : nullptr;
            if (!clonedGeometry) {
                logger::warn("[Face Swap] Falha ao clonar baked extra '{}' de {}.", source->name.c_str(), sourceLabel);
                return false;
            }

            RE::NiPointer<RE::BSGeometry> clone(clonedGeometry);
            if (ShouldUseRuntimeHeadMaterial(source)) {
                CopyRuntimeShaderProperty(clone.get(), runtimeHeadMaterialSource, source->name.c_str());
            }

            if (!RebindSkinToTargetSkeleton(source, clone.get(), sourceRoot, actor3D)) {
                logger::warn("[Face Swap] Baked extra '{}' de {} ignorado por skin/bones incompativeis.",
                    source->name.c_str(), sourceLabel);
                return false;
            }

            auto* mappedParent = MapObjectByName(source->parent, sourceRoot, faceNode);
            auto* targetParent = mappedParent ? mappedParent->AsNode() : nullptr;
            if (!targetParent) {
                targetParent = defaultTargetParent ? defaultTargetParent : faceNode;
            }

            clone->local = source->local;
            clone->SetUserData(a_actor);
            clone->SetAppCulled(false);
            attachedBakedExtraNames.insert(NormalizeGeometryKey(source->name.c_str()));
            bakedExtras.push_back({ std::move(clone), targetParent });
            return true;
        };

        for (const auto& source : sourceAllGeometries) {
            if (!source || consumedSources.contains(source.get()) || !IsBakedExtraCandidate(source.get())) {
                continue;
            }

            const auto appliedSourceHeadPartID = FindAppliedHeadPartIDForName(source->name.c_str(), appliedHeadParts);
            if (appliedSourceHeadPartID.empty()) {
                logger::debug("[Face Swap] Baked extra '{}' ignorado: nao corresponde a HeadPart aplicada no NPC.",
                    source->name.empty() ? "SemNome" : source->name.c_str());
                continue;
            }

            if (IsExtraHeadPartSatisfiedByOwner(appliedSourceHeadPartID, appliedHeadParts, faceNode)) {
                logger::debug("[Face Swap] Baked extra '{}' ignorado: eh extraPart de '{}' e o owner ja possui geometria runtime.",
                    appliedSourceHeadPartID,
                    GetExtraHeadPartOwner(appliedSourceHeadPartID, appliedHeadParts));
                attachedBakedExtraNames.insert(NormalizeGeometryKey(appliedSourceHeadPartID));
                continue;
            }

            sourceBakedExtraNames.insert(NormalizeGeometryKey(source->name.c_str()));

            if (HasRuntimeGeometryWithSameName(faceNode, source.get())) {
                logger::debug("[Face Swap] Baked extra '{}' ignorado: runtime ja possui geometria com mesmo nome.",
                    source->name.empty() ? "SemNome" : source->name.c_str());
                attachedBakedExtraNames.insert(NormalizeGeometryKey(source->name.c_str()));
                continue;
            }

            cloneBakedExtra(source.get(), sourceFaceNode, faceNode, a_nifPath);
        }

        std::unordered_map<std::string, RE::NiPointer<RE::NiAVObject>> indexedNifCache;
        for (const auto& appliedHeadPartID : appliedHeadParts.allEditorIDs) {
            if (appliedHeadPartID.empty()) {
                continue;
            }

            const auto headPartKey = NormalizeGeometryKey(appliedHeadPartID);
            if (attachedBakedExtraNames.contains(headPartKey)) {
                continue;
            }

            if (IsExtraHeadPartSatisfiedByOwner(appliedHeadPartID, appliedHeadParts, faceNode)) {
                logger::debug("[Face Swap] HeadPart extra '{}' nao sera buscada no indice: owner '{}' ja possui geometria runtime.",
                    appliedHeadPartID,
                    GetExtraHeadPartOwner(appliedHeadPartID, appliedHeadParts));
                attachedBakedExtraNames.insert(headPartKey);
                continue;
            }

            if (HasRuntimeGeometryWithName(faceNode, appliedHeadPartID)) {
                logger::debug("[Face Swap] HeadPart '{}' ja possui geometria runtime; indice nao sera usado.", appliedHeadPartID);
                continue;
            }

            if (sourceBakedExtraNames.contains(headPartKey) || FindGeometryByName(sourceFaceNode, appliedHeadPartID)) {
                logger::debug("[Face Swap] HeadPart '{}' existe na NIF custom atual; indice externo nao sera usado.", appliedHeadPartID);
                continue;
            }

            FaceGenGeometrySource indexedSource;
            if (!Manager::GetSingleton()->FindIndexedFaceGenGeometry(appliedHeadPartID, indexedSource)) {
                logger::debug("[Face Swap] HeadPart '{}' nao possui geometria runtime nem fonte no indice FaceGen.", appliedHeadPartID);
                continue;
            }

            logger::debug("[Face Swap] HeadPart '{}' sem geometria runtime; usando FaceGen indexada '{}' ({})",
                appliedHeadPartID, indexedSource.nifPath, indexedSource.kind);

            auto cacheIt = indexedNifCache.find(indexedSource.nifPath);
            if (cacheIt == indexedNifCache.end()) {
                auto indexedRoot = LoadNifFromFile(indexedSource.nifPath);
                cacheIt = indexedNifCache.emplace(indexedSource.nifPath, std::move(indexedRoot)).first;
            }

            auto* indexedRoot = cacheIt->second.get();
            auto* indexedFaceNode = indexedRoot ? netimmerse_cast<RE::NiNode*>(indexedRoot->GetObjectByName("BSFaceGenNiNodeSkinned")) : nullptr;
            if (!indexedFaceNode) {
                logger::warn("[Face Swap] Fonte indexada '{}' nao carregou BSFaceGenNiNodeSkinned para '{}'.",
                    indexedSource.nifPath, appliedHeadPartID);
                continue;
            }

            auto* indexedGeometry = FindGeometryByName(indexedFaceNode, indexedSource.geometryName);
            if (!indexedGeometry) {
                logger::warn("[Face Swap] Fonte indexada '{}' nao contem mais a geometria '{}'.",
                    indexedSource.nifPath, indexedSource.geometryName);
                continue;
            }

            if (cloneBakedExtra(indexedGeometry, indexedFaceNode, faceNode, indexedSource.nifPath)) {
                logger::debug("[Face Swap] Baked HeadPart '{}' anexada a partir do indice.", appliedHeadPartID);
            }
        }

        for (auto& item : bakedExtras) {
            if (item.parent && item.geometry) {
                item.parent->AttachChild(item.geometry.get());
                logger::debug("[Face Swap] Baked extra anexado: '{}'",
                    item.geometry->name.empty() ? "SemNome" : item.geometry->name.c_str());
            }
        }

        RE::NiUpdateData updateData{};
        updateData.time = faceNode->GetRuntimeData().lastTime;
        logger::debug("[Face Swap] Runtime update begin: faceNode={:X} actor3D={:X}",
            reinterpret_cast<std::uintptr_t>(faceNode),
            reinterpret_cast<std::uintptr_t>(actor3D));
        logger::debug("[Face Swap] Calling faceNode->UpdateWorldData...");
        faceNode->UpdateWorldData(&updateData);
        logger::debug("[Face Swap] UpdateWorldData OK. Calling UpdateFaceNodeBounds...");
        UpdateFaceNodeBounds(faceNode);
        logger::debug("[Face Swap] UpdateFaceNodeBounds OK. Calling FixSkinInstances...");
        faceNode->FixSkinInstances(actor3D->AsNode(), false);
        logger::debug("[Face Swap] FixSkinInstances OK. QueueUpdateNiObject...");
        if (auto* queue = RE::TaskQueueInterface::GetSingleton()) {
            queue->QueueUpdateNiObject(actor3D);
            logger::debug("[Face Swap] QueueUpdateNiObject OK.");
        } else {
            logger::warn("[Face Swap] TaskQueueInterface ausente para QueueUpdateNiObject.");
        }

        logger::debug("[Face Swap] Calling actor->UpdateSkinColor...");
        a_actor->UpdateSkinColor();
        logger::debug("[Face Swap] UpdateSkinColor OK. Calling actor->UpdateHairColor...");
        a_actor->UpdateHairColor();
        logger::debug("[Face Swap] UpdateHairColor OK.");
        if (npc) {
            logger::debug("[Face Swap] Calling npc->UpdateNeck faceNode={:X} npc={:X}...", reinterpret_cast<std::uintptr_t>(faceNode), reinterpret_cast<std::uintptr_t>(npc));
            npc->UpdateNeck(faceNode);
            logger::debug("[Face Swap] UpdateNeck OK.");
        } else {
            logger::warn("[Face Swap] UpdateNeck pulado: npc null.");
        }

        if (!neckSeamPatch.empty()) {
            logger::debug("[Face Swap] Applying immediate neck seam patch...");
            ApplyNeckSeamPatchToGeometry(targetHeadGeometry, neckSeamPatch, "post UpdateNeck imediato");

            const auto actorID = a_actor->GetFormID();
            const std::string targetHeadName = targetHeadGeometry && !targetHeadGeometry->name.empty() ? targetHeadGeometry->name.c_str() : "";
            auto delayedPatch = neckSeamPatch;
            auto scheduleDelayedNeckPatch = [actorID, targetHeadName, delayedPatch](std::uint32_t delayMs) {
                Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(delayMs), [actorID, targetHeadName, delayedPatch, delayMs]() {
                    auto* taskInterface = SKSE::GetTaskInterface();
                    if (!taskInterface) {
                        logger::warn("[Face Swap] Neck seam patch atrasado {}ms nao foi enfileirado: TaskInterface ausente.", delayMs);
                        return;
                    }

                    taskInterface->AddTask([actorID, targetHeadName, delayedPatch, delayMs]() {
                        logger::debug("[Face Swap] Neck seam delayed task BEGIN actor={:08X} delayMs={} targetHead='{}'", actorID, delayMs, targetHeadName);
                        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(actorID);
                        auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
                        auto* actor3D = actor ? actor->Get3D(false) : nullptr;
                        if (!actor || !actor3D) {
                            logger::debug("[Face Swap] Neck seam patch atrasado ignorado: ator/3D ausente {:08X} delayMs={}.", actorID, delayMs);
                            return;
                        }

                        auto* faceNode = netimmerse_cast<RE::BSFaceGenNiNode*>(actor3D->GetObjectByName("BSFaceGenNiNodeSkinned"));
                        if (!faceNode) {
                            logger::debug("[Face Swap] Neck seam patch atrasado ignorado: face node ausente {:08X} delayMs={}.", actorID, delayMs);
                            return;
                        }

                        RE::BSGeometry* delayedHead = nullptr;
                        if (!targetHeadName.empty()) {
                            delayedHead = FindGeometryByName(faceNode, targetHeadName);
                        }
                        if (!delayedHead) {
                            RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_geometry) {
                                if (IsBaseHeadGeometry(a_geometry)) {
                                    delayedHead = a_geometry;
                                    return RE::BSVisit::BSVisitControl::kStop;
                                }
                                return RE::BSVisit::BSVisitControl::kContinue;
                            });
                        }

                        if (ApplyNeckSeamPatchToGeometry(delayedHead, delayedPatch, std::format("task atrasado {}ms", delayMs))) {
                            RE::NiUpdateData updateData{};
                            updateData.time = faceNode->GetRuntimeData().lastTime;
                            faceNode->UpdateWorldData(&updateData);
                            UpdateFaceNodeBounds(faceNode);
                            if (auto* queue = RE::TaskQueueInterface::GetSingleton()) {
                                queue->QueueUpdateNiObject(actor3D);
                                logger::debug("[Face Swap] Neck seam delayed QueueUpdateNiObject OK actor={:08X} delayMs={}.", actorID, delayMs);
                            } else {
                                logger::warn("[Face Swap] Neck seam delayed sem TaskQueueInterface actor={:08X} delayMs={}.", actorID, delayMs);
                            }
                        }
                        logger::debug("[Face Swap] Neck seam delayed task END actor={:08X} delayMs={}.", actorID, delayMs);
                    });
                });
            };

            scheduleDelayedNeckPatch(60);
            scheduleDelayedNeckPatch(180);
            scheduleDelayedNeckPatch(360);
        }

        logger::debug("[Face Swap] Head morph aplicado na geometria original do ator: '{}' -> '{}'.",
            (*sourceHeadIt)->name.empty() ? "SemNome" : (*sourceHeadIt)->name.c_str(),
            targetHeadGeometry->name.empty() ? "SemNome" : targetHeadGeometry->name.c_str());
        logger::debug("[Face Swap] Partes faciais reposicionadas: {}", adjustedParts);
        logger::debug("[Face Swap] Baked extras anexados: {}", bakedExtras.size());
        DumpFaceDiagnostics(a_actor, npc, "Face Swap After Main");
        logger::debug("[Face Swap] === Concluida! ===");
        return;
    }

    logger::warn("[Face Swap] Head morph direto falhou; usando clone fallback experimental.");

    std::vector<ClonedFaceGeometry> pending;
    std::unordered_set<std::string> replacementNames;
    std::unordered_set<std::uint16_t> replacementSlots;
    NeckSeamPatch fallbackNeckSeamPatch;
    std::string fallbackNeckHeadName;
    RE::BSGeometry* fallbackRuntimeHeadMaterialSource = nullptr;
    RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_geometry) {
        if (!fallbackRuntimeHeadMaterialSource && IsBaseHeadGeometry(a_geometry)) {
            fallbackRuntimeHeadMaterialSource = a_geometry;
            return RE::BSVisit::BSVisitControl::kStop;
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });

    for (const auto& source : sourceGeometries) {
        if (!source) {
            continue;
        }

        logger::debug("[Face Swap] Clonando head geometry '{}'.", source->name.empty() ? "SemNome" : source->name.c_str());
        RE::NiPointer<RE::NiObject> clonedObject(source->Clone());
        auto* clonedGeometry = clonedObject ? clonedObject->AsGeometry() : nullptr;
        if (!clonedGeometry) {
            logger::warn("[Face Swap] Falha ao clonar geometria '{}'.", source->name.c_str());
            continue;
        }

        RE::NiPointer<RE::BSGeometry> clone(clonedGeometry);
        if (fallbackRuntimeHeadMaterialSource && ShouldUseRuntimeHeadMaterial(source.get())) {
            CopyRuntimeShaderProperty(clone.get(), fallbackRuntimeHeadMaterialSource, source->name.c_str());
        }

        if (!RebindSkinToTargetSkeleton(source.get(), clone.get(), sourceFaceNode, actor3D)) {
            logger::warn("[Face Swap] Geometria '{}' ignorada por skin/bones incompativeis.", source->name.c_str());
            continue;
        }

        auto* mappedParent = MapObjectByName(source->parent, sourceFaceNode, faceNode);
        auto* targetParent = mappedParent ? mappedParent->AsNode() : nullptr;
        if (!targetParent) {
            targetParent = faceNode;
        }

        clone->local = source->local;
        clone->SetUserData(a_actor);
        clone->SetAppCulled(false);
        if (fallbackRuntimeHeadMaterialSource && IsBaseHeadGeometry(clone.get())) {
            NeckSeamPatch candidatePatch;
            if (BuildMismatchedTopologyNeckPatch(fallbackRuntimeHeadMaterialSource, clone.get(), candidatePatch)) {
                fallbackNeckSeamPatch = std::move(candidatePatch);
                fallbackNeckHeadName = clone->name.empty() ? "" : clone->name.c_str();
                ApplyNeckSeamPatchToGeometry(clone.get(), fallbackNeckSeamPatch, "fallback pre-attach");
            }
        }
        RememberReplacementGeometry(clone.get(), replacementNames, replacementSlots);
        pending.push_back({ std::move(clone), targetParent });
    }

    if (pending.empty()) {
        logger::error("[Face Swap] Nenhuma geometria da custom NIF pode ser clonada/rebindada.");
        return;
    }

    std::size_t hiddenOriginals = 0;
    RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_geometry) {
        if (ShouldHideOriginalGeometry(a_geometry, replacementNames)) {
            a_geometry->SetAppCulled(true);
            ++hiddenOriginals;
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });

    for (auto& item : pending) {
        if (item.parent && item.geometry) {
            item.parent->AttachChild(item.geometry.get());
        }
    }

    logger::debug("[Face Swap] Clones anexados; pulando reset de animationData neste teste.");

    RE::NiUpdateData updateData{};
    updateData.time = faceNode->GetRuntimeData().lastTime;
    logger::debug("[Face Swap] Atualizando world data/bounds...");
    logger::debug("[Face Swap] Fallback calling UpdateWorldData...");
    faceNode->UpdateWorldData(&updateData);
    logger::debug("[Face Swap] Fallback UpdateWorldData OK. Calling UpdateFaceNodeBounds...");
    UpdateFaceNodeBounds(faceNode);
    logger::debug("[Face Swap] Fallback UpdateFaceNodeBounds OK.");
    logger::debug("[Face Swap] FixSkinInstances...");
    faceNode->FixSkinInstances(actor3D->AsNode(), false);
    logger::debug("[Face Swap] Fallback FixSkinInstances OK.");
    if (auto* queue = RE::TaskQueueInterface::GetSingleton()) {
        logger::debug("[Face Swap] QueueUpdateNiObject..");
        queue->QueueUpdateNiObject(actor3D);
        logger::debug("[Face Swap] Fallback QueueUpdateNiObject OK.");
    } else {
        logger::warn("[Face Swap] Fallback TaskQueueInterface ausente.");
    }

    logger::debug("[Face Swap] Fallback calling UpdateSkinColor...");
    a_actor->UpdateSkinColor();
    logger::debug("[Face Swap] Fallback UpdateSkinColor OK. Calling UpdateHairColor...");
    a_actor->UpdateHairColor();
    logger::debug("[Face Swap] Fallback UpdateHairColor OK.");
    if (auto npcBase = a_actor->GetActorBase()) {
        if (auto npcForNeck = npcBase->As<RE::TESNPC>()) {
            logger::debug("[Face Swap] UpdateNeck...");
            npcForNeck->UpdateNeck(faceNode);
            logger::debug("[Face Swap] Fallback UpdateNeck OK.");
        } else {
            logger::warn("[Face Swap] Fallback UpdateNeck pulado: actor base nao eh TESNPC.");
        }
    } else {
        logger::warn("[Face Swap] Fallback UpdateNeck pulado: actor base null.");
    }

    if (!fallbackNeckSeamPatch.empty()) {
        RE::BSGeometry* fallbackHead = nullptr;
        if (!fallbackNeckHeadName.empty()) {
            fallbackHead = FindGeometryByName(faceNode, fallbackNeckHeadName);
        }
        if (!fallbackHead) {
            RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_geometry) {
                if (IsBaseHeadGeometry(a_geometry) && !a_geometry->GetAppCulled()) {
                    fallbackHead = a_geometry;
                    return RE::BSVisit::BSVisitControl::kStop;
                }
                return RE::BSVisit::BSVisitControl::kContinue;
            });
        }

        ApplyNeckSeamPatchToGeometry(fallbackHead, fallbackNeckSeamPatch, "fallback post UpdateNeck");
        if (auto* queue = RE::TaskQueueInterface::GetSingleton()) {
            queue->QueueUpdateNiObject(actor3D);
        }

        const auto actorID = a_actor->GetFormID();
        const auto delayedPatch = fallbackNeckSeamPatch;
        const auto targetHeadName = fallbackNeckHeadName;
        auto scheduleFallbackDelayedNeckPatch = [actorID, targetHeadName, delayedPatch](std::uint32_t delayMs) {
            Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(delayMs), [actorID, targetHeadName, delayedPatch, delayMs]() {
                auto* taskInterface = SKSE::GetTaskInterface();
                if (!taskInterface) {
                    logger::warn("[Face Swap] Fallback neck patch atrasado {}ms nao foi enfileirado: TaskInterface ausente.", delayMs);
                    return;
                }

                taskInterface->AddTask([actorID, targetHeadName, delayedPatch, delayMs]() {
                    logger::debug("[Face Swap] Fallback neck delayed task BEGIN actor={:08X} delayMs={} targetHead='{}'",
                        actorID,
                        delayMs,
                        targetHeadName);
                    auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(actorID);
                    auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
                    auto* actor3D = actor ? actor->Get3D(false) : nullptr;
                    if (!actor || !actor3D) {
                        logger::debug("[Face Swap] Fallback neck patch atrasado ignorado: ator/3D ausente {:08X} delayMs={}.", actorID, delayMs);
                        return;
                    }

                    auto* faceNode = netimmerse_cast<RE::BSFaceGenNiNode*>(actor3D->GetObjectByName("BSFaceGenNiNodeSkinned"));
                    if (!faceNode) {
                        logger::debug("[Face Swap] Fallback neck patch atrasado ignorado: face node ausente {:08X} delayMs={}.", actorID, delayMs);
                        return;
                    }

                    RE::BSGeometry* delayedHead = nullptr;
                    if (!targetHeadName.empty()) {
                        delayedHead = FindGeometryByName(faceNode, targetHeadName);
                    }
                    if (!delayedHead) {
                        RE::BSVisit::TraverseScenegraphGeometries(faceNode, [&](RE::BSGeometry* a_geometry) {
                            if (IsBaseHeadGeometry(a_geometry) && !a_geometry->GetAppCulled()) {
                                delayedHead = a_geometry;
                                return RE::BSVisit::BSVisitControl::kStop;
                            }
                            return RE::BSVisit::BSVisitControl::kContinue;
                        });
                    }

                    if (ApplyNeckSeamPatchToGeometry(delayedHead, delayedPatch, std::format("fallback task atrasado {}ms", delayMs))) {
                        RE::NiUpdateData updateData{};
                        updateData.time = faceNode->GetRuntimeData().lastTime;
                        faceNode->UpdateWorldData(&updateData);
                        UpdateFaceNodeBounds(faceNode);
                        if (auto* queue = RE::TaskQueueInterface::GetSingleton()) {
                            queue->QueueUpdateNiObject(actor3D);
                            logger::debug("[Face Swap] Fallback neck delayed QueueUpdateNiObject OK actor={:08X} delayMs={}.", actorID, delayMs);
                        }
                    }
                    logger::debug("[Face Swap] Fallback neck delayed task END actor={:08X} delayMs={}.", actorID, delayMs);
                });
            });
        };

        scheduleFallbackDelayedNeckPatch(60);
        scheduleFallbackDelayedNeckPatch(180);
        scheduleFallbackDelayedNeckPatch(420);
    }

    logger::debug("[Face Swap] Geometrias da custom NIF clonadas no face node original: {} anexadas, {} originais ocultas.",
        pending.size(), hiddenOriginals);
    DumpFaceDiagnostics(a_actor, a_actor->GetActorBase() ? a_actor->GetActorBase()->As<RE::TESNPC>() : nullptr, "Face Swap After Fallback");
    logger::debug("[Face Swap] === Concluida! ===");
}
void Manager::ScheduleFaceDeform(RE::FormID actorID, const std::string& nifPath, int retries) {
    logger::debug("[ScheduleFaceDeform] Request actor={:08X} retries={} nif='{}'", actorID, retries, nifPath);
    if (retries <= 0) {
        logger::error("[Swap & Deform] Esgotaram as tentativas esperando o 3D do Ator {:08X} recarregar.", actorID);
        return;
    }
    if (nifPath.empty()) {
        logger::warn("[ScheduleFaceDeform] Ignorado actor={:08X}: nifPath vazio.", actorID);
        return;
    }

    Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(25), [actorID, nifPath, retries]() {
        logger::debug("[ScheduleFaceDeform] Delayed task wake actor={:08X} retries={}", actorID, retries);
        auto* taskInterface = SKSE::GetTaskInterface();
        if (!taskInterface) {
            logger::error("[ScheduleFaceDeform] TaskInterface ausente actor={:08X}; deformacao nao sera aplicada.", actorID);
            return;
        }

        taskInterface->AddTask([actorID, nifPath, retries]() {
            logger::debug("[ScheduleFaceDeform] Task begin actor={:08X} retries={}", actorID, retries);
            auto pActor = RE::TESForm::LookupByID<RE::Actor>(actorID);
            logger::debug("[ScheduleFaceDeform] Lookup actor={:08X} ptr={:X}", actorID, reinterpret_cast<std::uintptr_t>(pActor));

            if (pActor && pActor->Is3DLoaded() && pActor->GetFaceNodeSkinned()) {
                logger::debug("[ScheduleFaceDeform] 3D pronto actor={:08X} actorPtr={:X} faceNode={:X}; chamando DeformFaceToMatchNif.",
                    actorID,
                    reinterpret_cast<std::uintptr_t>(pActor),
                    reinterpret_cast<std::uintptr_t>(pActor->GetFaceNodeSkinned()));
                try {
                    Manager::DeformFaceToMatchNif(pActor, nifPath);
                    logger::debug("[ScheduleFaceDeform] DeformFaceToMatchNif retornou OK actor={:08X}.", actorID);
                } catch (const std::exception& e) {
                    logger::error("[ScheduleFaceDeform] EXCEPTION actor={:08X}: {}", actorID, e.what());
                } catch (...) {
                    logger::error("[ScheduleFaceDeform] UNKNOWN EXCEPTION actor={:08X}.", actorID);
                }
            }
            else {
                // Tenta novamente
                logger::debug("[ScheduleFaceDeform] Actor ainda nao pronto actor={:08X} ptr={:X} loaded={} faceNode={:X} retriesLeft={}",
                    actorID,
                    reinterpret_cast<std::uintptr_t>(pActor),
                    pActor ? pActor->Is3DLoaded() : false,
                    pActor ? reinterpret_cast<std::uintptr_t>(pActor->GetFaceNodeSkinned()) : 0,
                    retries - 1);
                ScheduleFaceDeform(actorID, nifPath, retries - 1);
            }
            logger::debug("[ScheduleFaceDeform] Task end actor={:08X} retries={}", actorID, retries);
            });
        });
}

void Manager::RegisterAffectedNPC(RE::FormID baseID, const std::string& nifPath) {
    if (nifPath.empty()) {
        logger::debug("[AffectedNPC] Register vazio tratado como Unregister base={:08X}", baseID);
        UnregisterAffectedNPC(baseID);
        return;
    }

    logger::debug("[AffectedNPC] Register base={:08X} nif='{}'", baseID, nifPath);
    _affectedNPCs[baseID] = nifPath;
}

void Manager::UnregisterAffectedNPC(RE::FormID baseID) {
    logger::debug("[AffectedNPC] Unregister base={:08X}", baseID);
    _affectedNPCs.erase(baseID);
}

void Manager::ClearAffectedNPCs() {
    logger::debug("[AffectedNPC] Clear all affected NPCs: count={}", _affectedNPCs.size());
    _affectedNPCs.clear();
}

bool Manager::IsNPCAffected(RE::FormID baseID, std::string& outNifPath) {
    auto it = _affectedNPCs.find(baseID);
    if (it != _affectedNPCs.end()) {
        outNifPath = it->second;
        return true;
    }
    return false;
}
