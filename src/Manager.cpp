#include "Manager.h"

#include <array>
#include <cctype>
#include <limits>
#include <unordered_map>
#include <unordered_set>

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

    // Fun��o auxiliar para reverter string para FormID no load do JSON
    RE::FormID FormIDFromString(const std::string& str) {
        auto pos = str.find('|');
        if (pos != std::string::npos) {
            std::string plugin = str.substr(0, pos);
            std::string idStr = str.substr(pos + 1);
            RE::FormID localId = std::stoul(idStr, nullptr, 16);
            auto dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler ? dataHandler->LookupFormID(localId, plugin) : 0;
        }
        return str.empty() ? 0 : std::stoul(str, nullptr, 16);
    }
}

void Manager::ApplyNPCCustomizationFromJSON(RE::TESNPC* npc, const rapidjson::Document& doc) {
    if (!npc) {
        logger::error("[ApplyJSON] Falha: Ponteiro de NPC nulo recebido.");
        return;
    }

    std::string npcName = npc->GetFullName() ? npc->GetFullName() : "Unnamed";
    logger::info("[ApplyJSON] === Iniciando aplicacao para NPC: {} [{:08X}] ===", npcName, npc->GetFormID());

    try {
        npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kIsChargenFacePreset);

        // 1. Atributos B�sicos
        logger::info("[ApplyJSON] [{:08X}] Passo 1: Aplicando atributos basicos...", npc->GetFormID());
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

        // 2. Associa��es de IDs
        logger::info("[ApplyJSON] [{:08X}] Passo 2: Resolvendo FormIDs de Race, Skin e Outfits...", npc->GetFormID());
        if (doc.HasMember("race")) {
            if (auto race = RE::TESForm::LookupByID<RE::TESRace>(FormUtil::FormIDFromString(doc["race"].GetString()))) {
                npc->race = race;
            }
            else {
                logger::warn("[ApplyJSON] [{:08X}] Falha ao encontrar Race ID.", npc->GetFormID());
            }
        }

        if (doc.HasMember("skin")) {
            if (auto skin = RE::TESForm::LookupByID<RE::TESObjectARMO>(FormUtil::FormIDFromString(doc["skin"].GetString()))) {
                npc->farSkin = skin;
            }
            else {
                npc->farSkin = nullptr;
            }
        }

        if (doc.HasMember("defaultOutfit")) {
            if (auto out = RE::TESForm::LookupByID<RE::BGSOutfit>(FormUtil::FormIDFromString(doc["defaultOutfit"].GetString()))) {
                npc->defaultOutfit = out;
            }
            else npc->defaultOutfit = nullptr;
        }

        if (doc.HasMember("sleepOutfit")) {
            if (auto out = RE::TESForm::LookupByID<RE::BGSOutfit>(FormUtil::FormIDFromString(doc["sleepOutfit"].GetString()))) {
                npc->sleepOutfit = out;
            }
            else npc->sleepOutfit = nullptr;
        }

        /*if (doc.HasMember("voice")) {
            if (auto voice = RE::TESForm::LookupByID<RE::BGSVoiceType>(FormUtil::FormIDFromString(doc["voice"].GetString()))) {
                npc->SetObjectVoiceType(voice);
            }
            else {
                npc->SetObjectVoiceType(nullptr); 
            }
        }*/

        if (doc.HasMember("hairColor")) {
            if (auto hc = RE::TESForm::LookupByID<RE::BGSColorForm>(FormUtil::FormIDFromString(doc["hairColor"].GetString()))) {
                if (!npc->headRelatedData) {
                    logger::info("[ApplyJSON] [{:08X}] Alocando HeadRelatedData...", npc->GetFormID());
                    npc->headRelatedData = new RE::TESNPC::HeadRelatedData();
                }
                npc->headRelatedData->hairColor = hc;
            }
        }

        // 3. HeadParts
        logger::info("[ApplyJSON] [{:08X}] Passo 3: Processando HeadParts...", npc->GetFormID());
        if (doc.HasMember("headParts") && doc["headParts"].IsArray()) {
            std::vector<RE::BGSHeadPart*> parts;
            std::set<RE::BGSHeadPart*> processed;

            std::function<void(RE::BGSHeadPart*)> AddPartAndExtras = [&](RE::BGSHeadPart* hp) {
                if (!hp || processed.contains(hp)) return;
                processed.insert(hp);
                parts.push_back(hp);
                for (auto* extra : hp->extraParts) {
                    if (extra) AddPartAndExtras(extra);
                }
                };

            for (auto& hpJson : doc["headParts"].GetArray()) {
                if (auto hp = RE::TESForm::LookupByID<RE::BGSHeadPart>(FormUtil::FormIDFromString(hpJson.GetString()))) {
                    AddPartAndExtras(hp);
                }
                else {
                    logger::warn("[ApplyJSON] [{:08X}] HeadPart ID {} nao encontrado e sera ignorado.", npc->GetFormID(), hpJson.GetString());
                }
            }

            if (npc->headParts) {
                logger::info("[ApplyJSON] [{:08X}] Liberando memoria das HeadParts originais...", npc->GetFormID());
                RE::free(npc->headParts);
                npc->headParts = nullptr;
            }

            if (!parts.empty()) {
                logger::info("[ApplyJSON] [{:08X}] Alocando {} novas HeadParts...", npc->GetFormID(), parts.size());
                auto newHeadParts = RE::calloc<RE::BGSHeadPart*>(parts.size());
                for (size_t i = 0; i < parts.size(); ++i) newHeadParts[i] = parts[i];
                npc->headParts = newHeadParts;
                npc->numHeadParts = static_cast<int8_t>(parts.size());
            }
            else {
                npc->numHeadParts = 0;
            }
        }

        // 4. Tint Layers
        logger::info("[ApplyJSON] [{:08X}] Passo 4: Processando Tint Layers...", npc->GetFormID());
        if (doc.HasMember("tintLayers") && doc["tintLayers"].IsArray()) {
            if (!npc->tintLayers) {
                logger::info("[ApplyJSON] [{:08X}] Criando novo array de TintLayers...", npc->GetFormID());
                npc->tintLayers = new RE::BSTArray<RE::TESNPC::Layer*>();
            }
            else {
                logger::info("[ApplyJSON] [{:08X}] Limpando {} TintLayers antigos...", npc->GetFormID(), npc->tintLayers->size());
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
            logger::info("[ApplyJSON] [{:08X}] {} Tint Layers injetadas.", npc->GetFormID(), idx);
        }

        // 5. Face Morphs
        logger::info("[ApplyJSON] [{:08X}] Passo 5: Processando Face Morphs...", npc->GetFormID());
        if (doc.HasMember("faceMorphs") && doc["faceMorphs"].IsArray()) {
            if (!npc->faceData) {
                logger::info("[ApplyJSON] [{:08X}] Alocando memoria para FaceData...", npc->GetFormID());
                npc->faceData = new RE::TESNPC::FaceData();
            }
            auto mArray = doc["faceMorphs"].GetArray();
            for (rapidjson::SizeType i = 0; i < mArray.Size() && i < 19; i++) {
                npc->faceData->morphs[i] = mArray[i].GetFloat();
            }
        }

        logger::info("[ApplyJSON] === Sucesso absoluto para NPC: {:08X} ===", npc->GetFormID());

    }
    catch (const std::exception& e) {
        logger::error("[ApplyJSON] CRITICAL EXCEPTION no NPC {:08X}: {}", npc->GetFormID(), e.what());
    }
    catch (...) {
        logger::error("[ApplyJSON] UNKNOWN EXCEPTION (Hard Crash evitado) no NPC {:08X}", npc->GetFormID());
    }
}

void Manager::PopulateAllLists() {
    if (_isPopulated) return;

    logger::info("Iniciando escaneamento de FormTypes...");

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

    _isPopulated = true;
    for (auto cb : _readyCallbacks) {
        if (cb) cb();
    }
    _readyCallbacks.clear();
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

    // 1. Testa se a string j� � um UTF-8 v�lido
    int u8Test = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_str.data(), static_cast<int>(a_str.size()), nullptr, 0);
    if (u8Test > 0) {
        // � UTF-8 v�lido (Skyrim SE nativo), retorna sem corromper
        return std::string(a_str);
    }

    // 2. Se falhou, a string � ANSI (Mod antigo ou locale espec�fico do Windows).
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
        // Vari�veis de aux�lio para o log de erro caso o catch seja acionado
        RE::FormID currentID = 0;
        std::string currentPlugin = "Unknown";

        try {
            currentID = form->GetFormID();

            // Obt�m o nome do plugin de origem antes de qualquer processamento complexo
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

            // EditorID: clib_util pode lan�ar exce��es em contextos raros de mem�ria
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

            // A convers�o UTF-8 � um ponto comum de falha se a string estiver corrompida
            info.name = ToUTF8(rawName);
            info.UpdateDisplayName();
            list.push_back(info);
        }
        catch (const std::exception& e) {
            // Log detalhado com FormID em Hexadecimal e o erro espec�fico
            logger::error("[PopulateList] Critical error on item {:08X} of plugin '{}' (Type: {}). Error: {}",
                currentID, currentPlugin, a_typeName, e.what());
        }
        catch (...) {
            // Captura erros desconhecidos que n�o herdam de std::exception
            logger::error("[PopulateList] Uknown error on item {:08X} of plugin '{}' (Type: {})",
                currentID, currentPlugin, a_typeName);
        }
    }
    logger::info("Carregados {} itens do tipo {}", list.size(), a_typeName);
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
            if (name.find("Head") != std::string::npos && name.find("Headband") == std::string::npos) {
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
        logger::info("[Head Swap] HeadPart identificado com sucesso: {}", headNodeName);
        return form->As<RE::BGSHeadPart>();
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

    void CollectHeadPartEditorIDs(RE::BGSHeadPart* a_headPart, std::unordered_set<std::string>& a_editorIDs)
    {
        if (!a_headPart) {
            return;
        }

        if (auto editorID = a_headPart->GetFormEditorID(); editorID && editorID[0] != '\0') {
            a_editorIDs.emplace(editorID);
        } else if (!a_headPart->formEditorID.empty()) {
            a_editorIDs.emplace(a_headPart->formEditorID.c_str());
        }

        for (auto* extra : a_headPart->extraParts) {
            CollectHeadPartEditorIDs(extra, a_editorIDs);
        }
    }

    std::unordered_set<std::string> GetAppliedHeadPartEditorIDs(RE::TESNPC* a_npc)
    {
        std::unordered_set<std::string> editorIDs;
        if (!a_npc || !a_npc->headParts) {
            return editorIDs;
        }

        for (int i = 0; i < a_npc->numHeadParts; ++i) {
            CollectHeadPartEditorIDs(a_npc->headParts[i], editorIDs);
        }

        return editorIDs;
    }

    bool IsAppliedHeadPartName(std::string_view a_name, const std::unordered_set<std::string>& a_appliedHeadPartIDs)
    {
        if (a_name.empty()) {
            return false;
        }

        if (a_appliedHeadPartIDs.contains(std::string(a_name))) {
            return true;
        }

        const auto targetKey = NormalizeGeometryKey(a_name);
        for (const auto& editorID : a_appliedHeadPartIDs) {
            if (NormalizeGeometryKey(editorID) == targetKey) {
                return true;
            }
        }

        return false;
    }

    bool IsGeometryAppliedHeadPart(RE::BSGeometry* a_geometry, const std::unordered_set<std::string>& a_appliedHeadPartIDs)
    {
        return a_geometry && !a_geometry->name.empty() && IsAppliedHeadPartName(a_geometry->name.c_str(), a_appliedHeadPartIDs);
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

    bool CopyVerticesToRuntimeGeometry(RE::BSGeometry* a_targetGeometry, RE::BSGeometry* a_sourceGeometry, const std::string& a_context)
    {
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

        for (std::uint32_t i = 0; i < targetVertCount; ++i) {
            targetVerts[i].x = sourceVerts[i].x;
            targetVerts[i].y = sourceVerts[i].y;
            targetVerts[i].z = sourceVerts[i].z;
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
                newFod->vertexData[i].x = sourceVerts[i].x;
                newFod->vertexData[i].y = sourceVerts[i].y;
                newFod->vertexData[i].z = sourceVerts[i].z;
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
                                    modelVerts[i].x = sourceVerts[i].x;
                                    modelVerts[i].y = sourceVerts[i].y;
                                    modelVerts[i].z = sourceVerts[i].z;
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
}

void Manager::ClearFaceGenGeometryIndex()
{
    _faceGenGeometryIndex.clear();
    _faceGenGeometryDuplicates = 0;
    logger::info("[FaceGen Index] Cleared baked geometry index.");
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
    logger::info("[Face Swap] === Start ===");

    if (!a_actor || !a_actor->Get3D(false)) {
        logger::error("[Face Swap] Ator invalido ou sem modelo 3D.");
        return;
    }

    RE::NiAVObject* actor3D = a_actor->Get3D(false);
    logger::info("[Face Swap] Actor 3D encontrado: {:X}", reinterpret_cast<std::uintptr_t>(actor3D));

    auto faceNode = netimmerse_cast<RE::BSFaceGenNiNode*>(actor3D->GetObjectByName("BSFaceGenNiNodeSkinned"));
    logger::info("[Face Swap] Face node runtime: {:X}", reinterpret_cast<std::uintptr_t>(faceNode));
    if (!faceNode || !faceNode->parent) {
        logger::error("[Face Swap] Ator nao possui BSFaceGenNiNodeSkinned.");
        return;
    }

    auto* npc = a_actor->GetActorBase() ? a_actor->GetActorBase()->As<RE::TESNPC>() : nullptr;
    const auto appliedHeadPartIDs = GetAppliedHeadPartEditorIDs(npc);
    logger::info("[Face Swap] HeadParts aplicadas no NPC para baked extras: {}", appliedHeadPartIDs.size());

    logger::info("[Face Swap] Carregando custom NIF: {}", a_nifPath);
    auto nifRoot = LoadNifFromFile(a_nifPath);
    logger::info("[Face Swap] NIF root carregado: {:X}", reinterpret_cast<std::uintptr_t>(nifRoot.get()));
    if (!nifRoot) {
        logger::error("[Face Swap] Falha ao carregar a raiz do NIF novo.");
        return;
    }

    logger::info("[Face Swap] Procurando NiNode BSFaceGenNiNodeSkinned na custom NIF...");
    auto sourceFaceNode = netimmerse_cast<RE::NiNode*>(nifRoot->GetObjectByName("BSFaceGenNiNodeSkinned"));
    logger::info("[Face Swap] Source face node: {:X}", reinterpret_cast<std::uintptr_t>(sourceFaceNode));
    if (!sourceFaceNode) {
        logger::error("[Face Swap] Custom NIF nao possui NiNode BSFaceGenNiNodeSkinned valido: {}", a_nifPath);
        return;
    }

    std::vector<RE::NiPointer<RE::BSGeometry>> sourceAllGeometries;
    std::vector<RE::NiPointer<RE::BSGeometry>> sourceGeometries;
    logger::info("[Face Swap] Coletando geometrias da custom NIF...");
    RE::BSVisit::TraverseScenegraphGeometries(sourceFaceNode, [&](RE::BSGeometry* a_geometry) {
        if (a_geometry) {
            sourceAllGeometries.emplace_back(a_geometry);
        }

        const auto kind = GetFacePartKind(a_geometry);
        if (!kind.empty()) {
            logger::info("[Face Swap] Source geometry selecionada [{}]: '{}' {:X}",
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
    logger::info("[Face Swap] Geometrias faciais selecionadas na custom NIF: {}", sourceGeometries.size());

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
            logger::info("[Face Swap] Target head geometry selecionada: '{}'",
                a_geometry && !a_geometry->name.empty() ? a_geometry->name.c_str() : "SemNome");
            targetHeadGeometries.emplace_back(a_geometry);
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });

    if (targetHeadGeometries.empty()) {
        logger::error("[Face Swap] Ator nao possui geometria base de head/face para receber morph.");
        return;
    }

    if (CopyVerticesToRuntimeGeometry(targetHeadGeometries.front().get(), sourceHeadIt->get(), "head")) {
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
                logger::info("[Face Swap] {} reposicionado: '{}' -> '{}'",
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

            if (!IsGeometryAppliedHeadPart(source.get(), appliedHeadPartIDs)) {
                logger::debug("[Face Swap] Baked extra '{}' ignorado: nao corresponde a HeadPart aplicada no NPC.",
                    source->name.empty() ? "SemNome" : source->name.c_str());
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
        for (const auto& appliedHeadPartID : appliedHeadPartIDs) {
            if (appliedHeadPartID.empty()) {
                continue;
            }

            const auto headPartKey = NormalizeGeometryKey(appliedHeadPartID);
            if (attachedBakedExtraNames.contains(headPartKey)) {
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

            logger::info("[Face Swap] HeadPart '{}' sem geometria runtime; usando FaceGen indexada '{}' ({})",
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
                logger::info("[Face Swap] Baked HeadPart '{}' anexada a partir do indice.", appliedHeadPartID);
            }
        }

        for (auto& item : bakedExtras) {
            if (item.parent && item.geometry) {
                item.parent->AttachChild(item.geometry.get());
                logger::info("[Face Swap] Baked extra anexado: '{}'",
                    item.geometry->name.empty() ? "SemNome" : item.geometry->name.c_str());
            }
        }

        RE::NiUpdateData updateData{};
        updateData.time = faceNode->GetRuntimeData().lastTime;
        faceNode->UpdateWorldData(&updateData);
        UpdateFaceNodeBounds(faceNode);
        faceNode->FixSkinInstances(actor3D->AsNode(), false);
        if (auto* queue = RE::TaskQueueInterface::GetSingleton()) {
            queue->QueueUpdateNiObject(actor3D);
        }

        if (npc) {
            npc->UpdateNeck(faceNode);
        }

        logger::info("[Face Swap] Head morph aplicado na geometria original do ator: '{}' -> '{}'.",
            (*sourceHeadIt)->name.empty() ? "SemNome" : (*sourceHeadIt)->name.c_str(),
            targetHeadGeometries.front()->name.empty() ? "SemNome" : targetHeadGeometries.front()->name.c_str());
        logger::info("[Face Swap] Partes faciais reposicionadas: {}", adjustedParts);
        logger::info("[Face Swap] Baked extras anexados: {}", bakedExtras.size());
        logger::info("[Face Swap] === Concluida! ===");
        return;
    }

    logger::warn("[Face Swap] Head morph direto falhou; usando clone fallback experimental.");

    std::vector<ClonedFaceGeometry> pending;
    std::unordered_set<std::string> replacementNames;
    std::unordered_set<std::uint16_t> replacementSlots;

    for (const auto& source : sourceGeometries) {
        if (!source) {
            continue;
        }

        logger::info("[Face Swap] Clonando head geometry '{}'.", source->name.empty() ? "SemNome" : source->name.c_str());
        RE::NiPointer<RE::NiObject> clonedObject(source->Clone());
        auto* clonedGeometry = clonedObject ? clonedObject->AsGeometry() : nullptr;
        if (!clonedGeometry) {
            logger::warn("[Face Swap] Falha ao clonar geometria '{}'.", source->name.c_str());
            continue;
        }

        RE::NiPointer<RE::BSGeometry> clone(clonedGeometry);
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

    logger::info("[Face Swap] Clones anexados; pulando reset de animationData neste teste.");

    RE::NiUpdateData updateData{};
    updateData.time = faceNode->GetRuntimeData().lastTime;
    logger::info("[Face Swap] Atualizando world data/bounds...");
    faceNode->UpdateWorldData(&updateData);
    UpdateFaceNodeBounds(faceNode);
    logger::info("[Face Swap] FixSkinInstances...");
    faceNode->FixSkinInstances(actor3D->AsNode(), false);
    if (auto* queue = RE::TaskQueueInterface::GetSingleton()) {
        logger::info("[Face Swap] QueueUpdateNiObject..");
        queue->QueueUpdateNiObject(actor3D);
    }

    if (auto npcBase = a_actor->GetActorBase()) {
        if (auto npcForNeck = npcBase->As<RE::TESNPC>()) {
            logger::info("[Face Swap] UpdateNeck...");
            npcForNeck->UpdateNeck(faceNode);
        }
    }

    logger::info("[Face Swap] Geometrias da custom NIF clonadas no face node original: {} anexadas, {} originais ocultas.",
        pending.size(), hiddenOriginals);
    logger::info("[Face Swap] === Concluida! ===");
}
void Manager::ScheduleFaceDeform(RE::FormID actorID, const std::string& nifPath, int retries) {
    if (retries <= 0) {
        logger::error("[Swap & Deform] Esgotaram as tentativas esperando o 3D do Ator {:08X} recarregar.", actorID);
        return;
    }

    std::thread([actorID, nifPath, retries]() {
        // Log apenas na primeira tentativa para n�o floodar
        if (retries == 20) logger::info("[TIMER] Thread de deforma��o iniciada para {:08X}. Aguardando 3D...", actorID);

        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        SKSE::GetTaskInterface()->AddTask([actorID, nifPath, retries]() {
            auto pActor = RE::TESForm::LookupByID<RE::Actor>(actorID);

            if (pActor && pActor->Is3DLoaded() && pActor->GetFaceNodeSkinned()) {
                //logger::info("[TIMER] Sucesso! 3D pronto para {:08X}. Aplicando DeformFace...", actorID);
                Manager::DeformFaceToMatchNif(pActor, nifPath);
            }
            else {
                // Tenta novamente
                if (retries % 5 == 0) logger::info("[TIMER] Ator {:08X} ainda n�o est� pronto. Tentativas restantes: {}", actorID, retries);
                ScheduleFaceDeform(actorID, nifPath, retries - 1);
            }
            });
        }).detach();
}

void Manager::RegisterAffectedNPC(RE::FormID baseID, const std::string& nifPath) {
    _affectedNPCs[baseID] = nifPath;
}

void Manager::UnregisterAffectedNPC(RE::FormID baseID) {
    _affectedNPCs.erase(baseID);
}

bool Manager::IsNPCAffected(RE::FormID baseID, std::string& outNifPath) {
    auto it = _affectedNPCs.find(baseID);
    if (it != _affectedNPCs.end()) {
        outNifPath = it->second;
        return true;
    }
    return false;
}
