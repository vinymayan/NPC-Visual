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
        std::set<RE::BGSHeadPart*> processed; // <--- NOVO: Trava de segurança

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
        }

        if (npc->headParts) RE::free(npc->headParts);

        if (!parts.empty()) {
            auto newHeadParts = RE::calloc<RE::BGSHeadPart*>(parts.size());
            for (size_t i = 0; i < parts.size(); ++i) newHeadParts[i] = parts[i];
            npc->headParts = newHeadParts;
            npc->numHeadParts = static_cast<int8_t>(parts.size());
        }
    }

    // 4. Tint Layers
    if (doc.HasMember("tintLayers") && doc["tintLayers"].IsArray()) {
        logger::info("[DEBUG] Aplicando {} Tint Layers...", doc["tintLayers"].Size());
        if (!npc->tintLayers) npc->tintLayers = new RE::BSTArray<RE::TESNPC::Layer*>();
        else npc->tintLayers->clear();

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

struct DynVertex { float x, y, z, w; };

void Manager::DeformFaceToMatchNif(RE::Actor* a_actor, const std::string& a_nifPath) {
    logger::info("[Face Swap] === Start ===");

    if (!a_actor || !a_actor->Get3D(false)) {
        logger::error("[Face Swap] Ator invalido ou sem modelo 3D.");
        return;
    }

    RE::NiAVObject* actor3D = a_actor->Get3D(false);
    auto faceNode = netimmerse_cast<RE::BSFaceGenNiNode*>(actor3D->GetObjectByName("BSFaceGenNiNodeSkinned"));
	//logger::info("[Face Swap] BSFaceGenNiNodeSkinned do ator encontrado: {}", faceNode ? "Sim" : "Nao");

    if (!faceNode || !faceNode->parent) {
        logger::error("[Face Swap] Ator nao possui BSFaceGenNiNodeSkinned.");
        return;
    }

    RE::NiNode* skeletonRoot = actor3D->AsNode();
    RE::NiPointer<RE::BSFaceGenAnimationData> savedAnimData = faceNode->GetRuntimeData().animationData;

    auto nifRoot = LoadNifFromFile(a_nifPath);
    if (!nifRoot) {
        logger::error("[Face Swap] Falha ao carregar a raiz do NIF novo.");
        return;
    }

    auto novaCabecaNode = netimmerse_cast<RE::NiNode*>(nifRoot->GetObjectByName("BSFaceGenNiNodeSkinned"));
    if (!novaCabecaNode) return;

    RE::NiPointer<RE::NiNode> safeNovaCabecaNode(novaCabecaNode);
    if (safeNovaCabecaNode->parent) {
        safeNovaCabecaNode->parent->AsNode()->DetachChild(safeNovaCabecaNode.get());
    }

    auto GetHeadPartType = [](const std::string& nodeName, RE::BGSHeadPart::HeadPartType& outType) -> bool {
        if (RE::BGSHeadPart* hp = RE::TESForm::LookupByEditorID<RE::BGSHeadPart>(nodeName)) {
            outType = hp->type.get();
            return true;
        }
        return false;
        };

    // =============================================================================
    // PARTE 1: NODE SWAP (Apenas para a Cabeça / Head)
    // =============================================================================
    RE::NiPointer<RE::NiAVObject> oldHeadMesh;
    RE::NiPointer<RE::NiAVObject> newHeadMesh;

    auto FindHeadMesh = [&GetHeadPartType](RE::NiNode* parentNode, const std::string& debugPrefix) -> RE::NiPointer<RE::NiAVObject> {
        if (!parentNode) return nullptr;

        for (auto& child : parentNode->GetChildren()) {
            if (!child) continue;

            std::string childName = (child->name.c_str()) ? child->name.c_str() : "Sem Nome";
            std::string lowerName = childName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

            bool isHeadByName = (lowerName.find("head") != std::string::npos && lowerName.find("headband") == std::string::npos) ||
                (lowerName.find("face") != std::string::npos && lowerName.find("facelight") == std::string::npos);

            if (isHeadByName) {
                // NOVA VALIDAÇÃO: Checa o EditorID
                RE::BGSHeadPart::HeadPartType nodeType;
                if (GetHeadPartType(childName, nodeType)) {
                    // Se o EditorID existe mas não for a malha base do rosto (kFace), pulamos ele.
                    // Isso filtra fios de cabelo de física (como _Fuse00_CollisionFemaleHead) e miscs
                    if (nodeType != RE::BGSHeadPart::HeadPartType::kFace) {
                        //logger::info("[FindHeadMesh] [{}] Ignorando '{}' pois o EditorID revela que nao eh kFace.", debugPrefix, childName);
                        continue;
                    }
                }

                //logger::info("[FindHeadMesh] [{}] Cabeca encontrada com precisao pelo nome: '{}'", debugPrefix, childName);
                return child;
            }
        }

        logger::error("[FindHeadMesh] [{}] ERRO: Nenhuma malha valida de cabeca encontrada! O modelo nao usa as keywords padroes.", debugPrefix);
        return nullptr;
        };

    oldHeadMesh = FindHeadMesh(faceNode, "Ator Original");
    if (oldHeadMesh) {
        //logger::info("[Face Swap] Resultado final para a cabeca do ator: '{}'", oldHeadMesh->name.c_str() ? oldHeadMesh->name.c_str() : "Sem Nome");
    }

    newHeadMesh = FindHeadMesh(safeNovaCabecaNode.get(), "Nova NIF");
    if (newHeadMesh) {
        //logger::info("[Face Swap] Resultado final para a cabeca na NIF: '{}'", newHeadMesh->name.c_str() ? newHeadMesh->name.c_str() : "Sem Nome");
    }

    // PREVENÇÃO DE CRASH: Se alguma das malhas não for encontrada, abortamos a execução de forma segura.
    if (!oldHeadMesh || !newHeadMesh) {
        logger::error("[Face Swap] ERRO CRITICO PREVENIDO: oldHeadMesh ({}) ou newHeadMesh ({}). Abortando Node Swap.",
            oldHeadMesh ? "OK" : "FALHOU", newHeadMesh ? "OK" : "FALHOU");
        return;
    }

    // =============================================================================
        // PARTE 1: MORPH DA CABEÇA (Substituindo o antigo Node Swap)
        // =============================================================================
    if (oldHeadMesh && newHeadMesh) {
        RE::BSGeometry* oldGeo = oldHeadMesh->AsGeometry();
        RE::BSGeometry* newGeo = newHeadMesh->AsGeometry();

        auto oldDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(oldGeo);
        auto newDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(newGeo);

        if (oldDynShape && newDynShape) {
            uint32_t oldVertCount = oldDynShape->GetTrishapeRuntimeData().vertexCount;
            uint32_t newVertCount = newDynShape->GetTrishapeRuntimeData().vertexCount;

            if (oldVertCount == newVertCount) {
                //logger::info("[Face Swap] Contagem de vertices compativel ({}). Iniciando Morph da Cabeca...", oldVertCount);

                // 1. Atualizar vértices da malha atual
                auto oldVerts = reinterpret_cast<DynVertex*>(oldDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
                auto newVerts = reinterpret_cast<DynVertex*>(newDynShape->GetDynamicTrishapeRuntimeData().dynamicData);

                for (uint32_t i = 0; i < oldVertCount; ++i) {
                    oldVerts[i].x = newVerts[i].x;
                    oldVerts[i].y = newVerts[i].y;
                    oldVerts[i].z = newVerts[i].z;
                }

                // 2. Atualizar ou recriar o FOD (FaceGenBaseMorphExtraData)
                if (oldDynShape->GetExtraData("FOD")) {
                    oldDynShape->RemoveExtraData("FOD");
                }

                auto newFod = RE::BSFaceGenBaseMorphExtraData::Create(nullptr, false);
                if (newFod) {
                    newFod->vertexCount = oldVertCount;
                    newFod->modelVertexCount = oldVertCount;
                    // Aloca a memória usando a contagem de vértices do modelo
                    newFod->vertexData = (RE::NiPoint3*)RE::MemoryManager::GetSingleton()->Allocate(sizeof(RE::NiPoint3) * oldVertCount, 0, false);

                    for (uint32_t i = 0; i < oldVertCount; ++i) {
                        newFod->vertexData[i].x = newVerts[i].x;
                        newFod->vertexData[i].y = newVerts[i].y;
                        newFod->vertexData[i].z = newVerts[i].z;
                    }
                    oldDynShape->AddExtraData(newFod);
                    //logger::info("[Face Swap] FOD atualizado para a nova geometria base.");
                }

                // 3. Atualizar vértices do modelo original salvo dentro do FMD
                auto oldNet = oldHeadMesh->AsNode() ? static_cast<RE::NiObjectNET*>(oldHeadMesh->AsNode()) : static_cast<RE::NiObjectNET*>(oldGeo);
                if (oldNet) {
                    if (auto oldExtra = oldNet->GetExtraData("FMD")) {
                        if (auto fmd = static_cast<RE::BSFaceGenModelExtraData*>(oldExtra)) {
                            // Usando sua lógica de checagem do snippet
                            if (fmd && fmd->m_model && fmd->m_model->modelMeshData && fmd->m_model->modelMeshData->faceNode) {
                                //logger::info("[Face Swap] Atualizando malha base dentro do FMD...");
                                auto origRootNode = fmd->m_model->modelMeshData->faceNode->AsNode();

                                auto& children = origRootNode->GetChildren();
                                for (uint16_t i = 0; i < children.size(); i++) {
                                    if (children[i]) {
                                        if (auto origGeo = children[i]->AsGeometry()) {
                                            if (auto origDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(origGeo)) {
                                                // Verifica se o shape em cache tem o mesmo número de vértices
                                                if (origDynShape->GetTrishapeRuntimeData().vertexCount == oldVertCount) {
                                                    auto origVerts = reinterpret_cast<DynVertex*>(origDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
                                                    for (uint32_t v = 0; v < oldVertCount; ++v) {
                                                        origVerts[v].x = newVerts[v].x;
                                                        origVerts[v].y = newVerts[v].y;
                                                        origVerts[v].z = newVerts[v].z;
                                                    }
                                                   // logger::info("[Face Swap] Vertices do FMD atualizados com sucesso!");
                                                }
                                            }
                                            // Interrompe ao achar a geometria principal da cabeça no backup do model
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

#ifndef SKYRIM_CROSS_VR
                // Atualiza o colisor/caixa delimitadora da engine para os novos vértices
                oldDynShape->UpdateWorldBound();
#endif
                logger::info("[Face Swap] Morph completo realizado na cabeca original do ator!");
            }
            else {
                logger::error("[Face Swap] Incompatibilidade de vertices: Original={} Novo={}. Impossivel realizar morph direto.", oldVertCount, newVertCount);
            }
        }
    }

    // =============================================================================
        // PARTE 2: MORPH INJECTION (Para Sobrancelhas, Olhos, Boca, etc)
        // =============================================================================
        // Nota: "Head" foi removido daqui pois ja foi processado na PARTE 1
        // Keywords todas em minusculo para facilitar o match case-insensitive
    std::vector<std::string> morphKeywords = { "brows", "eyes", "mouth", "beard", "scar" };

    // Iteramos por TODOS os filhos do faceNode do ator (NPC no jogo)
    for (auto& actorChild : faceNode->GetChildren()) {
        if (!actorChild || !actorChild->name.c_str()) continue;

        std::string actorName = actorChild->name.c_str();
        std::string lowerActorName = actorName;
        std::transform(lowerActorName.begin(), lowerActorName.end(), lowerActorName.begin(), ::tolower);

        std::string matchedKeyword = "";

        for (const auto& kw : morphKeywords) {
            if (lowerActorName.find(kw) != std::string::npos) {
                matchedKeyword = kw;
                break;
            }
        }

        if (matchedKeyword.empty()) continue;

        // Obter o tipo do HeadPart do Ator Original
        RE::BGSHeadPart::HeadPartType actorType;
        bool hasActorType = GetHeadPartType(actorName, actorType);

        for (auto& nifChild : safeNovaCabecaNode->GetChildren()) {
            if (!nifChild || !nifChild->name.c_str()) continue;

            std::string nifName = nifChild->name.c_str();
            std::string lowerNifName = nifName;
            std::transform(lowerNifName.begin(), lowerNifName.end(), lowerNifName.begin(), ::tolower);

            if (lowerNifName.find(matchedKeyword) != std::string::npos) {

                // NOVA VALIDAÇÃO: Checa se os HeadParts são do mesmo tipo (Ex: kEyes com kEyes)
                RE::BGSHeadPart::HeadPartType nifType;
                bool hasNifType = GetHeadPartType(nifName, nifType);

                if (hasActorType && hasNifType && actorType != nifType) {
                    logger::debug("[Face Swap] Ignorando Morph de '{}' para '{}' (Tipos de BGSHeadPart incompativeis).", actorName, nifName);
                    continue; // Pula pois os tipos não batem
                }

                auto currentDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(actorChild->AsGeometry());
                auto nifDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(nifChild->AsGeometry());

                if (currentDynShape && nifDynShape) {
                    uint32_t currentVertCount = currentDynShape->GetTrishapeRuntimeData().vertexCount;
                    uint32_t nifVertCount = nifDynShape->GetTrishapeRuntimeData().vertexCount;

                    if (currentVertCount == nifVertCount) {
                        //logger::info("[Face Swap] Injetando morph em '{}' (Match via '{}': {}). Vertices: {}",
                            //actorName, matchedKeyword, nifName, currentVertCount);

                        auto currentVerts = reinterpret_cast<DynVertex*>(currentDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
                        auto nifVerts = reinterpret_cast<DynVertex*>(nifDynShape->GetDynamicTrishapeRuntimeData().dynamicData);

                        for (uint32_t i = 0; i < currentVertCount; ++i) {
                            currentVerts[i].x = nifVerts[i].x;
                            currentVerts[i].y = nifVerts[i].y;
                            currentVerts[i].z = nifVerts[i].z;
                        }

                        if (currentDynShape->GetExtraData("FOD")) {
                            currentDynShape->RemoveExtraData("FOD");
                        }

                        auto newFod = RE::BSFaceGenBaseMorphExtraData::Create(nullptr, false);
                        if (newFod) {
                            newFod->vertexCount = currentVertCount;
                            newFod->modelVertexCount = currentVertCount;
                            newFod->vertexData = (RE::NiPoint3*)RE::MemoryManager::GetSingleton()->Allocate(sizeof(RE::NiPoint3) * currentVertCount, 0, false);

                            for (uint32_t i = 0; i < currentVertCount; ++i) {
                                newFod->vertexData[i].x = nifVerts[i].x;
                                newFod->vertexData[i].y = nifVerts[i].y;
                                newFod->vertexData[i].z = nifVerts[i].z;
                            }
                            currentDynShape->AddExtraData(newFod);
                        }

                        auto partNet = static_cast<RE::NiObjectNET*>(currentDynShape);
                        if (partNet) {
                            if (auto fmdExtra = partNet->GetExtraData("FMD")) {
                                if (auto fmd = static_cast<RE::BSFaceGenModelExtraData*>(fmdExtra)) {
                                    if (fmd && fmd->m_model && fmd->m_model->modelMeshData && fmd->m_model->modelMeshData->faceNode) {
                                        auto origRootNode = fmd->m_model->modelMeshData->faceNode->AsNode();

                                        auto& children = origRootNode->GetChildren();
                                        for (uint16_t i = 0; i < children.size(); i++) {
                                            if (children[i]) {
                                                if (auto origGeo = children[i]->AsGeometry()) {
                                                    if (auto origDynShape = netimmerse_cast<RE::BSDynamicTriShape*>(origGeo)) {
                                                        if (origDynShape->GetTrishapeRuntimeData().vertexCount == currentVertCount) {
                                                            auto origVerts = reinterpret_cast<DynVertex*>(origDynShape->GetDynamicTrishapeRuntimeData().dynamicData);
                                                            for (uint32_t v = 0; v < currentVertCount; ++v) {
                                                                origVerts[v].x = nifVerts[v].x;
                                                                origVerts[v].y = nifVerts[v].y;
                                                                origVerts[v].z = nifVerts[v].z;
                                                            }
                                                            logger::info("  -> FMD de '{}' atualizado com sucesso!", actorName);
                                                        }
                                                    }
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

#ifndef SKYRIM_CROSS_VR
                        currentDynShape->UpdateWorldBound();
#endif
                        break;
                    }
                    else {
                        logger::warn("[Face Swap] Falha ao injetar morph em '{}'. Vertices incompativeis (NPC: {}, NIF: {})", actorName, currentVertCount, nifVertCount);
                    }
                }
            }
        }
    }

    // =============================================================================
    // PARTE 3: FINALIZAÇÃO DA ENGINE
    // A cor não é mais aplicada aqui, apenas re-atrelamos as animações
    // =============================================================================
    faceNode->FixSkinInstances(skeletonRoot, true);

    if (savedAnimData) {
        savedAnimData->lock.Lock();
        savedAnimData->exprOverride = true;
        savedAnimData->lock.Unlock();
        //logger::info("[Face Swap] exprOverride ativada para regenerar as expressoes faciais.");
    }

    logger::info("[Face Swap] === Concluida! ===");
}

void Manager::ScheduleFaceDeform(RE::FormID actorID, const std::string& nifPath, int retries) {
    if (retries <= 0) {
        logger::error("[Swap & Deform] Esgotaram as tentativas esperando o 3D do Ator {:08X} recarregar.", actorID);
        return;
    }

    std::thread([actorID, nifPath, retries]() {
        // Log apenas na primeira tentativa para não floodar
        if (retries == 20) logger::info("[TIMER] Thread de deformação iniciada para {:08X}. Aguardando 3D...", actorID);

        std::this_thread::sleep_for(std::chrono::milliseconds(15));

        SKSE::GetTaskInterface()->AddTask([actorID, nifPath, retries]() {
            auto pActor = RE::TESForm::LookupByID<RE::Actor>(actorID);

            if (pActor && pActor->Is3DLoaded() && pActor->GetFaceNodeSkinned()) {
                //logger::info("[TIMER] Sucesso! 3D pronto para {:08X}. Aplicando DeformFace...", actorID);
                Manager::DeformFaceToMatchNif(pActor, nifPath);
            }
            else {
                // Tenta novamente
                if (retries % 5 == 0) logger::info("[TIMER] Ator {:08X} ainda não está pronto. Tentativas restantes: {}", actorID, retries);
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
