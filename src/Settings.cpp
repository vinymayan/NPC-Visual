#include "Settings.h"
#include "Manager.h"

const char* BasePath = "Data/SKSE/Plugins/NPC Replacer";
const char* NPCPath = "Data/SKSE/Plugins/NPC Replacer/NPC";
const char* PresetsPath = "Data/SKSE/Plugins/NPC Replacer/Presets";

// Variaveis de estado para a UI
static RE::Actor* g_currentActor = nullptr;
static RE::TESNPC* g_currentNPC = nullptr;

static bool isEditingPreset = false;
static std::string activePresetName = "";

// Atributos base
static float ui_height = 1.0f;
static float ui_bodyColor[4] = { 1.0f, 1.0f, 1.0f, 0.0f }; // R, G, B, A (QNAM restaurado para 4 valores)
static bool ui_isFemale = false;
static bool ui_oppositeGenderAnim = false;

// Morphs (NAM9 tem 19 valores)
static float ui_morphs[19] = { 0.0f };
static const char* morphNames[18] = {
    "Nose: Long/Short", "Nose: Up/Down", "Jaw: Up/Down", "Jaw: Narrow/Wide", "Jaw: Forward/Back",
    "Cheeks: Up/Down", "Cheeks: Forward/Back", "Eyes: Up/Down", "Eyes: In/Out", "Brows: Up/Down",
    "Brows: In/Out", "Brows: Forward/Back", "Lips: Up/Down", "Lips: In/Out", "Chin: Narrow/Wide",
    "Chin: Up/Down", "Chin: Underbite/Overbite", "Eyes: Forward/Back"
};

// Ponteiros para Dropdowns
static RE::TESRace* ui_race = nullptr;
static RE::TESObjectARMO* ui_skin = nullptr;
static RE::BGSOutfit* ui_outfit = nullptr;
static RE::BGSColorForm* ui_hairColor = nullptr;
static RE::BGSOutfit* ui_sleepOutfit = nullptr;

static std::vector<RE::BGSHeadPart*> ui_headParts;

struct UITintLayer {
    uint16_t index;
    std::string name; // Guarda o nome do SkinTone (ex: "Lip Color")
    float color[4];   // r, g, b, a (TintLayers exigem Alpha)
    float interpolation;
    uint16_t preset;
};

struct AvailableTint {
    uint16_t index;
    std::string name;
};

static std::vector<UITintLayer> ui_tintLayers;

// ==========================================
// PRESETS E BACKUP DE ESTADO (REVERTER)
// ==========================================
static std::string ui_linkedPreset = "";
static std::vector<std::string> ui_availablePresets;
static rapidjson::Document originalEngineState;

// ==========================================
// CONTROLADOR DE ALTERAÇÕES NÃO GUARDADAS
// ==========================================
static std::string g_lastSavedStateStr = "";
static std::string g_lastSavedPresetLink = "";


// ==========================================
// FUNÇÕES DE EXPORTAÇÃO ZIP (MINIZ)
// ==========================================
std::string SanitizeFilename(std::string name) {
    std::string invalid = "<>:/\\|?*\"";
    for (char& c : name) {
        if (invalid.find(c) != std::string::npos) c = '_';
    }
    return name;
}

void ExportPresetAsZip(const std::string& presetName) {
    namespace fs = std::filesystem;
    std::string sourcePath = std::format("{}/{}.json", PresetsPath, presetName);
    if (!fs::exists(sourcePath)) return;

    // PONTO 4: Pasta padrão de exports
    fs::path exportDir = "Data/Exports";
    fs::create_directories(exportDir);

    std::string zipPath = (exportDir / (SanitizeFilename(presetName) + "_Preset.zip")).string();

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) {
        logger::error("Export: Failed to initialize ZIP file at {}", zipPath);
        return;
    }

    std::string internalZipPath = std::format("SKSE/Plugins/NPC Replacer/Presets/{}.json", presetName);

    if (!mz_zip_writer_add_file(&zip_archive, internalZipPath.c_str(), sourcePath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION)) {
        logger::error("Export: Failed to add file {} to ZIP", presetName);
        mz_zip_writer_finalize_archive(&zip_archive);
        mz_zip_writer_end(&zip_archive);
        return;
    }

    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);
    logger::info("Preset '{}' successfully exported to: {}", presetName, zipPath);
}

void ExportNPCAsZip(RE::TESNPC* npc, const std::string& linkedPreset) {
    if (!npc) return;
    namespace fs = std::filesystem;

    std::string editorID = clib_util::editorID::get_editorID(npc);
    if (editorID.empty()) editorID = std::format("{:08X}", npc->GetFormID());
    std::string npcSourcePath = std::format("{}/{}.json", NPCPath, editorID);

    if (!fs::exists(npcSourcePath)) return;

    // PONTO 4: Pasta padrão de exports
    fs::path exportDir = "Data/Exports";
    fs::create_directories(exportDir);

    std::string npcName = npc->GetFullName() ? npc->GetFullName() : editorID;
    std::string zipPath = (exportDir / (SanitizeFilename(npcName) + "_NPC.zip")).string();

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) {
        logger::error("Export: Failed to initialize ZIP file at {}", zipPath);
        return;
    }

    // Adiciona o JSON do NPC no ZIP
    std::string internalNpcPath = std::format("SKSE/Plugins/NPC Replacer/NPC/{}.json", editorID);
    mz_zip_writer_add_file(&zip_archive, internalNpcPath.c_str(), npcSourcePath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION);

    // PONTO 3: Se tiver um Preset linkado, exporta ele junto no mesmo ZIP
    if (!linkedPreset.empty()) {
        std::string presetSourcePath = std::format("{}/{}.json", PresetsPath, linkedPreset);
        if (fs::exists(presetSourcePath)) {
            std::string internalPresetPath = std::format("SKSE/Plugins/NPC Replacer/Presets/{}.json", linkedPreset);
            mz_zip_writer_add_file(&zip_archive, internalPresetPath.c_str(), presetSourcePath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION);
        }
    }

    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);
    logger::info("NPC '{}' successfully exported to: {}", editorID, zipPath);
}

bool IsHeadPartValid(RE::BGSHeadPart* hp, RE::TESRace* race, bool isFemale) {
    if (!hp) return true;

    // Checa o Sexo
    bool hpFemale = hp->flags.all(RE::BGSHeadPart::Flag::kFemale);
    bool hpMale = hp->flags.all(RE::BGSHeadPart::Flag::kMale);
    if (isFemale && hpMale && !hpFemale) return false;
    if (!isFemale && hpFemale && !hpMale) return false;

    // Checa a Raça
    if (race && hp->validRaces) {
        if (!hp->validRaces->HasForm(race)) return false;
    }
    return true;
}

// Função Auxiliar para descobrir o que o Index do Tint significa baseado na Raça e Sexo
std::string GetSkinToneName(RE::TESRace* race, RE::SEX sex, uint16_t index) {
    if (!race) return "Unknown";

    // Fallback pra male (0) ou female (1) 
    size_t sexIdx = static_cast<size_t>(sex);
    if (sexIdx >= 2) sexIdx = 0;

    auto faceData = race->faceRelatedData[sexIdx];
    if (!faceData || !faceData->tintMasks) return "Unknown";

    for (std::uint32_t i = 0; i < faceData->tintMasks->size(); ++i) {
        auto tintAsset = (*faceData->tintMasks)[i];
        if (tintAsset && tintAsset->texture.index == index) {
            switch (tintAsset->texture.skinTone.get()) {
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kLipColor: return "Lip Color";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kCheekColor: return "Cheek Color";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kEyeliner: return "Eyeliner";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kEyeSocketUpper: return "Eye Socket Upper";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kEyeSocketLower: return "Eye Socket Lower";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kSkinTone: return "Skin Tone";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kPaint: return "Paint";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kLaughLines: return "Laugh Lines";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kCheekColorLower: return "Cheek Color Lower";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kNose: return "Nose";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kChin: return "Chin";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kNeck: return "Neck";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kForehead: return "Forehead";
            case RE::TESRace::FaceRelatedData::TintAsset::TintLayer::SkinTone::kDirt: return "Dirt";
            default: return "Custom/Misc";
            }
        }
    }
    return "Not Found";
}

std::vector<AvailableTint> GetAvailableTints(RE::TESRace* race, bool isFemale) {
    std::vector<AvailableTint> result;
    if (!race) return result;

    size_t sexIdx = isFemale ? 1 : 0;
    auto faceData = race->faceRelatedData[sexIdx];
    if (!faceData || !faceData->tintMasks) return result;

    for (std::uint32_t i = 0; i < faceData->tintMasks->size(); ++i) {
        auto tintAsset = (*faceData->tintMasks)[i];
        if (tintAsset) {
            AvailableTint t;
            t.index = tintAsset->texture.index;
            t.name = GetSkinToneName(race, isFemale ? RE::SEX::kFemale : RE::SEX::kMale, t.index);
            result.push_back(t);
        }
    }
    return result;
}

// Retorna os Presets disponíveis na pasta
void RefreshAvailablePresets() {
    ui_availablePresets.clear();
    std::filesystem::create_directories(PresetsPath);
    for (const auto& entry : std::filesystem::directory_iterator(PresetsPath)) {
        if (entry.path().extension() == ".json") {
            ui_availablePresets.push_back(entry.path().stem().string());
        }
    }
}

void GenerateJSONFromUI(rapidjson::Document& doc) {
    auto& allocator = doc.GetAllocator();
    doc.SetObject();

    doc.AddMember("height", ui_height, allocator);
    doc.AddMember("isFemale", ui_isFemale, allocator);
    doc.AddMember("oppositeGenderAnim", ui_oppositeGenderAnim, allocator);

    rapidjson::Value bodyTint(rapidjson::kObjectType);
    bodyTint.AddMember("r", static_cast<int>(ui_bodyColor[0] * 255.0f), allocator);
    bodyTint.AddMember("g", static_cast<int>(ui_bodyColor[1] * 255.0f), allocator);
    bodyTint.AddMember("b", static_cast<int>(ui_bodyColor[2] * 255.0f), allocator);
    bodyTint.AddMember("a", static_cast<int>(ui_bodyColor[3] * 255.0f), allocator);
    doc.AddMember("bodyTintColor", bodyTint, allocator);

    if (ui_race) doc.AddMember("race", rapidjson::Value(FormUtil::NormalizeFormID(ui_race).c_str(), allocator), allocator);
    if (ui_skin) doc.AddMember("skin", rapidjson::Value(FormUtil::NormalizeFormID(ui_skin).c_str(), allocator), allocator);
    if (ui_outfit) doc.AddMember("defaultOutfit", rapidjson::Value(FormUtil::NormalizeFormID(ui_outfit).c_str(), allocator), allocator);
    if (ui_sleepOutfit) doc.AddMember("sleepOutfit", rapidjson::Value(FormUtil::NormalizeFormID(ui_sleepOutfit).c_str(), allocator), allocator);
    if (ui_hairColor) doc.AddMember("hairColor", rapidjson::Value(FormUtil::NormalizeFormID(ui_hairColor).c_str(), allocator), allocator);

    rapidjson::Value hpArray(rapidjson::kArrayType);
    for (auto* hp : ui_headParts) {
        if (!hp) continue;
        std::string hpStr = FormUtil::NormalizeFormID(hp);
        hpArray.PushBack(rapidjson::Value(hpStr.c_str(), allocator), allocator);
    }
    doc.AddMember("headParts", hpArray, allocator);

    rapidjson::Value tintArray(rapidjson::kArrayType);
    for (auto& tl : ui_tintLayers) {
        rapidjson::Value tlVal(rapidjson::kObjectType);
        tlVal.AddMember("index", tl.index, allocator);
        rapidjson::Value cVal(rapidjson::kObjectType);
        cVal.AddMember("r", static_cast<int>(tl.color[0] * 255.0f), allocator);
        cVal.AddMember("g", static_cast<int>(tl.color[1] * 255.0f), allocator);
        cVal.AddMember("b", static_cast<int>(tl.color[2] * 255.0f), allocator);
        cVal.AddMember("a", static_cast<int>(tl.color[3] * 255.0f), allocator);
        tlVal.AddMember("color", cVal, allocator);
        tlVal.AddMember("interpolation", tl.interpolation, allocator);
        tlVal.AddMember("preset", tl.preset, allocator);
        tintArray.PushBack(tlVal, allocator);
    }
    doc.AddMember("tintLayers", tintArray, allocator);

    bool hasMorphs = false;
    for (int i = 0; i < 19; i++) {
        if (ui_morphs[i] >= -1.0f && ui_morphs[i] <= 1.0f) {
            hasMorphs = true;
            break;
        }
    }
    if (hasMorphs) {
        rapidjson::Value morphsArray(rapidjson::kArrayType);
        for (int i = 0; i < 19; i++) morphsArray.PushBack(ui_morphs[i], allocator);
        doc.AddMember("faceMorphs", morphsArray, allocator);
    }
}

void UpdateLastSavedState() {
    rapidjson::Document doc;
    GenerateJSONFromUI(doc);
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    g_lastSavedStateStr = buffer.GetString();
    g_lastSavedPresetLink = ui_linkedPreset;
}

bool HasUnsavedChanges() {
    if (ui_linkedPreset != g_lastSavedPresetLink) return true;

    rapidjson::Document doc;
    GenerateJSONFromUI(doc);
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return g_lastSavedStateStr != buffer.GetString();
}

void ParseJSONToUI(const rapidjson::Document& j) {
    // Limpa os dados de ponteiro primeiro para evitar lixo de memória / estado anterior
    ui_race = nullptr;
    ui_skin = nullptr;
    ui_outfit = nullptr;
    ui_sleepOutfit = nullptr;
    ui_hairColor = nullptr;

    if (j.HasMember("height") && j["height"].IsFloat()) ui_height = j["height"].GetFloat();
    if (j.HasMember("isFemale") && j["isFemale"].IsBool()) ui_isFemale = j["isFemale"].GetBool();
    if (j.HasMember("oppositeGenderAnim") && j["oppositeGenderAnim"].IsBool()) ui_oppositeGenderAnim = j["oppositeGenderAnim"].GetBool();

    if (j.HasMember("bodyTintColor")) {
        auto& c = j["bodyTintColor"];
        ui_bodyColor[0] = (c.HasMember("r") ? c["r"].GetInt() : 255) / 255.0f;
        ui_bodyColor[1] = (c.HasMember("g") ? c["g"].GetInt() : 255) / 255.0f;
        ui_bodyColor[2] = (c.HasMember("b") ? c["b"].GetInt() : 255) / 255.0f;
        ui_bodyColor[3] = (c.HasMember("a") ? c["a"].GetInt() : 0) / 255.0f;
    }

    if (j.HasMember("race")) ui_race = RE::TESForm::LookupByID<RE::TESRace>(FormUtil::FormIDFromString(j["race"].GetString()));
    if (j.HasMember("skin")) ui_skin = RE::TESForm::LookupByID<RE::TESObjectARMO>(FormUtil::FormIDFromString(j["skin"].GetString()));
    if (j.HasMember("defaultOutfit")) ui_outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(FormUtil::FormIDFromString(j["defaultOutfit"].GetString()));
    if (j.HasMember("sleepOutfit")) ui_sleepOutfit = RE::TESForm::LookupByID<RE::BGSOutfit>(FormUtil::FormIDFromString(j["sleepOutfit"].GetString()));
    if (j.HasMember("hairColor")) ui_hairColor = RE::TESForm::LookupByID<RE::BGSColorForm>(FormUtil::FormIDFromString(j["hairColor"].GetString()));

    ui_headParts.clear();
    if (j.HasMember("headParts") && j["headParts"].IsArray()) {
        for (auto& hpJson : j["headParts"].GetArray()) {
            if (auto hp = RE::TESForm::LookupByID<RE::BGSHeadPart>(FormUtil::FormIDFromString(hpJson.GetString()))) {
                ui_headParts.push_back(hp);
            }
        }
    }

    ui_tintLayers.clear();
    if (j.HasMember("tintLayers") && j["tintLayers"].IsArray()) {
        for (auto& l : j["tintLayers"].GetArray()) {
            UITintLayer tl;
            tl.index = static_cast<uint16_t>(l["index"].GetInt());
            tl.name = GetSkinToneName(ui_race, ui_isFemale ? RE::SEX::kFemale : RE::SEX::kMale, tl.index);
            auto& colorObj = l["color"];
            tl.color[0] = (colorObj.HasMember("r") ? colorObj["r"].GetInt() : 0) / 255.0f;
            tl.color[1] = (colorObj.HasMember("g") ? colorObj["g"].GetInt() : 0) / 255.0f;
            tl.color[2] = (colorObj.HasMember("b") ? colorObj["b"].GetInt() : 0) / 255.0f;
            tl.color[3] = (colorObj.HasMember("a") ? colorObj["a"].GetInt() : 255) / 255.0f;
            tl.interpolation = l["interpolation"].GetFloat();
            tl.preset = static_cast<uint16_t>(l["preset"].GetInt());
            ui_tintLayers.push_back(tl);
        }
    }

    if (j.HasMember("faceMorphs") && j["faceMorphs"].IsArray()) {
        auto mArray = j["faceMorphs"].GetArray();
        for (rapidjson::SizeType i = 0; i < mArray.Size() && i < 19; i++) ui_morphs[i] = mArray[i].GetFloat();
    }
    else {
        // Inicializa com o valor máximo indicando que está desabilitado
        for (int i = 0; i < 19; i++) ui_morphs[i] = 3.402823466e+38f;
    }
}

template <typename T>
void DrawDropdown(const char* label, const std::string& category, T** formPtr, int& selectedIndex, bool disabled = false, bool filterRaceSex = false, float customWidth = -1.0f) {
    const auto& fullList = Manager::GetSingleton()->GetList(category);
    if (fullList.empty()) return;

    std::vector<const char*> comboItems;
    std::vector<int> mapToFull;

    comboItems.push_back("None");
    mapToFull.push_back(-1);

    for (size_t i = 0; i < fullList.size(); ++i) {
        if (filterRaceSex) {
            auto form = RE::TESForm::LookupByID(fullList[i].formID);
            if (form && form->Is(RE::FormType::HeadPart)) {
                if (!IsHeadPartValid(form->As<RE::BGSHeadPart>(), ui_race, ui_isFemale)) continue;
            }
        }
        comboItems.push_back(fullList[i].cachedDisplayName.c_str());
        mapToFull.push_back(static_cast<int>(i));
    }

    int localSelection = 0;
    if (*formPtr) {
        RE::FormID targetID = (*formPtr)->GetFormID();
        for (size_t i = 1; i < mapToFull.size(); i++) {
            if (fullList[mapToFull[i]].formID == targetID) {
                localSelection = static_cast<int>(i);
                break;
            }
        }
    }

    ImGuiMCP::PushID(label);

    // CORREÇÃO 2: Pega o Label, acha o "##" e oculta o ID da frente do texto visual
    std::string displayLabel = label;
    size_t hashPos = displayLabel.find("##");
    if (hashPos != std::string::npos) {
        displayLabel = displayLabel.substr(0, hashPos);
    }
    ImGuiMCP::Text("%s:", displayLabel.c_str());
    ImGuiMCP::SameLine();

    if (disabled) {
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "[LOCKED] %s", comboItems.empty() ? "None" : comboItems[localSelection]);
    }
    else {
        // PONTO 3: Aplica a largura customizada na caixa do dropdown
        if (customWidth > 0.0f) ImGuiMCP::SetNextItemWidth(customWidth);

        const char* previewValue = comboItems.empty() ? "None" : comboItems[localSelection];

        if (ImGuiMCP::BeginCombo("##drop", previewValue)) {
            // PONTO 4: Campo de pesquisa dinâmico para os itens do dropdown
            static std::map<std::string, std::string> searchBuffers;
            char searchBuf[256] = "";
            if (searchBuffers.contains(label)) strcpy_s(searchBuf, searchBuffers[label].c_str());

            ImGuiMCP::SetNextItemWidth(-1.0f); // Ocupa toda a largura do combo popup
            if (ImGuiMCP::InputText("##busca", searchBuf, sizeof(searchBuf))) {
                searchBuffers[label] = searchBuf;
            }
            ImGuiMCP::Separator();

            std::string searchStr = searchBuf;
            std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

            ImGuiMCP::BeginChild("##scroll", ImGuiMCP::ImVec2(0, 200), false);
            for (int i = 0; i < comboItems.size(); i++) {
                std::string itemStr = comboItems[i];
                std::string itemLower = itemStr;
                std::transform(itemLower.begin(), itemLower.end(), itemLower.begin(), ::tolower);

                // Mostra apenas itens que contém o que foi digitado
                if (searchStr.empty() || itemLower.find(searchStr) != std::string::npos) {
                    bool isSelected = (localSelection == i);
                    if (ImGuiMCP::Selectable(comboItems[i], isSelected)) {
                        localSelection = i;
                        int originalIndex = mapToFull[localSelection];
                        if (originalIndex == -1) *formPtr = nullptr;
                        else *formPtr = RE::TESForm::LookupByID<T>(fullList[originalIndex].formID);

                        searchBuffers[label] = ""; // Limpa a busca ao selecionar
                    }
                    if (isSelected) ImGuiMCP::SetItemDefaultFocus();
                }
            }
            ImGuiMCP::EndChild();
            ImGuiMCP::EndCombo();
        }
    }
    ImGuiMCP::PopID();
}
void LoadPresetToUI(const std::string& presetName) {
    isEditingPreset = true;
    activePresetName = presetName;
    ui_linkedPreset = "";

    std::string path = std::format("{}/{}.json", PresetsPath, presetName);
    FILE* fp = nullptr;
    fopen_s(&fp, path.c_str(), "rb");
    if (fp) {
        char readBuffer[65536];
        rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
        rapidjson::Document doc;
        doc.ParseStream(is);
        fclose(fp);
        ParseJSONToUI(doc);
        logger::info("Preset {} loaded into UI.", presetName);
    }

    UpdateLastSavedState(); // Atualiza a "fotografia" do estado guardado
}

void LoadNPCToUI(RE::TESNPC* npcToLoad = nullptr, RE::Actor* actorRef = nullptr) {
    if (!npcToLoad) {
        auto ref = RE::Console::GetSelectedRef();
        if (!ref) { logger::warn("No NPC selected."); return; }
        g_currentActor = ref->As<RE::Actor>();
        if (!g_currentActor) return;
        g_currentNPC = g_currentActor->GetActorBase();
    }
    else {
        g_currentNPC = npcToLoad;
        g_currentActor = actorRef;
    }

    if (!g_currentNPC) return;

    isEditingPreset = false;
    activePresetName = "";
    ui_linkedPreset = "";

    ui_height = g_currentNPC->height;
    ui_isFemale = g_currentNPC->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kFemale);
    ui_oppositeGenderAnim = g_currentNPC->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kOppositeGenderAnims);

    ui_bodyColor[0] = g_currentNPC->bodyTintColor.red / 255.0f;
    ui_bodyColor[1] = g_currentNPC->bodyTintColor.green / 255.0f;
    ui_bodyColor[2] = g_currentNPC->bodyTintColor.blue / 255.0f;
    ui_bodyColor[3] = g_currentNPC->bodyTintColor.alpha / 255.0f;

    ui_race = g_currentNPC->race;
    ui_skin = g_currentNPC->farSkin;
    ui_outfit = g_currentNPC->defaultOutfit;
    ui_sleepOutfit = g_currentNPC->sleepOutfit;
    ui_hairColor = g_currentNPC->headRelatedData ? g_currentNPC->headRelatedData->hairColor : nullptr;

    ui_headParts.clear();

    for (int i = 0; i < g_currentNPC->numHeadParts; i++) {
        if (g_currentNPC->headParts && g_currentNPC->headParts[i]) ui_headParts.push_back(g_currentNPC->headParts[i]);
    }

    ui_tintLayers.clear();
    if (g_currentNPC->tintLayers) {
        for (std::uint32_t i = 0; i < g_currentNPC->tintLayers->size(); ++i) {
            auto layer = (*g_currentNPC->tintLayers)[i];
            if (layer) {
                UITintLayer uiLayer;
                uiLayer.index = layer->tintIndex;
                uiLayer.name = GetSkinToneName(g_currentNPC->race, g_currentNPC->GetSex(), layer->tintIndex);
                uiLayer.color[0] = layer->tintColor.red / 255.0f; uiLayer.color[1] = layer->tintColor.green / 255.0f;
                uiLayer.color[2] = layer->tintColor.blue / 255.0f; uiLayer.color[3] = layer->tintColor.alpha / 255.0f;
                uiLayer.interpolation = layer->interpolationValue / 100.0f;
                uiLayer.preset = layer->preset;
                ui_tintLayers.push_back(uiLayer);
            }
        }
    }

    if (g_currentNPC->faceData) {
        for (int i = 0; i < 19; i++) ui_morphs[i] = g_currentNPC->faceData->morphs[i];
    }
    else {
        for (int i = 0; i < 19; i++) ui_morphs[i] = 3.402823466e+38f;
    }

    GenerateJSONFromUI(originalEngineState);

    std::string editorID = clib_util::editorID::get_editorID(g_currentNPC);
    if (editorID.empty()) editorID = std::format("{:08X}", g_currentNPC->GetFormID());
    std::string filePath = std::format("{}/{}.json", NPCPath, editorID);

    if (std::filesystem::exists(filePath)) {
        FILE* fp = nullptr;
        fopen_s(&fp, filePath.c_str(), "rb");
        if (fp) {
            char readBuffer[65536];
            rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
            rapidjson::Document doc;
            doc.ParseStream(is);
            fclose(fp);
            if (doc.IsObject() && doc.HasMember("preset") && doc["preset"].IsString()) {
                ui_linkedPreset = doc["preset"].GetString();
            }
        }
    }

    UpdateLastSavedState(); // Atualiza a "fotografia" do estado guardado
}

void SaveData() {
    rapidjson::Document doc;
    auto& allocator = doc.GetAllocator();
    doc.SetObject();

    std::string finalPath;

    if (isEditingPreset) {
        GenerateJSONFromUI(doc);
        std::filesystem::create_directories(PresetsPath);
        finalPath = std::format("{}/{}.json", PresetsPath, activePresetName);
    }
    else {
        if (!g_currentNPC) return;
        if (!ui_linkedPreset.empty()) {
            doc.AddMember("preset", rapidjson::Value(ui_linkedPreset.c_str(), allocator), allocator);
        }
        else {
            GenerateJSONFromUI(doc);
        }
        std::string editorID = clib_util::editorID::get_editorID(g_currentNPC);
        if (editorID.empty()) editorID = std::format("{:08X}", g_currentNPC->GetFormID());
        std::filesystem::create_directories(NPCPath);
        finalPath = std::format("{}/{}.json", NPCPath, editorID);
    }

    FILE* fp = nullptr;
    fopen_s(&fp, finalPath.c_str(), "wb");
    if (fp) {
        char writeBuffer[65536];
        rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
        rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
        doc.Accept(writer);
        fclose(fp);
        logger::info("Data saved to {}", finalPath);
    }

    if (isEditingPreset && !activePresetName.empty()) {
        if (std::filesystem::exists(NPCPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(NPCPath)) {
                if (entry.path().extension() == ".json") {
                    FILE* nFp = nullptr;
                    fopen_s(&nFp, entry.path().string().c_str(), "rb");
                    if (nFp) {
                        char readBuffer[2048];
                        rapidjson::FileReadStream is(nFp, readBuffer, sizeof(readBuffer));
                        rapidjson::Document npcDoc;
                        npcDoc.ParseStream(is);
                        fclose(nFp);

                        // Checa se este NPC está atrelado ao preset que acabamos de salvar
                        if (npcDoc.IsObject() && npcDoc.HasMember("preset") && npcDoc["preset"].IsString()) {
                            if (npcDoc["preset"].GetString() == activePresetName) {

                                std::string filename = entry.path().stem().string();
                                RE::TESNPC* targetNPC = nullptr;

                                // Tenta achar o NPC pelo EditorID ou FormID
                                if (auto edidForm = RE::TESForm::LookupByEditorID(filename)) {
                                    targetNPC = edidForm->As<RE::TESNPC>();
                                }
                                else {
                                    try {
                                        RE::FormID id = std::stoul(filename, nullptr, 16);
                                        if (auto idForm = RE::TESForm::LookupByID(id)) targetNPC = idForm->As<RE::TESNPC>();
                                    }
                                    catch (...) {}
                                }

                                // Se achou o NPC na memória do jogo, aplica os dados novos do preset ('doc') nele
                                if (targetNPC) {
                                    Manager::ApplyNPCCustomizationFromJSON(targetNPC, doc);
                                    logger::info("Updated NPC base {} with modified preset '{}'", filename, activePresetName);

                                    auto processLists = RE::ProcessLists::GetSingleton();
                                    if (processLists) {
                                        for (auto& actorHandle : processLists->highActorHandles) {
                                            auto ref = actorHandle.get();
                                            if (ref) {
                                                if (auto actor = ref->As<RE::Actor>()) {
                                                    if (!actor->IsPlayerRef() && actor->GetActorBase() == targetNPC) {
                                                        actor->UpdateSkinColor();
                                                        actor->UpdateHairColor();
                                                        actor->Update3DModel();
                                                        actor->DoReset3D(true);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    UpdateLastSavedState();
}

void ApplyNPC(bool force3DReset = true) {
    if (!g_currentNPC) return;
    rapidjson::Document doc;

    if (!ui_linkedPreset.empty()) {
        // Se usar Preset, aplica os dados do Preset
        std::string pPath = std::format("{}/{}.json", PresetsPath, ui_linkedPreset);
        FILE* fp = nullptr;
        fopen_s(&fp, pPath.c_str(), "rb");
        if (fp) {
            char readBuffer[65536];
            rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
            doc.ParseStream(is);
            fclose(fp);
        }
    }
    else {
        GenerateJSONFromUI(doc);
    }

    Manager::ApplyNPCCustomizationFromJSON(g_currentNPC, doc);

    if (g_currentActor && force3DReset) {
        g_currentActor->UpdateSkinColor();
        g_currentActor->UpdateHairColor();
        g_currentActor->Update3DModel();
        g_currentActor->DoReset3D(true);
    }
}

// Botão "Voltar ao Padrão"
void RevertNPC() {
    if (!g_currentNPC || !originalEngineState.IsObject()) return;

    // Volta UI e Motor para o que estava gravado no Backup quando lemos o NPC
    ui_linkedPreset = "";
    Manager::ApplyNPCCustomizationFromJSON(g_currentNPC, originalEngineState);

    // Recarrega a UI baseada na engine recém-restaurada
    LoadNPCToUI(g_currentNPC, g_currentActor);

    if (g_currentActor) {
        g_currentActor->UpdateSkinColor();
        g_currentActor->UpdateHairColor();
        g_currentActor->Update3DModel();
        g_currentActor->DoReset3D(true);
    }
}

void DrawMainEditorUI() {
    bool isLocked = (!ui_linkedPreset.empty() && !isEditingPreset);
    bool hasErrors = false;
    std::string errorMsgs = "";

    // Deteta se o utilizador alterou qualquer coisa no ecrã
    bool isDirty = HasUnsavedChanges();

    std::map<RE::BGSHeadPart::HeadPartType, int> hpCounts;
    for (auto hp : ui_headParts) {
        if (!hp) continue;
        hpCounts[hp->type.get()]++;
        if (!IsHeadPartValid(hp, ui_race, ui_isFemale)) {
            hasErrors = true;
            errorMsgs += std::format("- Incompatible part detected: {}\n", clib_util::editorID::get_editorID(hp));
        }
    }
    for (auto& pair : hpCounts) {
        if (pair.second > 1 && pair.first != RE::BGSHeadPart::HeadPartType::kMisc && pair.first != RE::BGSHeadPart::HeadPartType::kScar) {
            hasErrors = true;
            errorMsgs += "- Multiple HeadParts of the same unique type found!\n";
            break;
        }
    }

    if (hasErrors) {
        ImGuiMCP::TextColored({ 1.0f, 0.2f, 0.2f, 1.0f }, "ERROR: Resolve the red incompatibilities to save!");
        if (!errorMsgs.empty()) ImGuiMCP::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, errorMsgs.c_str());
    }
    else {
        if (!isEditingPreset && ImGuiMCP::Button("Apply in-game")) { ApplyNPC(); }
        ImGuiMCP::SameLine();

        // Dá uma cor de aviso ao botão de "SALVAR" se estiver "sujo"
        if (isDirty) ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, { 0.6f, 0.4f, 0.1f, 1.0f });
        if (ImGuiMCP::Button("Save data")) { SaveData(); }
        if (isDirty) ImGuiMCP::PopStyleColor();

        if (!isEditingPreset) {
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Revert to default")) { RevertNPC(); }

            if (g_currentNPC) {
                std::string editorID = clib_util::editorID::get_editorID(g_currentNPC);
                if (editorID.empty()) editorID = std::format("{:08X}", g_currentNPC->GetFormID());
                std::string filePath = std::format("{}/{}.json", NPCPath, editorID);

                if (std::filesystem::exists(filePath)) {
                    ImGuiMCP::SameLine();

                    // Bloqueio do botão caso tenha edições por guardar
                    if (isDirty) {
                        ImGuiMCP::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "[Save to Export]");
                        if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("There are pending changes. Click Save data first.");
                    }
                    else {
                        if (ImGuiMCP::Button("Export")) {
                            ExportNPCAsZip(g_currentNPC, ui_linkedPreset);
                        }
                        if (ImGuiMCP::IsItemHovered()) {
                            ImGuiMCP::SetTooltip("The .zip file will be saved in the folder:\nData/Exports");
                        }
                    }
                }
            }
        }
        else if (isEditingPreset && !activePresetName.empty()) {
            std::string presetPath = std::format("{}/{}.json", PresetsPath, activePresetName);
            if (std::filesystem::exists(presetPath)) {
                ImGuiMCP::SameLine();

                // Bloqueio do botão caso tenha edições por guardar
                if (isDirty) {
                    ImGuiMCP::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "[Save to Export]");
                    if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("There are pending changes. Click SAVE DATA first.");
                }
                else {
                    if (ImGuiMCP::Button("Export")) {
                        ExportPresetAsZip(activePresetName);
                    }
                    if (ImGuiMCP::IsItemHovered()) {
                        ImGuiMCP::SetTooltip("The .zip file will be saved in the folder:\nData/Exports");
                    }
                }
            }
        }
    }
    ImGuiMCP::Separator();

    if (isLocked) {
        ImGuiMCP::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f }, "This NPC is locked and using PRESET: %s", ui_linkedPreset.c_str());
        if (ImGuiMCP::Button("Unlink Preset (Free Edit)")) ui_linkedPreset = "";

        // PONTO 1: Botão para Editar o Preset linkado
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Edit this Preset")) {
            LoadPresetToUI(ui_linkedPreset);
        }
        ImGuiMCP::Separator();
    }
    else if (!isEditingPreset) {
        RefreshAvailablePresets();

        static char newPresetName[128] = "";
        static bool nameExistsError = false;

        ImGuiMCP::SetNextItemWidth(250.0f);
        ImGuiMCP::InputText("New Preset Name", newPresetName, sizeof(newPresetName));
        ImGuiMCP::SameLine();

        if (ImGuiMCP::Button("Save as Preset")) {
            if (strlen(newPresetName) > 0) {
                std::string newName(newPresetName);

                if (std::find(ui_availablePresets.begin(), ui_availablePresets.end(), newName) != ui_availablePresets.end()) {
                    nameExistsError = true;
                }
                else {
                    nameExistsError = false;
                    std::string backupName = activePresetName;
                    bool backupEdit = isEditingPreset;

                    activePresetName = newPresetName;
                    isEditingPreset = true;
                    SaveData();

                    ui_linkedPreset = newPresetName;
                    isEditingPreset = false;
                    activePresetName = "";
                    SaveData();

                    isEditingPreset = backupEdit;
                    activePresetName = backupName;
                    memset(newPresetName, 0, sizeof(newPresetName));
                }
            }
        }

        if (nameExistsError) {
            ImGuiMCP::TextColored({ 1.0f, 0.2f, 0.2f, 1.0f }, "Error: A Preset with this name already exists!");
        }

        if (!ui_availablePresets.empty()) {
            static int selPreset = 0;
            if (selPreset >= ui_availablePresets.size()) selPreset = 0;

            std::vector<const char*> presetCStr;
            for (auto& p : ui_availablePresets) presetCStr.push_back(p.c_str());

            ImGuiMCP::Text("Apply Existing Preset:"); ImGuiMCP::SameLine();
            ImGuiMCP::SetNextItemWidth(250.0f);
            ImGuiMCP::Combo("##presetCombo", &selPreset, presetCStr.data(), static_cast<int>(presetCStr.size()));
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Apply and Link Preset")) {
                std::string chosenPreset = ui_availablePresets[selPreset];
                std::string pPath = std::format("{}/{}.json", PresetsPath, chosenPreset);
                FILE* fp = nullptr;
                fopen_s(&fp, pPath.c_str(), "rb");
                if (fp) {
                    char readBuffer[65536];
                    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
                    rapidjson::Document doc;
                    doc.ParseStream(is);
                    fclose(fp);
                    ParseJSONToUI(doc);
                }
                ui_linkedPreset = chosenPreset;
                SaveData();
                ApplyNPC();
            }
        }
        ImGuiMCP::Separator();
    }

    ImGuiMCP::Text("Basic Attributes");
    if (isLocked) ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "[LOCKED]");
    else {
        ImGuiMCP::SetNextItemWidth(200.0f);
        ImGuiMCP::InputFloat("Height", &ui_height, 0.01f, 0.1f, "%.3f");
        ImGuiMCP::ColorEdit4("Body Color (QNAM)", ui_bodyColor);
        ImGuiMCP::Checkbox("Female", &ui_isFemale);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox("Opposite Gender Anims", &ui_oppositeGenderAnim);
    }

    static int s_raceIdx = 0, s_skinIdx = 0, s_outfitIdx = 0, s_sleepOutfitIdx = 0, s_hairColorIdx = 0;
    DrawDropdown("Race", "Race", &ui_race, s_raceIdx, isLocked);
    DrawDropdown("Worn Skin", "Armor", &ui_skin, s_skinIdx, isLocked);
    DrawDropdown("Default Outfit", "Outfit", &ui_outfit, s_outfitIdx, isLocked);
    DrawDropdown("Sleep Outfit", "Outfit", &ui_sleepOutfit, s_sleepOutfitIdx, isLocked);
    DrawDropdown("Hair Color", "ColorForm", &ui_hairColor, s_hairColorIdx, isLocked);

    ImGuiMCP::Separator();

    if (!isLocked) {
        ImGuiMCP::Text("Current HeadParts:");
        if (ImGuiMCP::BeginTable("HeadPartsTable", 3, ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable)) {
            ImGuiMCP::TableSetupColumn("Action", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGuiMCP::TableSetupColumn("Type", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn("Name / EditorID", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableHeadersRow();

            int to_remove_hp = -1;
            for (size_t i = 0; i < ui_headParts.size(); i++) {
                auto hp = ui_headParts[i];
                bool hpValid = IsHeadPartValid(hp, ui_race, ui_isFemale);

                ImGuiMCP::TableNextRow();
                ImGuiMCP::PushID(static_cast<int>(i));

                ImGuiMCP::TableSetColumnIndex(0);
                if (ImGuiMCP::Button(" X ")) to_remove_hp = static_cast<int>(i);

                ImGuiMCP::TableSetColumnIndex(1);
                const char* tipoStr = "Unknown";
                if (hp) {
                    switch (hp->type.get()) {
                    case RE::BGSHeadPart::HeadPartType::kHair: tipoStr = "Hair"; break;
                    case RE::BGSHeadPart::HeadPartType::kEyes: tipoStr = "Eyes"; break;
                    case RE::BGSHeadPart::HeadPartType::kFace: tipoStr = "Face"; break;
                    case RE::BGSHeadPart::HeadPartType::kEyebrows: tipoStr = "Eyebrows"; break;
                    case RE::BGSHeadPart::HeadPartType::kFacialHair: tipoStr = "Facial Hair"; break;
                    case RE::BGSHeadPart::HeadPartType::kScar: tipoStr = "Scar"; break;
                    case RE::BGSHeadPart::HeadPartType::kMisc: tipoStr = "Misc"; break;
                    }
                }
                if (!hpValid) ImGuiMCP::TextColored({ 1.0f, 0.2f, 0.2f, 1.0f }, "%s", tipoStr);
                else ImGuiMCP::Text("%s", tipoStr);

                ImGuiMCP::TableSetColumnIndex(2);
                if (!hpValid) ImGuiMCP::TextColored({ 1.0f, 0.2f, 0.2f, 1.0f }, "[INCOMPATIBLE] %s", hp ? clib_util::editorID::get_editorID(hp).c_str() : "Null");
                else ImGuiMCP::Text("%s", hp ? clib_util::editorID::get_editorID(hp).c_str() : "Null");

                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndTable();
            if (to_remove_hp != -1) ui_headParts.erase(ui_headParts.begin() + to_remove_hp);
        }

        ImGuiMCP::Text("Add New HeadPart:");
        static int s_hpCatIdx = 0;
        static const char* ui_categories[] = { "Hair", "Facial Hair", "Eye Brows", "Eye", "Face", "Misc", "Scar" };
        static RE::BGSHeadPart* newHp = nullptr;
        static int s_newHpIdx = 0;

        ImGuiMCP::SetNextItemWidth(200.0f);
        ImGuiMCP::Combo("Category##Add", &s_hpCatIdx, ui_categories, 7);

        DrawDropdown("Parts (Compatible)##Add", ui_categories[s_hpCatIdx], &newHp, s_newHpIdx, false, true, 340.0f);
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Add Part")) {
            if (newHp && std::find(ui_headParts.begin(), ui_headParts.end(), newHp) == ui_headParts.end()) {
                ui_headParts.push_back(newHp);
            }
        }

        ImGuiMCP::Separator();

        auto availableTints = GetAvailableTints(ui_race, ui_isFemale);
        std::vector<std::string> tintNamesStr;
        std::vector<const char*> tintNamesCStr;
        for (const auto& t : availableTints) {
            tintNamesStr.push_back(std::format("{} ({})", t.name, t.index));
        }
        for (const auto& s : tintNamesStr) tintNamesCStr.push_back(s.c_str());

        ImGuiMCP::Text("Tint Layers (Makeup, Skin, Dirt)");
        if (ImGuiMCP::BeginTable("TintTable", 5, ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_Resizable)) {
            ImGuiMCP::TableSetupColumn("Action", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGuiMCP::TableSetupColumn("Type", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Color", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGuiMCP::TableSetupColumn("Opacity", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn("Preset", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGuiMCP::TableHeadersRow();

            int to_remove_tint = -1;
            for (size_t i = 0; i < ui_tintLayers.size(); i++) {
                ImGuiMCP::PushID(static_cast<int>(i));
                ImGuiMCP::TableNextRow();

                ImGuiMCP::TableSetColumnIndex(0);
                if (ImGuiMCP::Button(" X ")) to_remove_tint = static_cast<int>(i);

                ImGuiMCP::TableSetColumnIndex(1);
                if (!availableTints.empty()) {
                    int current_sel = 0;
                    for (size_t j = 0; j < availableTints.size(); j++) {
                        if (availableTints[j].index == ui_tintLayers[i].index) {
                            current_sel = static_cast<int>(j); break;
                        }
                    }
                    ImGuiMCP::SetNextItemWidth(-1.0f);
                    if (ImGuiMCP::Combo("##tintcombo", &current_sel, tintNamesCStr.data(), static_cast<int>(tintNamesCStr.size()))) {
                        ui_tintLayers[i].index = availableTints[current_sel].index;
                        ui_tintLayers[i].name = availableTints[current_sel].name;
                    }
                }
                else {
                    ImGuiMCP::Text("%s (%d)", ui_tintLayers[i].name.c_str(), ui_tintLayers[i].index);
                }

                ImGuiMCP::TableSetColumnIndex(2);
                ImGuiMCP::ColorEdit4("##color", ui_tintLayers[i].color, ImGuiMCP::ImGuiColorEditFlags_NoInputs);

                ImGuiMCP::TableSetColumnIndex(3);
                ImGuiMCP::SetNextItemWidth(-1.0f);
                if (ImGuiMCP::InputFloat("##interp", &ui_tintLayers[i].interpolation, 0.0f, 0.0f, "%.2f")) {
                    if (ui_tintLayers[i].interpolation < 0.0f) ui_tintLayers[i].interpolation = 0.0f;
                    if (ui_tintLayers[i].interpolation > 1.0f) ui_tintLayers[i].interpolation = 1.0f;
                }

                ImGuiMCP::TableSetColumnIndex(4);
                ImGuiMCP::SetNextItemWidth(-1.0f);
                int presetInt = ui_tintLayers[i].preset;
                if (ImGuiMCP::InputInt("##preset", &presetInt, 0)) {
                    if (presetInt < 0) presetInt = 0;
                    ui_tintLayers[i].preset = static_cast<uint16_t>(presetInt);
                }

                ImGuiMCP::PopID();
            }
            ImGuiMCP::EndTable();
            if (to_remove_tint != -1) ui_tintLayers.erase(ui_tintLayers.begin() + to_remove_tint);
        }

        static int s_newTintIdx = 0;
        if (!availableTints.empty()) {
            if (s_newTintIdx >= availableTints.size()) s_newTintIdx = 0;

            ImGuiMCP::Text("Add New Tint Layer:");
            ImGuiMCP::SetNextItemWidth(250.0f);
            ImGuiMCP::Combo("##TintsAdd", &s_newTintIdx, tintNamesCStr.data(), static_cast<int>(tintNamesCStr.size()));
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Add Tint")) {
                uint16_t chosenIndex = availableTints[s_newTintIdx].index;
                bool exists = false;
                for (const auto& tl : ui_tintLayers) {
                    if (tl.index == chosenIndex) { exists = true; break; }
                }

                if (!exists) {
                    UITintLayer newTl;
                    newTl.index = chosenIndex;
                    newTl.name = availableTints[s_newTintIdx].name;
                    newTl.color[0] = 1.0f; newTl.color[1] = 1.0f; newTl.color[2] = 1.0f; newTl.color[3] = 1.0f;
                    newTl.interpolation = 1.0f;
                    newTl.preset = 0;
                    ui_tintLayers.push_back(newTl);
                }
            }
        }
        else {
            ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "Select a valid Race to add Tints.");
        }

        ImGuiMCP::Separator();

        ImGuiMCP::Text("Face Morphs (Bone Structure)");

        bool morphsDisabled = true;
        for (int i = 0; i < 18; i++) {
            if (ui_morphs[i] >= -1.0f && ui_morphs[i] <= 1.0f) {
                morphsDisabled = false;
                break;
            }
        }

        if (morphsDisabled) {
            ImGuiMCP::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "Face Morphs are DISABLED for this NPC (Engine Default).");
            if (ImGuiMCP::Button("Enable Face Morphs")) {
                for (int i = 0; i < 19; i++) ui_morphs[i] = 0.0f;
            }
        }
        else {
            if (ImGuiMCP::Button("Disable Face Morphs (Restore Default)")) {
                for (int i = 0; i < 19; i++) ui_morphs[i] = 3.402823466e+38f;
            }
            ImGuiMCP::Spacing();

            if (ImGuiMCP::BeginTable("MorphsTable", 2, ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_Resizable)) {
                for (int i = 0; i < 18; i++) {
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);
                    ImGuiMCP::Text("%s", morphNames[i]);
                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::PushID(i);

                    float inputWidth = 60.0f;
                    float sliderWidth = ImGuiMCP::GetColumnWidth() - inputWidth - 20.0f;

                    if (sliderWidth > 50.0f) {
                        ImGuiMCP::SetNextItemWidth(sliderWidth);
                        ImGuiMCP::SliderFloat("##morph_slider", &ui_morphs[i], -1.0f, 1.0f, "%.3f");
                        ImGuiMCP::SameLine();
                    }

                    ImGuiMCP::SetNextItemWidth(inputWidth);
                    if (ImGuiMCP::InputFloat("##morph_input", &ui_morphs[i], 0.0f, 0.0f, "%.3f")) {
                        if (ui_morphs[i] < -1.0f) ui_morphs[i] = -1.0f;
                        if (ui_morphs[i] > 1.0f) ui_morphs[i] = 1.0f;
                    }

                    ImGuiMCP::PopID();
                }
                ImGuiMCP::EndTable();
            }
        }
    }
}

void NSettings::Presets() {
    ImGuiMCP::Text("Preset Manager");
    ImGuiMCP::Separator();

    ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "TIP: To create a new Preset, go to the 'NPC Editor' tab, load an NPC,\nmodify it and use the 'Save as Preset' button.");
    ImGuiMCP::Separator();

    RefreshAvailablePresets();

    static std::map<std::string, std::vector<std::string>> presetUsageDB;
    static bool needsUsageScan = true;
    if (ImGuiMCP::Button("Refresh Usage List") || needsUsageScan) {
        presetUsageDB.clear();
        if (std::filesystem::exists(NPCPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(NPCPath)) {
                if (entry.path().extension() == ".json") {
                    FILE* fp = nullptr;
                    fopen_s(&fp, entry.path().string().c_str(), "rb");
                    if (fp) {
                        char readBuffer[2048];
                        rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
                        rapidjson::Document doc;
                        doc.ParseStream(is);
                        fclose(fp);
                        if (doc.IsObject() && doc.HasMember("preset") && doc["preset"].IsString()) {
                            presetUsageDB[doc["preset"].GetString()].push_back(entry.path().stem().string());
                        }
                    }
                }
            }
        }
        needsUsageScan = false;
    }

    auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable;

    static bool openApplyModal = false;
    static std::string presetToApply = "";

    // Variáveis para o Delete Modal
    static bool openDeleteModal = false;
    static std::string presetToDelete = "";
    static bool deleteLinkedNPCs = false;

    // Alterado para 6 Colunas incluindo Deletar.
    if (ImGuiMCP::BeginTable("PresetDB", 6, tableFlags)) {
        ImGuiMCP::TableSetupColumn("Edit", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGuiMCP::TableSetupColumn("Apply", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGuiMCP::TableSetupColumn("Export", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGuiMCP::TableSetupColumn("Delete", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGuiMCP::TableSetupColumn("Preset Name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableSetupColumn("Affected NPCs", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableHeadersRow();

        for (const auto& pName : ui_availablePresets) {
            ImGuiMCP::TableNextRow();

            // Coluna 0 - Botão Editar
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::PushID(pName.c_str());
            if (ImGuiMCP::Button("Edit")) {
                LoadPresetToUI(pName);
            }

            // Coluna 1 - Botão Aplicar
            ImGuiMCP::TableSetColumnIndex(1);
            if (ImGuiMCP::Button("Apply")) {
                presetToApply = pName;
                openApplyModal = true;
            }

            // Coluna 2 - Botão Exportar Preset
            ImGuiMCP::TableSetColumnIndex(2);
            if (isEditingPreset && activePresetName == pName && HasUnsavedChanges()) {
                ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "Save First");
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip("This Preset is currently being edited and has unsaved changes.\nSave the data first.");
                }
            }
            else {
                if (ImGuiMCP::Button("Export")) {
                    ExportPresetAsZip(pName);
                }
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip("The .zip file will be saved in the folder:\nData/Exports");
                }
            }

            // Coluna 3 - Botão Deletar (NOVO)
            ImGuiMCP::TableSetColumnIndex(3);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, { 0.8f, 0.2f, 0.2f, 1.0f });
            if (ImGuiMCP::Button("Delete")) {
                presetToDelete = pName;
                deleteLinkedNPCs = false; // Reseta o estado do checkbox
                openDeleteModal = true;
            }
            ImGuiMCP::PopStyleColor();
            ImGuiMCP::PopID();

            // Coluna 4 - Nome
            ImGuiMCP::TableSetColumnIndex(4);
            ImGuiMCP::Text("%s", pName.c_str());

            // Coluna 5 - Uso
            ImGuiMCP::TableSetColumnIndex(5);
            std::string users = "";
            for (const auto& u : presetUsageDB[pName]) users += u + ", ";
            if (!users.empty()) { users.pop_back(); users.pop_back(); }
            ImGuiMCP::TextWrapped("%s", users.empty() ? "None" : users.c_str());
        }
        ImGuiMCP::EndTable();
    }

    // =====================================
    // MODAL DE APLICAR PRESET
    // =====================================
    if (openApplyModal) ImGuiMCP::OpenPopup("ModalAplicarPreset");

    if (ImGuiMCP::BeginPopupModal("ModalAplicarPreset", &openApplyModal, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiMCP::Text("Select NPCs to link to preset: %s", presetToApply.c_str());
        ImGuiMCP::Separator();

        static char modalFilter[256] = "";
        ImGuiMCP::SetNextItemWidth(300.0f);
        ImGuiMCP::InputText("Search NPC", modalFilter, sizeof(modalFilter));

        auto manager = Manager::GetSingleton();
        const auto& npcList = manager->GetList("NPC");

        static std::set<RE::FormID> selectedNPCs;

        std::string search(modalFilter);
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);
        std::vector<size_t> filteredIdx;
        for (size_t i = 0; i < npcList.size(); i++) {
            if (!search.empty()) {
                std::string n = npcList[i].name; std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                std::string e = npcList[i].editorID; std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                if (n.find(search) == std::string::npos && e.find(search) == std::string::npos) continue;
            }
            filteredIdx.push_back(i);
        }

        ImGuiMCP::Text("NPCs found: %d", (int)filteredIdx.size());

        ImGuiMCP::BeginChild("NPCListModalChild", ImGuiMCP::ImVec2(500, 300), true);
        static auto clipper = ImGuiMCP::ImGuiListClipperManager::Create();
        ImGuiMCP::ImGuiListClipperManager::Begin(clipper, (int)filteredIdx.size(), -1.0f);
        while (ImGuiMCP::ImGuiListClipperManager::Step(clipper)) {
            for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
                size_t idx = filteredIdx[i];
                const auto& item = npcList[idx];
                bool isSelected = selectedNPCs.contains(item.formID);

                ImGuiMCP::PushID(static_cast<int>(item.formID));
                std::string label = std::format("{} [{:08X}]", item.name.empty() ? item.editorID : item.name, item.formID);
                if (ImGuiMCP::Checkbox(label.c_str(), &isSelected)) {
                    if (isSelected) selectedNPCs.insert(item.formID);
                    else selectedNPCs.erase(item.formID);
                }
                ImGuiMCP::PopID();
            }
        }
        ImGuiMCP::ImGuiListClipperManager::End(clipper);
        ImGuiMCP::EndChild();

        ImGuiMCP::Separator();

        if (ImGuiMCP::Button("Check All Visible")) {
            for (size_t idx : filteredIdx) selectedNPCs.insert(npcList[idx].formID);
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Uncheck All")) {
            selectedNPCs.clear();
        }

        ImGuiMCP::Spacing();

        std::string btnApplyText = std::format("Apply and Save ({})", selectedNPCs.size());
        if (ImGuiMCP::Button(btnApplyText.c_str(), ImGuiMCP::ImVec2(180, 0))) {

            // NOVO (1): Carrega o Preset em memória UMA VEZ antes de aplicar aos NPCs
            rapidjson::Document presetDoc;
            std::string pPath = std::format("{}/{}.json", PresetsPath, presetToApply);
            FILE* pFp = nullptr;
            fopen_s(&pFp, pPath.c_str(), "rb");
            if (pFp) {
                char readBuffer[65536];
                rapidjson::FileReadStream is(pFp, readBuffer, sizeof(readBuffer));
                presetDoc.ParseStream(is);
                fclose(pFp);
            }

            for (auto formID : selectedNPCs) {
                if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(formID)) {

                    // NOVO (1): Aplica o Preset no TESNPC na memória
                    if (presetDoc.IsObject()) {
                        Manager::ApplyNPCCustomizationFromJSON(npc, presetDoc);
                    }

                    // Salva a alteração atrelando o preset no disco
                    rapidjson::Document doc;
                    auto& allocator = doc.GetAllocator();
                    doc.SetObject();
                    doc.AddMember("preset", rapidjson::Value(presetToApply.c_str(), allocator), allocator);

                    std::string editorID = clib_util::editorID::get_editorID(npc);
                    if (editorID.empty()) editorID = std::format("{:08X}", npc->GetFormID());

                    std::filesystem::create_directories(NPCPath);
                    std::string finalPath = std::format("{}/{}.json", NPCPath, editorID);

                    FILE* fp = nullptr;
                    fopen_s(&fp, finalPath.c_str(), "wb");
                    if (fp) {
                        char writeBuffer[65536];
                        rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
                        rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
                        doc.Accept(writer);
                        fclose(fp);
                    }
                }
            }
            needsUsageScan = true;
            selectedNPCs.clear();
            modalFilter[0] = '\0';
            openApplyModal = false;
            ImGuiMCP::CloseCurrentPopup();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Cancel", ImGuiMCP::ImVec2(120, 0))) {
            selectedNPCs.clear();
            modalFilter[0] = '\0';
            openApplyModal = false;
            ImGuiMCP::CloseCurrentPopup();
        }
        ImGuiMCP::EndPopup();
    }

    // =====================================
    // MODAL DE DELETAR PRESET (NOVO - 2)
    // =====================================
    if (openDeleteModal) ImGuiMCP::OpenPopup("Confirm Delete Preset");

    if (ImGuiMCP::BeginPopupModal("Confirm Delete Preset", &openDeleteModal, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiMCP::Text("Are you sure you want to delete the preset '%s'?", presetToDelete.c_str());
        ImGuiMCP::Separator();

        auto& users = presetUsageDB[presetToDelete];
        if (!users.empty()) {
            ImGuiMCP::TextColored({ 1.0f, 0.6f, 0.2f, 1.0f }, "Warning: This preset is being used by %d NPC(s).", (int)users.size());

            ImGuiMCP::BeginChild("DeleteAffectedList", ImGuiMCP::ImVec2(400, 150), true);
            for (const auto& u : users) {
                ImGuiMCP::Text("- %s", u.c_str());
            }
            ImGuiMCP::EndChild();

            ImGuiMCP::Checkbox("Also delete the JSON edits for these affected NPCs?", &deleteLinkedNPCs);
            ImGuiMCP::Spacing();
        }

        if (ImGuiMCP::Button("Yes, Delete", ImGuiMCP::ImVec2(120, 0))) {
            // Deleta o arquivo do Preset do disco
            std::string presetPath = std::format("{}/{}.json", PresetsPath, presetToDelete);
            if (std::filesystem::exists(presetPath)) {
                std::filesystem::remove(presetPath);
            }

            // Deleta os NPCs vinculados se o utilizador confirmou no checkbox
            if (deleteLinkedNPCs && !users.empty()) {
                for (const auto& u : users) {
                    std::string nPath = std::format("{}/{}.json", NPCPath, u);
                    if (std::filesystem::exists(nPath)) {
                        std::filesystem::remove(nPath);
                    }
                }
            }

            // Se o preset deletado era o que estava aberto na interface, limpa
            if (activePresetName == presetToDelete) {
                activePresetName = "";
                isEditingPreset = false;
            }
            if (ui_linkedPreset == presetToDelete) {
                ui_linkedPreset = "";
            }

            needsUsageScan = true;
            openDeleteModal = false;
            ImGuiMCP::CloseCurrentPopup();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Cancel", ImGuiMCP::ImVec2(120, 0))) {
            openDeleteModal = false;
            ImGuiMCP::CloseCurrentPopup();
        }
        ImGuiMCP::EndPopup();
    }
}

void NSettings::NPCList() {
    auto manager = Manager::GetSingleton();
    const auto& npcList = manager->GetList("NPC");

    if (npcList.empty()) {
        ImGuiMCP::Text("No NPCs loaded into memory. Force a scan.");
        if (ImGuiMCP::Button("Force Scan")) manager->PopulateAllLists();
        return;
    }

    static std::map<std::string, std::string> affectedDB;
    static bool needScan = true;

    if (needScan) {
        affectedDB.clear();
        if (std::filesystem::exists(NPCPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(NPCPath)) {
                if (entry.path().extension() == ".json") {
                    std::string stem = entry.path().stem().string();
                    std::string presetLinked = "Custom";

                    FILE* fp = nullptr;
                    fopen_s(&fp, entry.path().string().c_str(), "rb");
                    if (fp) {
                        char readBuffer[2048];
                        rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
                        rapidjson::Document doc;
                        doc.ParseStream(is);
                        fclose(fp);
                        if (doc.IsObject() && doc.HasMember("preset") && doc["preset"].IsString()) {
                            presetLinked = doc["preset"].GetString();
                        }
                    }
                    affectedDB[stem] = presetLinked;
                }
            }
        }
        needScan = false;
    }

    if (ImGuiMCP::Button("Refresh Database")) needScan = true;

    static char filterBuffer[256] = "";
    static bool showOnlyAffected = false;
    static std::vector<size_t> cachedFilteredIndices;

    ImGuiMCP::SetNextItemWidth(200.0f);
    bool searchChanged = ImGuiMCP::InputText("Filter Name/EditorID", filterBuffer, sizeof(filterBuffer));
    ImGuiMCP::SameLine();
    bool toggleChanged = ImGuiMCP::Checkbox("Only Modified NPCs", &showOnlyAffected);

    if (searchChanged || toggleChanged || cachedFilteredIndices.empty()) {
        cachedFilteredIndices.clear();
        std::string search(filterBuffer);
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);

        for (size_t i = 0; i < npcList.size(); i++) {
            const auto& item = npcList[i];
            std::string edidHex = std::format("{:08X}", item.formID);
            bool isAffected = affectedDB.contains(item.editorID) || affectedDB.contains(edidHex);

            if (showOnlyAffected && !isAffected) continue;

            if (!search.empty()) {
                std::string n = item.name; std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                std::string e = item.editorID; std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                if (n.find(search) == std::string::npos && e.find(search) == std::string::npos) continue;
            }
            cachedFilteredIndices.push_back(i);
        }
    }

    ImGuiMCP::Text("Showing %d NPCs", (int)cachedFilteredIndices.size());
    ImGuiMCP::Separator();

    auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
        ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

    if (ImGuiMCP::BeginTable("NPCDatabaseTable", 4, tableFlags)) {
        ImGuiMCP::TableSetupColumn("Action", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGuiMCP::TableSetupColumn("FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGuiMCP::TableSetupColumn("Name / EditorID", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableSetupColumn("Status", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGuiMCP::TableHeadersRow();

        static auto clipper = ImGuiMCP::ImGuiListClipperManager::Create();
        ImGuiMCP::ImGuiListClipperManager::Begin(clipper, (int)cachedFilteredIndices.size(), -1.0f);

        while (ImGuiMCP::ImGuiListClipperManager::Step(clipper)) {
            for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
                size_t idx = cachedFilteredIndices[i];
                const auto& item = npcList[idx];

                std::string edidHex = std::format("{:08X}", item.formID);

                auto dbIt = affectedDB.find(item.editorID);
                if (dbIt == affectedDB.end()) dbIt = affectedDB.find(edidHex);

                ImGuiMCP::TableNextRow();

                ImGuiMCP::TableSetColumnIndex(0);
                ImGuiMCP::PushID(static_cast<int>(item.formID));
                if (ImGuiMCP::Button("Edit")) {
                    if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(item.formID)) {
                        LoadNPCToUI(npc, nullptr);
                        // A UI não troca de aba automaticamente pois SKSEMenuFramework não suporta isso em runtime.
                    }
                }
                ImGuiMCP::PopID();

                ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::Text("%08X", item.formID);
                ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::TextUnformatted(item.name.empty() ? item.editorID.c_str() : item.name.c_str());

                ImGuiMCP::TableSetColumnIndex(3);
                if (dbIt != affectedDB.end()) {
                    if (dbIt->second == "Custom") {
                        ImGuiMCP::TextColored({ 0.2f, 1.0f, 0.2f, 1.0f }, "MODIFIED");
                    }
                    else {
                        ImGuiMCP::TextColored({ 0.2f, 1.0f, 1.0f, 1.0f }, "Preset: %s", dbIt->second.c_str());
                    }
                }
                else {
                    ImGuiMCP::TextDisabled("Default");
                }
            }
        }
        ImGuiMCP::ImGuiListClipperManager::End(clipper);
        ImGuiMCP::EndTable();
    }
}

void NSettings::NPCMenu() {


    if (!isEditingPreset) {
        if (ImGuiMCP::Button("Load Selected NPC (Console)")) {
            LoadNPCToUI();
        }
    }

    if (isEditingPreset) {
        ImGuiMCP::TextColored({ 0.2f, 1.0f, 0.2f, 1.0f }, "PRESET EDIT MODE: %s", activePresetName.c_str());
        if (ImGuiMCP::Button("<- Exit Preset Editor")) {
            isEditingPreset = false;
            activePresetName = "";
            g_currentNPC = nullptr; // Limpa para forçar carregar outro
        }
        ImGuiMCP::Separator();
        DrawMainEditorUI();
    }
    else {
        if (!g_currentNPC) {
            ImGuiMCP::Text("Open the game console, select an NPC and press 'Load'.");
        }
        else {
            std::string label = std::format("Editing NPC: {} [{:08X}]", g_currentNPC->GetFullName() ? g_currentNPC->GetFullName() : "Unnamed", g_currentNPC->GetFormID());
            ImGuiMCP::Text(label.c_str());
            ImGuiMCP::Separator();
            DrawMainEditorUI();
        }
    }
}



void NSettings::MmRegister() {
    if (SKSEMenuFramework::IsInstalled()) {
        SKSEMenuFramework::SetSection("NPC Editor");
        SKSEMenuFramework::AddSectionItem("NPC Editor", NPCMenu);
        SKSEMenuFramework::AddSectionItem("Presets", Presets);
        SKSEMenuFramework::AddSectionItem("NPC Database", NPCList);
    }
}

// Carregamento de Inicialização (Auto-Load no jogo)
void NSettings::Load() {
    std::filesystem::create_directories(NPCPath);
    std::filesystem::create_directories(PresetsPath);

    int countPresetsCarregados = 0;
    int countNPCsModificados = 0;

    std::map<std::string, rapidjson::Document> presetCache;
    for (const auto& entry : std::filesystem::directory_iterator(PresetsPath)) {
        if (entry.path().extension() == ".json") {
            FILE* fp = nullptr;
            fopen_s(&fp, entry.path().string().c_str(), "rb");
            if (fp) {
                char readBuffer[65536];
                rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
                rapidjson::Document doc;
                doc.ParseStream(is);
                fclose(fp);
                if (doc.IsObject()) {
                    presetCache[entry.path().stem().string()] = std::move(doc);
                    countPresetsCarregados++; // PONTO 7: Contador de Presets
                }
            }
        }
    }

    for (const auto& entry : std::filesystem::directory_iterator(NPCPath)) {
        if (entry.path().extension() == ".json") {
            std::string filename = entry.path().stem().string();

            RE::TESNPC* targetNPC = nullptr;

            if (auto edidForm = RE::TESForm::LookupByEditorID(filename)) {
                targetNPC = edidForm->As<RE::TESNPC>();
            }
            else {
                try {
                    RE::FormID id = std::stoul(filename, nullptr, 16);
                    if (auto idForm = RE::TESForm::LookupByID(id)) targetNPC = idForm->As<RE::TESNPC>();
                }
                catch (...) {}
            }

            if (targetNPC) {
                FILE* fp = nullptr;
                fopen_s(&fp, entry.path().string().c_str(), "rb");
                if (fp) {
                    char readBuffer[65536];
                    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
                    rapidjson::Document doc;
                    doc.ParseStream(is);
                    fclose(fp);

                    if (doc.IsObject()) {
                        if (doc.HasMember("preset") && doc["preset"].IsString()) {
                            std::string presetName = doc["preset"].GetString();
                            if (presetCache.find(presetName) != presetCache.end()) {
                                Manager::ApplyNPCCustomizationFromJSON(targetNPC, presetCache[presetName]);
                                countNPCsModificados++; // PONTO 7: Contador de NPCs
                            }
                            else {
                                logger::warn("NPC {} points to preset {} which does not exist.", filename, presetName);
                            }
                        }
                        else {
                            Manager::ApplyNPCCustomizationFromJSON(targetNPC, doc);
                            countNPCsModificados++; // PONTO 7: Contador de NPCs Custom
                        }
                    }
                }
            }
        }
    }

    // PONTO 7: LOG FINAL DE STATUS NO BOOT
    logger::info("[NPC Replacer] Loading complete: {} presets cached, {} modified NPCs applied.", countPresetsCarregados, countNPCsModificados);
}
