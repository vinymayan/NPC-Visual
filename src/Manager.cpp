#include "Manager.h"

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

    // Função auxiliar para reverter string para FormID no load do JSON
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
    if (!npc) return;

    npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kIsChargenFacePreset);

    // 1. Atributos Básicos
    if (doc.HasMember("height") && doc["height"].IsFloat()) {
        npc->height = doc["height"].GetFloat();
    }

    if (doc.HasMember("isFemale") && doc["isFemale"].IsBool()) {
        if (doc["isFemale"].GetBool()) {
            npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
        }
        else {
            npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
        }
    }

    if (doc.HasMember("oppositeGenderAnim") && doc["oppositeGenderAnim"].IsBool()) {
        if (doc["oppositeGenderAnim"].GetBool()) {
            npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kOppositeGenderAnims);
        }
        else {
            npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kOppositeGenderAnims);
        }
    }

    if (doc.HasMember("bodyTintColor")) {
        auto& c = doc["bodyTintColor"];
        npc->bodyTintColor.red = c.HasMember("r") ? c["r"].GetInt() : 255;
        npc->bodyTintColor.green = c.HasMember("g") ? c["g"].GetInt() : 255;
        npc->bodyTintColor.blue = c.HasMember("b") ? c["b"].GetInt() : 255;
        npc->bodyTintColor.alpha = c.HasMember("a") ? c["a"].GetInt() : 255;
    }

    // 2. Associações de IDs
    if (doc.HasMember("race")) {
        if (auto race = RE::TESForm::LookupByID<RE::TESRace>(FormUtil::FormIDFromString(doc["race"].GetString()))) {
            npc->race = race;
        }
    }

    if (doc.HasMember("skin")) {
        if (auto skin = RE::TESForm::LookupByID<RE::TESObjectARMO>(FormUtil::FormIDFromString(doc["skin"].GetString()))) {
            npc->farSkin = skin;
        }
        else {
            npc->farSkin = nullptr; // Reseta se não encontrar ou for "Nenhum"
        }
    }

    if (doc.HasMember("defaultOutfit")) {
        if (auto out = RE::TESForm::LookupByID<RE::BGSOutfit>(FormUtil::FormIDFromString(doc["defaultOutfit"].GetString()))) {
            npc->defaultOutfit = out;
        }
        else {
            npc->defaultOutfit = nullptr;
        }
    }

    if (doc.HasMember("sleepOutfit")) {
        if (auto out = RE::TESForm::LookupByID<RE::BGSOutfit>(FormUtil::FormIDFromString(doc["sleepOutfit"].GetString()))) {
            npc->sleepOutfit = out;
        }
        else {
            npc->sleepOutfit = nullptr;
        }
    }

    if (doc.HasMember("hairColor")) {
        if (auto hc = RE::TESForm::LookupByID<RE::BGSColorForm>(FormUtil::FormIDFromString(doc["hairColor"].GetString()))) {
            if (!npc->headRelatedData) {
                npc->headRelatedData = new RE::TESNPC::HeadRelatedData();
            }
            npc->headRelatedData->hairColor = hc;
        }
    }

    // 3. HeadParts (Substituição de Array Dinâmico com Extra Parts)
    if (doc.HasMember("headParts") && doc["headParts"].IsArray()) {
        std::vector<RE::BGSHeadPart*> parts;
        std::function<void(RE::BGSHeadPart*)> AddPartAndExtras = [&](RE::BGSHeadPart* hp) {
            if (!hp) return;
            if (std::find(parts.begin(), parts.end(), hp) == parts.end()) {
                parts.push_back(hp);
                for (auto* extra : hp->extraParts) {
                    AddPartAndExtras(extra);
                }
            }
            };


        for (auto& hpJson : doc["headParts"].GetArray()) {
            if (auto hp = RE::TESForm::LookupByID<RE::BGSHeadPart>(FormUtil::FormIDFromString(hpJson.GetString()))) {
                AddPartAndExtras(hp);
            }
        }

        if (npc->headParts) {
            RE::free(npc->headParts);
            npc->headParts = nullptr;
        }
        npc->numHeadParts = 0; 

        if (!parts.empty()) {
            auto newHeadParts = RE::calloc<RE::BGSHeadPart*>(parts.size());
            for (size_t i = 0; i < parts.size(); ++i) {
                newHeadParts[i] = parts[i];
            }
            npc->headParts = newHeadParts;
            npc->numHeadParts = static_cast<int8_t>(parts.size());
        }
    }
    else {
        if (npc->headParts && npc->numHeadParts > 0) {
            std::vector<RE::BGSHeadPart*> currentParts;
            for (int i = 0; i < npc->numHeadParts; ++i) {
                if (npc->headParts[i]) currentParts.push_back(npc->headParts[i]);
            }

            RE::free(npc->headParts);
            auto newHeadParts = RE::calloc<RE::BGSHeadPart*>(currentParts.size());
            for (size_t i = 0; i < currentParts.size(); ++i) {
                newHeadParts[i] = currentParts[i];
            }
            npc->headParts = newHeadParts;
            npc->numHeadParts = static_cast<int8_t>(currentParts.size());
        }
    }

    // 4. Tint Layers (Maquiagem, Sujeira, Pele)
    if (doc.HasMember("tintLayers") && doc["tintLayers"].IsArray()) {
        if (!npc->tintLayers) {
            npc->tintLayers = new RE::BSTArray<RE::TESNPC::Layer*>();
        }
        else {
            // Limpa as antigas (opcional dependendo de como preferir lidar com vazamento de mem, mas ok para edição na hora)
            npc->tintLayers->clear();
        }

        for (auto& l : doc["tintLayers"].GetArray()) {
            auto layer = new RE::TESNPC::Layer(); // Cria a nova layer
            layer->tintIndex = static_cast<uint16_t>(l["index"].GetInt());
            auto& c = l["color"];
            layer->tintColor.red = c.HasMember("r") ? c["r"].GetInt() : 0;
            layer->tintColor.green = c.HasMember("g") ? c["g"].GetInt() : 0;
            layer->tintColor.blue = c.HasMember("b") ? c["b"].GetInt() : 0;
            layer->tintColor.alpha = c.HasMember("a") ? c["a"].GetInt() : 255;

            // O Skyrim salva a Opacidade variando de 0 a 100 inteiros
            layer->interpolationValue = static_cast<uint16_t>(l["interpolation"].GetFloat() * 100.0f);
            layer->preset = static_cast<uint16_t>(l["preset"].GetInt());

            npc->tintLayers->push_back(layer);
        }
    }

    // 5. Face Morphs (Deslizantes do Rosto)
    if (doc.HasMember("faceMorphs") && doc["faceMorphs"].IsArray()) {
        if (!npc->faceData) {
            npc->faceData = new RE::TESNPC::FaceData();
        }
        auto mArray = doc["faceMorphs"].GetArray();
        for (rapidjson::SizeType i = 0; i < mArray.Size() && i < 19; i++) {
            npc->faceData->morphs[i] = mArray[i].GetFloat();
        }
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

// Altere a implementação:
std::string Manager::ToUTF8(std::string_view a_str) {
    if (a_str.empty()) return "";

    // Converte string_view para data temporária para o WinAPI
    int wlen = MultiByteToWideChar(CP_ACP, 0, a_str.data(), static_cast<int>(a_str.size()), nullptr, 0);
    if (wlen <= 0) return std::string(a_str);

    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, a_str.data(), static_cast<int>(a_str.size()), &wstr[0], wlen);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return std::string(a_str);

    std::string u8str(u8len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &u8str[0], u8len, nullptr, nullptr);

    if (!u8str.empty() && u8str.back() == '\0') u8str.pop_back();

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
        // Variáveis de auxílio para o log de erro caso o catch seja acionado
        RE::FormID currentID = 0;
        std::string currentPlugin = "Unknown";

        try {
            currentID = form->GetFormID();

            // Obtém o nome do plugin de origem antes de qualquer processamento complexo
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

            // EditorID: clib_util pode lançar exceções em contextos raros de memória
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

            // A conversão UTF-8 é um ponto comum de falha se a string estiver corrompida
            info.name = ToUTF8(rawName);
            info.UpdateDisplayName();
            list.push_back(info);
        }
        catch (const std::exception& e) {
            // Log detalhado com FormID em Hexadecimal e o erro específico
            logger::error("[PopulateList] Critical error on item {:08X} of plugin '{}' (Type: {}). Error: {}",
                currentID, currentPlugin, a_typeName, e.what());
        }
        catch (...) {
            // Captura erros desconhecidos que não herdam de std::exception
            logger::error("[PopulateList] Uknown error on item {:08X} of plugin '{}' (Type: {})",
                currentID, currentPlugin, a_typeName);
        }
    }
    logger::info("Carregados {} itens do tipo {}", list.size(), a_typeName);
}