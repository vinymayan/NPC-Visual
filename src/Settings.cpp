#include "Settings.h"
#include "Manager.h"

const char* BasePath = "Data/Viny Mods/NPC Visual";
const char* NPCPath = "Data/Viny Mods/NPC Visual/NPC";
const char* PresetsPath = "Data/Viny Mods/NPC Visual/Presets";
const char* ExportPath = "Data/Viny Mods/NPC Visual/Export";
const char* LanguagePath = "Data/Viny Mods/NPC Visual/Language.json";

const char* LegacyBasePath = "Data/SKSE/Plugins/NPC Replacer";
const char* LegacyNPCPath = "Data/SKSE/Plugins/NPC Replacer/NPC";
const char* LegacyPresetsPath = "Data/SKSE/Plugins/NPC Replacer/Presets";

// Variaveis de estado para a UI
static RE::Actor* g_currentActor = nullptr;
static RE::TESNPC* g_currentNPC = nullptr;

static std::map<std::string, std::string> g_loc;

static void WriteDefaultLanguageFile()
{
    rapidjson::Document doc;
    doc.SetObject();

    if (std::filesystem::exists(LanguagePath)) {
        FILE* readFp = nullptr;
        fopen_s(&readFp, LanguagePath, "rb");
        if (readFp) {
            char readBuffer[65536];
            rapidjson::FileReadStream is(readFp, readBuffer, sizeof(readBuffer));
            doc.ParseStream(is);
            fclose(readFp);

            if (doc.HasParseError() || !doc.IsObject()) {
                logger::warn("[Language] Existing Language.json is invalid; rebuilding defaults.");
                doc.SetObject();
            }
        }
    }

    std::filesystem::create_directories(BasePath);
    auto& allocator = doc.GetAllocator();

    auto ensureString = [&](const char* key, const char* value) {
        if (doc.HasMember(key)) {
            return;
        }

        rapidjson::Value name;
        name.SetString(key, allocator);
        rapidjson::Value text;
        text.SetString(value, allocator);
        doc.AddMember(name, text, allocator);
    };

    ensureString("menu.editor", "Editor");
    ensureString("menu.presets", "Presets");
    ensureString("menu.database", "Database");
    ensureString("menu.export", "Export");
    ensureString("menu.debug", "Debug");
    ensureString("export.title", "Export Package");
    ensureString("export.description", "Select presets and NPC edits to export into one ZIP package.");
    ensureString("export.refresh", "Refresh Lists");
    ensureString("export.export_selected", "Export Selected");
    ensureString("export.output_hint", "ZIP files are saved to Data/Viny Mods/NPC Visual/Export.");
    ensureString("debug.title", "Debug Tools");
    ensureString("debug.reload_data", "Reload Data");
    ensureString("debug.reload_data_hint", "Refreshes the internal form database. Use this after dynamic form mods have injected or updated forms.");

    FILE* fp = nullptr;
    fopen_s(&fp, LanguagePath, "wb");
    if (!fp) {
        return;
    }
    char writeBuffer[65536];
    rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
    rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
    doc.Accept(writer);
    fclose(fp);
}

static void LoadLanguage()
{
    g_loc.clear();
    WriteDefaultLanguageFile();

    FILE* fp = nullptr;
    fopen_s(&fp, LanguagePath, "rb");
    if (!fp) {
        return;
    }

    char readBuffer[65536];
    rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
    rapidjson::Document doc;
    doc.ParseStream(is);
    fclose(fp);

    if (!doc.IsObject()) {
        return;
    }

    for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
        if (it->name.IsString() && it->value.IsString()) {
            g_loc[it->name.GetString()] = it->value.GetString();
        }
    }
}

static const char* GetLoc(const char* key, const char* fallback)
{
    auto it = g_loc.find(key);
    return it != g_loc.end() ? it->second.c_str() : fallback;
}

static void CopyDirectoryFilesIfMissing(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (!std::filesystem::exists(from)) {
        return;
    }

    std::filesystem::create_directories(to);
    for (const auto& entry : std::filesystem::directory_iterator(from)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext != ".json" && ext != ".png") {
            continue;
        }

        const auto target = to / entry.path().filename();
        if (!std::filesystem::exists(target)) {
            std::error_code ec;
            std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::none, ec);
            if (ec) {
                logger::warn("[Migration] Failed to copy '{}' to '{}': {}", entry.path().string(), target.string(), ec.message());
            }
        }
    }
}

static void EnsureStorageDirectories()
{
    std::filesystem::create_directories(BasePath);
    std::filesystem::create_directories(NPCPath);
    std::filesystem::create_directories(PresetsPath);
    std::filesystem::create_directories(ExportPath);

    // IMPORTANTE:
    // Nao copie arquivos do legacy aqui. Esta funcao roda no boot, no refresh da UI
    // e em varios fluxos de leitura. Se ela copiar aqui, a migracao acontece antes
    // do usuario salvar qualquer dado. A migracao legacy -> novo caminho deve
    // acontecer apenas depois de SaveData()/gravacao bem-sucedida.
    WriteDefaultLanguageFile();
}

static std::vector<std::filesystem::path> CollectJsonFiles(const std::filesystem::path& primary, const std::filesystem::path& legacy)
{
    std::vector<std::filesystem::path> result;
    std::set<std::string> seen;
    auto collect = [&](const std::filesystem::path& dir) {
        if (!std::filesystem::exists(dir)) {
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") {
                continue;
            }
            const auto stem = entry.path().stem().string();
            if (seen.insert(stem).second) {
                result.push_back(entry.path());
            }
        }
        };

    collect(primary);
    collect(legacy);
    return result;
}

static std::filesystem::path ResolveJsonPath(const std::string& folder, const std::string& legacyFolder, const std::string& name)
{
    const auto primary = std::filesystem::path(folder) / (name + ".json");
    if (std::filesystem::exists(primary)) {
        return primary;
    }

    const auto legacy = std::filesystem::path(legacyFolder) / (name + ".json");
    if (std::filesystem::exists(legacy)) {
        return legacy;
    }

    return primary;
}

static void RemoveJsonInBothLocations(const std::string& folder, const std::string& legacyFolder, const std::string& name)
{
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(folder) / (name + ".json"), ec);
    ec.clear();
    std::filesystem::remove(std::filesystem::path(legacyFolder) / (name + ".json"), ec);
}

static std::string NormalizeFsPathForCompare(std::filesystem::path path)
{
    auto value = path.lexically_normal().string();
    std::replace(value.begin(), value.end(), '/', '\\');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static bool PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& root)
{
    auto value = NormalizeFsPathForCompare(path);
    auto prefix = NormalizeFsPathForCompare(root);
    return value.rfind(prefix, 0) == 0;
}

static const char* JsonKind(const rapidjson::Value& value)
{
    if (value.IsObject()) return "object";
    if (value.IsArray()) return "array";
    if (value.IsString()) return "string";
    if (value.IsBool()) return "bool";
    if (value.IsNumber()) return "number";
    if (value.IsNull()) return "null";
    return "unknown";
}

static void LogJsonCompatibilityShape(const std::filesystem::path& path, const rapidjson::Document& doc, const char* context)
{
    std::uint32_t oldStringRefs = 0;
    std::uint32_t newObjectRefs = 0;
    std::uint32_t numericRefs = 0;
    std::uint32_t emptyStringRefs = 0;
    std::uint32_t invalidRefs = 0;

    auto classifyRef = [&](const rapidjson::Value& value) {
        if (value.IsObject()) {
            ++newObjectRefs;
        }
        else if (value.IsString()) {
            if (value.GetStringLength() == 0) {
                ++emptyStringRefs;
            }
            else {
                ++oldStringRefs;
            }
        }
        else if (value.IsUint()) {
            ++numericRefs;
        }
        else {
            ++invalidRefs;
        }
    };

    const char* formKeys[] = { "race", "skin", "defaultOutfit", "sleepOutfit", "voice", "hairColor" };
    for (const auto* key : formKeys) {
        if (doc.HasMember(key)) {
            classifyRef(doc[key]);
            logger::debug("[JSONCompat] context={} key='{}' kind={} path='{}'",
                context,
                key,
                JsonKind(doc[key]),
                path.string());
        }
    }

    std::uint32_t headPartCount = 0;
    if (doc.HasMember("headParts") && doc["headParts"].IsArray()) {
        for (const auto& hp : doc["headParts"].GetArray()) {
            ++headPartCount;
            classifyRef(hp);
        }
    }
    else if (doc.HasMember("headParts")) {
        ++invalidRefs;
        logger::debug("[JSONCompat] context={} key='headParts' kind={} expected=array path='{}'",
            context,
            JsonKind(doc["headParts"]),
            path.string());
    }

    logger::debug("[JSONCompat] context={} path='{}' legacyLocation={} presetLink={} customFaceNif={} headParts={} oldStringRefs={} newObjectRefs={} numericRefs={} emptyStringRefs={} invalidRefs={}",
        context,
        path.string(),
        PathStartsWith(path, LegacyBasePath),
        doc.HasMember("preset") && doc["preset"].IsString(),
        doc.HasMember("customFaceNif") && doc["customFaceNif"].IsString(),
        headPartCount,
        oldStringRefs,
        newObjectRefs,
        numericRefs,
        emptyStringRefs,
        invalidRefs);
}

static bool ReadJsonObjectFromFile(const std::filesystem::path& path, rapidjson::Document& outDoc, const char* context)
{
    outDoc.SetObject();

    FILE* fp = nullptr;
    const auto pathStr = path.string();
    std::error_code fileEc;
    const auto fileSize = std::filesystem::exists(path, fileEc) && !fileEc ? std::filesystem::file_size(path, fileEc) : 0;
    logger::debug("[{}] ReadJsonObject BEGIN path='{}' exists={} size={} legacyLocation={}",
        context,
        pathStr,
        std::filesystem::exists(path),
        fileEc ? 0 : fileSize,
        PathStartsWith(path, LegacyBasePath));

    fopen_s(&fp, pathStr.c_str(), "rb");
    if (!fp) {
        logger::warn("[{}] Falha ao abrir JSON: {}", context, pathStr);
        return false;
    }

    try {
        char readBuffer[65536];
        rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
        outDoc.ParseStream(is);
        fclose(fp);
        fp = nullptr;
    }
    catch (...) {
        if (fp) {
            fclose(fp);
        }
        logger::error("[{}] Excecao ao ler JSON: {}", context, pathStr);
        return false;
    }

    if (outDoc.HasParseError()) {
        logger::error("[{}] JSON invalido: {} offset={} code={}",
            context,
            pathStr,
            outDoc.GetErrorOffset(),
            static_cast<int>(outDoc.GetParseError()));
        return false;
    }

    if (!outDoc.IsObject()) {
        logger::error("[{}] JSON nao e objeto: {}", context, pathStr);
        return false;
    }

    LogJsonCompatibilityShape(path, outDoc, context);
    logger::debug("[{}] ReadJsonObject END path='{}' members={}", context, pathStr, outDoc.MemberCount());

    return true;
}

static void OverlayJsonObject(rapidjson::Document& dst, const rapidjson::Document& src)
{
    if (!dst.IsObject()) {
        dst.SetObject();
    }
    if (!src.IsObject()) {
        return;
    }

    auto& allocator = dst.GetAllocator();
    for (auto it = src.MemberBegin(); it != src.MemberEnd(); ++it) {
        if (!it->name.IsString()) {
            continue;
        }

        const auto key = it->name.GetString();
        auto dstIt = dst.FindMember(key);
        if (dstIt != dst.MemberEnd()) {
            dstIt->value.CopyFrom(it->value, allocator);
        }
        else {
            rapidjson::Value name;
            name.SetString(it->name.GetString(), it->name.GetStringLength(), allocator);
            rapidjson::Value value;
            value.CopyFrom(it->value, allocator);
            dst.AddMember(name, value, allocator);
        }
    }
}

static void RemoveLegacyFileIfExists(const std::filesystem::path& path)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        ec.clear();
        std::filesystem::remove(path, ec);
        if (ec) {
            logger::warn("[Migration] Failed to remove legacy file '{}': {}", path.string(), ec.message());
        }
    }
}

static void CopyLegacySidecarIfMissing(const std::filesystem::path& legacyPath, const std::filesystem::path& newPath)
{
    std::error_code ec;
    if (!std::filesystem::exists(legacyPath, ec) || ec) {
        return;
    }
    ec.clear();
    if (std::filesystem::exists(newPath, ec) && !ec) {
        return;
    }

    ec.clear();
    std::filesystem::create_directories(newPath.parent_path(), ec);
    ec.clear();
    std::filesystem::copy_file(legacyPath, newPath, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        logger::warn("[Migration] Failed to copy legacy sidecar '{}' to '{}': {}",
            legacyPath.string(),
            newPath.string(),
            ec.message());
    }
}

static void FinalizeLegacyMigrationAfterSave(const std::string& folder, const std::string& legacyFolder, const std::string& name)
{
    const auto legacyJson = std::filesystem::path(legacyFolder) / (name + ".json");
    const auto legacyPng = std::filesystem::path(legacyFolder) / (name + ".png");
    const auto newPng = std::filesystem::path(folder) / (name + ".png");

    // O JSON novo ja foi escrito pelo SaveData. Copiamos apenas sidecars, como thumbs .png.
    CopyLegacySidecarIfMissing(legacyPng, newPng);

    // So depois da gravacao/copia bem-sucedida tentamos limpar o legacy.
    RemoveLegacyFileIfExists(legacyJson);

    std::error_code ec;
    if (!std::filesystem::exists(legacyPng, ec) || ec) {
        return;
    }
    ec.clear();
    if (std::filesystem::exists(newPng, ec) && !ec) {
        RemoveLegacyFileIfExists(legacyPng);
    }
}

static bool isEditingPreset = false;
static std::string activePresetName = "";

// Atributos base
static float ui_height = 1.0f;
static float ui_weight = 50.0f;
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
static RE::BGSVoiceType* ui_voice = nullptr;
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
static std::map<RE::FormID, std::string> g_vanillaNPCStates;

void CaptureVanillaState(RE::TESNPC* npc, std::string& outJson) {
    rapidjson::Document doc;
    auto& allocator = doc.GetAllocator();
    doc.SetObject();

    doc.AddMember("height", npc->height, allocator);
    doc.AddMember("weight", npc->weight, allocator);
    doc.AddMember("isFemale", npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kFemale), allocator);
    doc.AddMember("oppositeGenderAnim", npc->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kOppositeGenderAnims), allocator);

    rapidjson::Value bodyTint(rapidjson::kObjectType);
    bodyTint.AddMember("r", npc->bodyTintColor.red, allocator);
    bodyTint.AddMember("g", npc->bodyTintColor.green, allocator);
    bodyTint.AddMember("b", npc->bodyTintColor.blue, allocator);
    bodyTint.AddMember("a", npc->bodyTintColor.alpha, allocator);
    doc.AddMember("bodyTintColor", bodyTint, allocator);

    if (npc->race) doc.AddMember("race", FormUtil::MakeFormRef(npc->race, allocator), allocator);
    if (npc->farSkin) doc.AddMember("skin", FormUtil::MakeFormRef(npc->farSkin, allocator), allocator);
    if (npc->defaultOutfit) doc.AddMember("defaultOutfit", FormUtil::MakeFormRef(npc->defaultOutfit, allocator), allocator);
    if (npc->sleepOutfit) doc.AddMember("sleepOutfit", FormUtil::MakeFormRef(npc->sleepOutfit, allocator), allocator);
    if (npc->voiceType) doc.AddMember("voice", FormUtil::MakeFormRef(npc->voiceType, allocator), allocator);
    if (npc->headRelatedData && npc->headRelatedData->hairColor) doc.AddMember("hairColor", FormUtil::MakeFormRef(npc->headRelatedData->hairColor, allocator), allocator);

    rapidjson::Value hpArray(rapidjson::kArrayType);
    std::set<RE::BGSHeadPart*> ownedExtraParts;
    if (npc->headParts) {
        for (int i = 0; i < npc->numHeadParts; i++) {
            auto hp = npc->headParts[i];
            if (hp) {
                for (auto* extra : hp->extraParts) {
                    if (extra) ownedExtraParts.insert(extra);
                }
            }
        }
    }
    for (int i = 0; i < npc->numHeadParts; i++) {
        if (npc->headParts && npc->headParts[i]) {
            auto hp = npc->headParts[i];
            if (ownedExtraParts.find(hp) == ownedExtraParts.end()) {
                hpArray.PushBack(FormUtil::MakeFormRef(hp, allocator), allocator);
            }
        }
    }
    doc.AddMember("headParts", hpArray, allocator);

    rapidjson::Value tintArray(rapidjson::kArrayType);
    if (npc->tintLayers) {
        for (std::uint32_t i = 0; i < npc->tintLayers->size(); ++i) {
            auto layer = (*npc->tintLayers)[i];
            if (layer) {
                rapidjson::Value tlVal(rapidjson::kObjectType);
                tlVal.AddMember("index", layer->tintIndex, allocator);
                rapidjson::Value cVal(rapidjson::kObjectType);
                cVal.AddMember("r", layer->tintColor.red, allocator);
                cVal.AddMember("g", layer->tintColor.green, allocator);
                cVal.AddMember("b", layer->tintColor.blue, allocator);
                cVal.AddMember("a", layer->tintColor.alpha, allocator);
                tlVal.AddMember("color", cVal, allocator);
                tlVal.AddMember("interpolation", layer->interpolationValue / 100.0f, allocator);
                tlVal.AddMember("preset", layer->preset, allocator);
                tintArray.PushBack(tlVal, allocator);
            }
        }
    }
    doc.AddMember("tintLayers", tintArray, allocator);

    rapidjson::Value morphsArray(rapidjson::kArrayType);
    if (npc->faceData) {
        for (int i = 0; i < 19; i++) morphsArray.PushBack(npc->faceData->morphs[i], allocator);
    }
    doc.AddMember("faceMorphs", morphsArray, allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    outJson = buffer.GetString();
}

static rapidjson::Document BuildSafeCustomizationDocument(RE::TESNPC* npc, const rapidjson::Document& loadedDoc)
{
    rapidjson::Document safeDoc;
    safeDoc.SetObject();

    // Arquivos legacy podem nao possuir campos que o aplicador atual espera.
    // Baseamos o documento no estado vanilla do NPC e sobrescrevemos com o JSON carregado.
    if (npc) {
        std::string baseState;
        const auto id = npc->GetFormID();
        auto it = g_vanillaNPCStates.find(id);
        if (it != g_vanillaNPCStates.end()) {
            baseState = it->second;
        }
        else {
            logger::debug("[JSONCompat] Capturando vanilla state para NPC {:08X} antes de aplicar JSON.", id);
            CaptureVanillaState(npc, baseState);
            g_vanillaNPCStates[id] = baseState;
            logger::debug("[JSONCompat] Vanilla state capturado para NPC {:08X}.", id);
        }

        safeDoc.Parse(baseState.c_str());
        if (safeDoc.HasParseError() || !safeDoc.IsObject()) {
            safeDoc.SetObject();
        }
    }

    OverlayJsonObject(safeDoc, loadedDoc);
    return safeDoc;
}

// ==========================================
// CONTROLADOR DE ALTERA��ES N�O GUARDADAS
// ==========================================
static std::string g_lastSavedStateStr = "";
static std::string g_lastSavedPresetLink = "";

static std::string ui_customFaceNif = "";

struct CustomFaceInfo {
    std::string nifPath;
    std::string displayPath;
    std::string displayName;
    RE::FormID originFormID;
    void* textureID;
};
static std::vector<CustomFaceInfo> scannedFaces;
static bool needFaceScan = true;
static bool openFaceSelectModal = false;
static bool refreshListsOnNextMenuOpen = true;
static int lastNPCVisualRenderFrame = -1;

void ScanFaceGeom();

void EnsureMenuListsPopulated()
{
    auto* manager = Manager::GetSingleton();
    if (!manager->_isPopulated) {
        logger::debug("[MenuPopulate] PopulateAllLists BEGIN reason=menu_open");
        manager->PopulateAllLists();
        logger::debug("[MenuPopulate] PopulateAllLists END reason=menu_open");
    }
    else {
        logger::debug("[MenuPopulate] PopulateAllLists SKIP alreadyPopulated=true reason=menu_open");
    }
}

static std::string NormalizeGamePath(std::string path)
{
    std::replace(path.begin(), path.end(), '/', '\\');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });
    return path;
}

static std::string ResolveFaceDisplayName(const std::string& nifPath)
{
    const auto key = NormalizeGamePath(nifPath);
    for (const auto& face : scannedFaces) {
        if (NormalizeGamePath(face.nifPath) == key) {
            return face.displayName;
        }
    }

    std::filesystem::path path(nifPath);
    const auto stem = path.stem().string();
    const auto pluginFolder = path.parent_path().filename().string();
    bool isStrictHex = !stem.empty() && stem.length() <= 8;
    for (const char c : stem) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            isStrictHex = false;
            break;
        }
    }

    if (isStrictHex) {
        RE::FormID formID = 0;
        try {
            const auto rawID = static_cast<std::uint32_t>(std::stoul(stem, nullptr, 16));
            if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
                if (const auto* modFile = dataHandler->LookupModByName(pluginFolder)) {
                    const auto localID = modFile->IsLight() ? (rawID & 0x00000FFF) : (rawID & 0x00FFFFFF);
                    formID = dataHandler->LookupFormID(localID, pluginFolder);
                }
            }
            if (formID == 0) {
                formID = rawID;
            }
        }
        catch (...) {
            formID = 0;
        }

        if (auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(formID)) {
            return std::format("{} [{:08X}]", npc->GetFullName() ? npc->GetFullName() : "Unnamed", formID);
        }
        if (formID != 0) {
            return std::format("{} [{:08X}]", pluginFolder.empty() ? "FaceGeom" : pluginFolder, formID);
        }
    }

    return stem.empty() ? nifPath : stem;
}

static std::string ResolveDefaultFaceGeomPath(RE::TESNPC* npc)
{
    if (!npc) {
        return {};
    }

    for (const auto& face : scannedFaces) {
        if (face.originFormID == npc->GetFormID()) {
            return face.nifPath;
        }
    }

    const auto* file = FormUtil::GetMasterFile(npc);
    if (!file) {
        return {};
    }

    const auto localID = file->IsLight() ? (npc->GetFormID() & 0x00000FFF) : (npc->GetFormID() & 0x00FFFFFF);
    auto nifPath = std::format("meshes\\actors\\character\\FaceGenData\\FaceGeom\\{}\\{:08X}.nif", file->GetFilename(), localID);
    auto dataPath = std::filesystem::path("Data") / nifPath;
    if (std::filesystem::exists(dataPath)) {
        return nifPath;
    }

    return {};
}

// ==========================================
// FUN��ES DE EXPORTA��O ZIP (MINIZ)
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
    EnsureStorageDirectories();
    std::string sourcePath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, presetName).string();
    if (!fs::exists(sourcePath)) return;

    // PONTO 4: Pasta padr�o de exports
    fs::path exportDir = ExportPath;
    fs::create_directories(exportDir);

    std::string zipPath = (exportDir / (SanitizeFilename(presetName) + "_Preset.zip")).string();

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) {
        logger::error("Export: Failed to initialize ZIP file at {}", zipPath);
        return;
    }

    std::string internalZipPath = std::format("Viny Mods/NPC Visual/Presets/{}.json", presetName);

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
    EnsureStorageDirectories();

    std::string editorID = clib_util::editorID::get_editorID(npc);
    if (editorID.empty()) editorID = std::format("{:08X}", npc->GetFormID());
    std::string npcSourcePath = ResolveJsonPath(NPCPath, LegacyNPCPath, editorID).string();

    if (!fs::exists(npcSourcePath)) return;

    // PONTO 4: Pasta padr�o de exports
    fs::path exportDir = ExportPath;
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
    std::string internalNpcPath = std::format("Viny Mods/NPC Visual/NPC/{}.json", editorID);
    mz_zip_writer_add_file(&zip_archive, internalNpcPath.c_str(), npcSourcePath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION);

    // PONTO 3: Se tiver um Preset linkado, exporta ele junto no mesmo ZIP
    if (!linkedPreset.empty()) {
        std::string presetSourcePath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, linkedPreset).string();
        if (fs::exists(presetSourcePath)) {
            std::string internalPresetPath = std::format("Viny Mods/NPC Visual/Presets/{}.json", linkedPreset);
            mz_zip_writer_add_file(&zip_archive, internalPresetPath.c_str(), presetSourcePath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION);
        }
    }

    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);
    logger::info("NPC '{}' successfully exported to: {}", editorID, zipPath);
}

static bool AddFileToZipOnce(mz_zip_archive& zip, std::set<std::string>& addedPaths, const std::filesystem::path& source, const std::string& internalPath)
{
    if (!std::filesystem::exists(source) || !addedPaths.insert(internalPath).second) {
        return true;
    }

    if (!mz_zip_writer_add_file(&zip, internalPath.c_str(), source.string().c_str(), nullptr, 0, MZ_BEST_COMPRESSION)) {
        logger::error("Export: Failed to add '{}' as '{}'", source.string(), internalPath);
        return false;
    }
    return true;
}

static std::string ReadLinkedPresetName(const std::filesystem::path& npcJson)
{
    rapidjson::Document doc;
    if (ReadJsonObjectFromFile(npcJson, doc, "ReadLinkedPresetName") &&
        doc.HasMember("preset") && doc["preset"].IsString()) {
        return doc["preset"].GetString();
    }
    return {};
}

static void ExportSelectedPackage(const std::string& packageName, const std::set<std::string>& presetNames, const std::set<std::string>& npcNames)
{
    EnsureStorageDirectories();
    if (presetNames.empty() && npcNames.empty()) {
        logger::warn("Export: no presets or NPCs selected.");
        return;
    }

    namespace fs = std::filesystem;
    fs::create_directories(ExportPath);
    const auto safeName = SanitizeFilename(packageName.empty() ? "NPCVisual_Export" : packageName);
    const auto zipPath = (fs::path(ExportPath) / (safeName + ".zip")).string();

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));
    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) {
        logger::error("Export: Failed to initialize ZIP file at {}", zipPath);
        return;
    }

    std::set<std::string> addedPaths;
    std::set<std::string> presetsToExport = presetNames;

    for (const auto& npcName : npcNames) {
        const auto npcPath = ResolveJsonPath(NPCPath, LegacyNPCPath, npcName);
        const auto linkedPreset = ReadLinkedPresetName(npcPath);
        if (!linkedPreset.empty()) {
            presetsToExport.insert(linkedPreset);
        }
        AddFileToZipOnce(zip_archive, addedPaths, npcPath, std::format("Viny Mods/NPC Visual/NPC/{}.json", npcName));
    }

    for (const auto& presetName : presetsToExport) {
        AddFileToZipOnce(zip_archive, addedPaths, ResolveJsonPath(PresetsPath, LegacyPresetsPath, presetName),
            std::format("Viny Mods/NPC Visual/Presets/{}.json", presetName));
    }

    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);
    logger::info("Export package successfully written to: {}", zipPath);
}

bool IsHeadPartValid(RE::BGSHeadPart* hp, RE::TESRace* race, bool isFemale) {
    if (!hp) return true;

    // 1. Checa o Sexo
    bool hpFemale = hp->flags.all(RE::BGSHeadPart::Flag::kFemale);
    bool hpMale = hp->flags.all(RE::BGSHeadPart::Flag::kMale);
    if (isFemale && hpMale && !hpFemale) return false;
    if (!isFemale && hpFemale && !hpMale) return false;

    // 2. Checa a Ra�a
    if (race) {
        // Se a propriedade validRaces for nula, a pe�a � v�lida para todas as ra�as.
        if (!hp->validRaces) {
            return true;
        }

        // Verifica se a ra�a do NPC est� explicitamente na lista
        if (hp->validRaces->HasForm(race)) {
            return true;
        }

        // FALLBACK: Ra�as derivadas (ex: Vampiros)
        // O Skyrim usa a armorParentRace (ex: NordRaceVampire -> NordRace) para herdar HeadParts.
        if (race->armorParentRace && hp->validRaces->HasForm(race->armorParentRace)) {
            return true;
        }

        // EXCE��O: Se a lista existe, mas o criador do mod deixou ela vazia.
        // Assumimos que a inten��o era ser v�lida para todas as ra�as.
        if (hp->validRaces->forms.empty()) {
            return true;
        }

        // Se chegou aqui, a ra�a (ou a ra�a base) realmente n�o tem permiss�o para usar esta pe�a.
        return false;
    }

    return true;
}

// Fun��o auxiliar para listar as ra�as permitidas de uma HeadPart em formato de texto
std::string GetValidRacesString(RE::BGSHeadPart* hp) {
    if (!hp || !hp->validRaces || hp->validRaces->forms.empty()) return "Any";

    std::string races = "";
    for (auto form : hp->validRaces->forms) {
        if (form) {
            std::string edid = clib_util::editorID::get_editorID(form);
            if (edid.empty()) edid = std::format("{:08X}", form->GetFormID());
            races += edid + ", ";
        }
    }
    if (!races.empty()) {
        races.pop_back(); races.pop_back(); // Remove a �ltima v�rgula e espa�o
    }
    return races;
}

// Fun��o Auxiliar para descobrir o que o Index do Tint significa baseado na Ra�a e Sexo
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

struct CustomPresetInfo {
    std::string name;
    void* textureID;
};
static std::vector<CustomPresetInfo> scannedPresets;
static bool openPresetSelectModal = false;

void RefreshAvailablePresets() {
    scannedPresets.clear();
    ui_availablePresets.clear();
    EnsureStorageDirectories();

    for (const auto& presetPath : CollectJsonFiles(PresetsPath, LegacyPresetsPath)) {
        std::filesystem::directory_entry entry(presetPath);
        if (entry.path().extension() == ".json") {
            CustomPresetInfo info;
            info.name = entry.path().stem().string();
            ui_availablePresets.push_back(info.name); // Mant�m compatibilidade com fun��es antigas

            std::filesystem::path pngPath = entry.path();
            pngPath.replace_extension(".png");

            // Verifica e carrega a imagem do preset se existir
            if (std::filesystem::exists(pngPath)) {
                info.textureID = SKSEMenuFramework::LoadTexture(pngPath.string());
            }
            else {
                info.textureID = nullptr;
            }

            scannedPresets.push_back(info);
        }
    }
}

void GenerateJSONFromUI(rapidjson::Document& doc) {
    auto& allocator = doc.GetAllocator();
    doc.SetObject();

    doc.AddMember("height", ui_height, allocator);
    doc.AddMember("weight", ui_weight, allocator);
    doc.AddMember("isFemale", ui_isFemale, allocator);
    doc.AddMember("oppositeGenderAnim", ui_oppositeGenderAnim, allocator);

    rapidjson::Value bodyTint(rapidjson::kObjectType);
    bodyTint.AddMember("r", static_cast<int>(ui_bodyColor[0] * 255.0f), allocator);
    bodyTint.AddMember("g", static_cast<int>(ui_bodyColor[1] * 255.0f), allocator);
    bodyTint.AddMember("b", static_cast<int>(ui_bodyColor[2] * 255.0f), allocator);
    bodyTint.AddMember("a", static_cast<int>(ui_bodyColor[3] * 255.0f), allocator);
    doc.AddMember("bodyTintColor", bodyTint, allocator);

    if (ui_race) doc.AddMember("race", FormUtil::MakeFormRef(ui_race, allocator), allocator);
    if (ui_skin) doc.AddMember("skin", FormUtil::MakeFormRef(ui_skin, allocator), allocator);
    if (ui_outfit) doc.AddMember("defaultOutfit", FormUtil::MakeFormRef(ui_outfit, allocator), allocator);
    else doc.AddMember("defaultOutfit", "", allocator);
    if (ui_sleepOutfit) doc.AddMember("sleepOutfit", FormUtil::MakeFormRef(ui_sleepOutfit, allocator), allocator);
    else doc.AddMember("sleepOutfit", "", allocator);
    if (ui_voice) doc.AddMember("voice", FormUtil::MakeFormRef(ui_voice, allocator), allocator);
    else doc.AddMember("voice", "", allocator);
    if (ui_hairColor) doc.AddMember("hairColor", FormUtil::MakeFormRef(ui_hairColor, allocator), allocator);
    if (!ui_customFaceNif.empty()) {
        doc.AddMember("customFaceNif", rapidjson::Value(ui_customFaceNif.c_str(), allocator), allocator);
    }

    rapidjson::Value hpArray(rapidjson::kArrayType);
    for (auto* hp : ui_headParts) {
        if (!hp) continue;
        hpArray.PushBack(FormUtil::MakeFormRef(hp, allocator), allocator);
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
    // Limpa os dados de ponteiro primeiro para evitar lixo de memoria / estado anterior.
    ui_race = nullptr;
    ui_skin = nullptr;
    ui_outfit = nullptr;
    ui_sleepOutfit = nullptr;
    ui_hairColor = nullptr;
    ui_voice = nullptr;
    ui_customFaceNif = "";

    auto getFloat = [](const rapidjson::Value& obj, const char* key, float fallback) {
        return obj.IsObject() && obj.HasMember(key) && obj[key].IsNumber() ? obj[key].GetFloat() : fallback;
        };
    auto getInt = [](const rapidjson::Value& obj, const char* key, int fallback) {
        return obj.IsObject() && obj.HasMember(key) && obj[key].IsNumber() ? obj[key].GetInt() : fallback;
        };

    if (!j.IsObject()) {
        return;
    }

    if (j.HasMember("height") && j["height"].IsNumber()) ui_height = j["height"].GetFloat();
    if (j.HasMember("weight") && j["weight"].IsNumber()) ui_weight = j["weight"].GetFloat();
    if (j.HasMember("isFemale") && j["isFemale"].IsBool()) ui_isFemale = j["isFemale"].GetBool();
    if (j.HasMember("oppositeGenderAnim") && j["oppositeGenderAnim"].IsBool()) ui_oppositeGenderAnim = j["oppositeGenderAnim"].GetBool();

    if (j.HasMember("bodyTintColor") && j["bodyTintColor"].IsObject()) {
        const auto& c = j["bodyTintColor"];
        ui_bodyColor[0] = getInt(c, "r", 255) / 255.0f;
        ui_bodyColor[1] = getInt(c, "g", 255) / 255.0f;
        ui_bodyColor[2] = getInt(c, "b", 255) / 255.0f;
        ui_bodyColor[3] = getInt(c, "a", 0) / 255.0f;
    }

    if (j.HasMember("race")) ui_race = FormUtil::ResolveForm<RE::TESRace>(j["race"]);
    if (j.HasMember("skin")) ui_skin = FormUtil::ResolveForm<RE::TESObjectARMO>(j["skin"]);
    if (j.HasMember("defaultOutfit")) ui_outfit = FormUtil::ResolveForm<RE::BGSOutfit>(j["defaultOutfit"]);
    if (j.HasMember("sleepOutfit")) ui_sleepOutfit = FormUtil::ResolveForm<RE::BGSOutfit>(j["sleepOutfit"]);
    if (j.HasMember("voice")) ui_voice = FormUtil::ResolveForm<RE::BGSVoiceType>(j["voice"]);
    if (j.HasMember("hairColor")) ui_hairColor = FormUtil::ResolveForm<RE::BGSColorForm>(j["hairColor"]);

    ui_headParts.clear();
    if (j.HasMember("headParts") && j["headParts"].IsArray()) {
        for (const auto& hpJson : j["headParts"].GetArray()) {
            if (!hpJson.IsString() && !hpJson.IsObject() && !hpJson.IsUint()) {
                continue;
            }
            if (auto hp = FormUtil::ResolveForm<RE::BGSHeadPart>(hpJson)) {
                ui_headParts.push_back(hp);
            }
        }
    }

    ui_tintLayers.clear();
    if (j.HasMember("tintLayers") && j["tintLayers"].IsArray()) {
        for (const auto& l : j["tintLayers"].GetArray()) {
            if (!l.IsObject() || !l.HasMember("index") || !l["index"].IsNumber()) {
                continue;
            }

            UITintLayer tl{};
            tl.index = static_cast<uint16_t>(l["index"].GetInt());
            tl.name = GetSkinToneName(ui_race, ui_isFemale ? RE::SEX::kFemale : RE::SEX::kMale, tl.index);

            if (l.HasMember("color") && l["color"].IsObject()) {
                const auto& colorObj = l["color"];
                tl.color[0] = getInt(colorObj, "r", 0) / 255.0f;
                tl.color[1] = getInt(colorObj, "g", 0) / 255.0f;
                tl.color[2] = getInt(colorObj, "b", 0) / 255.0f;
                tl.color[3] = getInt(colorObj, "a", 255) / 255.0f;
            }
            else {
                tl.color[0] = 0.0f;
                tl.color[1] = 0.0f;
                tl.color[2] = 0.0f;
                tl.color[3] = 1.0f;
            }

            tl.interpolation = getFloat(l, "interpolation", 1.0f);
            tl.preset = static_cast<uint16_t>(getInt(l, "preset", 0));
            ui_tintLayers.push_back(tl);
        }
    }

    if (j.HasMember("faceMorphs") && j["faceMorphs"].IsArray()) {
        for (int i = 0; i < 19; i++) {
            ui_morphs[i] = 3.402823466e+38f;
        }
        const auto mArray = j["faceMorphs"].GetArray();
        for (rapidjson::SizeType i = 0; i < mArray.Size() && i < 19; i++) {
            if (mArray[i].IsNumber()) {
                ui_morphs[i] = mArray[i].GetFloat();
            }
        }
    }
    else {
        for (int i = 0; i < 19; i++) ui_morphs[i] = 3.402823466e+38f;
    }

    if (j.HasMember("customFaceNif") && j["customFaceNif"].IsString()) {
        ui_customFaceNif = j["customFaceNif"].GetString();
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

    // CORRE��O 2: Pega o Label, acha o "##" e oculta o ID da frente do texto visual
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
            // PONTO 4: Campo de pesquisa din�mico para os itens do dropdown
            static std::map<std::string, std::string> searchBuffers;
            char searchBuf[256] = "";
            if (searchBuffers.contains(label)) strcpy_s(searchBuf, searchBuffers[label].c_str());

            ImGuiMCP::SetNextItemWidth(-1.0f); // Ocupa toda a largura do combo popup
            if (ImGuiMCP::InputText("##busca", searchBuf, sizeof(searchBuf))) {
                searchBuffers[label] = searchBuf;
            }
            ImGuiMCP::Separator();

            std::string searchStr = searchBuf;
            std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), [](unsigned char c) { return std::tolower(c); });

            ImGuiMCP::BeginChild("##scroll", ImGuiMCP::ImVec2(0, 200), false);
            for (int i = 0; i < comboItems.size(); i++) {
                std::string itemStr = comboItems[i];
                std::string itemLower = itemStr;
                std::transform(itemLower.begin(), itemLower.end(), itemLower.begin(), [](unsigned char c) { return std::tolower(c); });

                // Mostra apenas itens que cont�m o que foi digitado
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

    std::string path = ResolveJsonPath(PresetsPath, LegacyPresetsPath, presetName).string();
    rapidjson::Document doc;
    if (ReadJsonObjectFromFile(path, doc, "UI/LoadPreset")) {
        ParseJSONToUI(doc);
        logger::info("Preset {} loaded into UI.", presetName);
    }

    UpdateLastSavedState(); // Atualiza a "fotografia" do estado guardado
}

void LoadNPCToUI(RE::TESNPC* npcToLoad = nullptr, RE::Actor* actorRef = nullptr) {
    logger::debug("[UI EditNPC] LoadNPCToUI BEGIN npcToLoad={:X} actorRef={:X}",
        reinterpret_cast<std::uintptr_t>(npcToLoad),
        reinterpret_cast<std::uintptr_t>(actorRef));

    if (!npcToLoad) {
        logger::debug("[UI EditNPC] Resolving NPC from console selected ref BEGIN");
        auto ref = RE::Console::GetSelectedRef();
        if (!ref) { logger::warn("No NPC selected."); return; }
        g_currentActor = ref->As<RE::Actor>();
        if (!g_currentActor) {
            logger::debug("[UI EditNPC] Console selected ref is not an actor ptr={:X}", reinterpret_cast<std::uintptr_t>(ref.get()));
            return;
        }
        g_currentNPC = g_currentActor->GetActorBase();
        logger::debug("[UI EditNPC] Resolving NPC from console selected ref END actor={:08X}",
            g_currentActor ? g_currentActor->GetFormID() : 0);
    }
    else {
        g_currentNPC = npcToLoad;
        g_currentActor = actorRef;
    }

    if (!g_currentNPC) {
        logger::debug("[UI EditNPC] ABORT: g_currentNPC is null");
        return;
    }

    const auto npcEditorID = clib_util::editorID::get_editorID(g_currentNPC);
    logger::debug("[UI EditNPC] NPC resolved form={:08X} editorID='{}' name='{}' npcPtr={:X} actorPtr={:X} headParts={} tintLayersPtr={:X} faceData={:X}",
        g_currentNPC->GetFormID(),
        npcEditorID,
        g_currentNPC->GetFullName() ? g_currentNPC->GetFullName() : "",
        reinterpret_cast<std::uintptr_t>(g_currentNPC),
        reinterpret_cast<std::uintptr_t>(g_currentActor),
        static_cast<std::uint32_t>(g_currentNPC->numHeadParts),
        reinterpret_cast<std::uintptr_t>(g_currentNPC->tintLayers),
        reinterpret_cast<std::uintptr_t>(g_currentNPC->faceData));

    logger::debug("[UI EditNPC] DumpFaceDiagnostics BEGIN npc={:08X}", g_currentNPC->GetFormID());
    Manager::DumpFaceDiagnostics(g_currentActor, g_currentNPC, "UI LoadNPCToUI");
    logger::debug("[UI EditNPC] DumpFaceDiagnostics END npc={:08X}", g_currentNPC->GetFormID());

    if (!g_vanillaNPCStates.contains(g_currentNPC->GetFormID())) {
        logger::debug("[UI EditNPC] CaptureVanillaState BEGIN npc={:08X}", g_currentNPC->GetFormID());
        std::string vanillaStr;
        CaptureVanillaState(g_currentNPC, vanillaStr);
        g_vanillaNPCStates[g_currentNPC->GetFormID()] = vanillaStr;
        logger::debug("[UI EditNPC] CaptureVanillaState END npc={:08X} bytes={}", g_currentNPC->GetFormID(), vanillaStr.size());
    }
    else {
        logger::debug("[UI EditNPC] CaptureVanillaState SKIP cached npc={:08X}", g_currentNPC->GetFormID());
    }

    isEditingPreset = false;
    activePresetName = "";
    ui_linkedPreset = "";
    ui_customFaceNif = "";

    ui_height = g_currentNPC->height;
    ui_weight = g_currentNPC->weight;
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
    ui_voice = g_currentNPC->voiceType;
    ui_hairColor = g_currentNPC->headRelatedData ? g_currentNPC->headRelatedData->hairColor : nullptr;

    ui_headParts.clear();

    //Mapeia todas as "Extra Parts" que pertencem a alguma pe�a principal que o NPC tem equipada.
    std::set<RE::BGSHeadPart*> ownedExtraParts;
    if (g_currentNPC->headParts) {
        for (int i = 0; i < g_currentNPC->numHeadParts; i++) {
            auto hp = g_currentNPC->headParts[i];
            if (hp) {
                for (auto* extra : hp->extraParts) {
                    if (extra) ownedExtraParts.insert(extra);
                }
            }
        }
    }

    // Adiciona � UI apenas as pe�as que N�O s�o "filhas" de outra pe�a j� equipada.
    for (int i = 0; i < g_currentNPC->numHeadParts; i++) {
        if (g_currentNPC->headParts && g_currentNPC->headParts[i]) {
            auto hp = g_currentNPC->headParts[i];

            // Se esta headpart N�O estiver na lista de filhas ownedExtraParts, mostramos na UI.
            if (ownedExtraParts.find(hp) == ownedExtraParts.end()) {
                ui_headParts.push_back(hp);
            }
        }
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
    std::string filePath = ResolveJsonPath(NPCPath, LegacyNPCPath, editorID).string();
    logger::debug("[UI EditNPC] Checking saved NPC JSON npc={:08X} editorID='{}' path='{}' exists={}",
        g_currentNPC->GetFormID(),
        editorID,
        filePath,
        std::filesystem::exists(filePath));

    if (std::filesystem::exists(filePath)) {
        logger::debug("[UI EditNPC] Read saved NPC JSON BEGIN path='{}'", filePath);
        FILE* fp = nullptr;
        fopen_s(&fp, filePath.c_str(), "rb");
        if (fp) {
            char readBuffer[65536];
            rapidjson::FileReadStream is(fp, readBuffer, sizeof(readBuffer));
            rapidjson::Document doc;
            doc.ParseStream(is);
            fclose(fp);
            logger::debug("[UI EditNPC] Read saved NPC JSON END path='{}' isObject={} parseError={}",
                filePath,
                doc.IsObject(),
                doc.HasParseError());
            if (doc.IsObject() && doc.HasMember("preset") && doc["preset"].IsString()) {
                ui_linkedPreset = doc["preset"].GetString();
                std::string presetPath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, ui_linkedPreset).string();
                logger::debug("[UI EditNPC] Linked preset detected npc={:08X} preset='{}' path='{}' exists={}",
                    g_currentNPC->GetFormID(),
                    ui_linkedPreset,
                    presetPath,
                    std::filesystem::exists(presetPath));
                FILE* presetFp = nullptr;
                fopen_s(&presetFp, presetPath.c_str(), "rb");
                if (presetFp) {
                    logger::debug("[UI EditNPC] Read linked preset JSON BEGIN path='{}'", presetPath);
                    char presetReadBuffer[65536];
                    rapidjson::FileReadStream presetStream(presetFp, presetReadBuffer, sizeof(presetReadBuffer));
                    rapidjson::Document presetDoc;
                    presetDoc.ParseStream(presetStream);
                    fclose(presetFp);
                    logger::debug("[UI EditNPC] Read linked preset JSON END path='{}' isObject={} parseError={}",
                        presetPath,
                        presetDoc.IsObject(),
                        presetDoc.HasParseError());
                    if (presetDoc.IsObject() && presetDoc.HasMember("customFaceNif") && presetDoc["customFaceNif"].IsString()) {
                        ui_customFaceNif = presetDoc["customFaceNif"].GetString();
                    }
                }
            }
            else if (doc.IsObject() && doc.HasMember("customFaceNif") && doc["customFaceNif"].IsString()) {
                ui_customFaceNif = doc["customFaceNif"].GetString();
            }
        }
        else {
            logger::debug("[UI EditNPC] Failed to open saved NPC JSON path='{}'", filePath);
        }
    }

    if (ui_customFaceNif.empty()) {
        if (needFaceScan) {
            logger::debug("[UI EditNPC] ScanFaceGeom BEGIN while resolving default face npc={:08X}", g_currentNPC->GetFormID());
            ScanFaceGeom();
            logger::debug("[UI EditNPC] ScanFaceGeom END while resolving default face npc={:08X}", g_currentNPC->GetFormID());
        }
        logger::debug("[UI EditNPC] ResolveDefaultFaceGeomPath BEGIN npc={:08X}", g_currentNPC->GetFormID());
        ui_customFaceNif = ResolveDefaultFaceGeomPath(g_currentNPC);
        logger::debug("[UI EditNPC] ResolveDefaultFaceGeomPath END npc={:08X} nif='{}'", g_currentNPC->GetFormID(), ui_customFaceNif);
        if (!ui_customFaceNif.empty()) {
            logger::debug("[UI LoadNPCToUI] FaceGeom padrao resolvida para NPC {:08X}: '{}'",
                g_currentNPC->GetFormID(),
                ui_customFaceNif);
        }
    }

    UpdateLastSavedState(); // Atualiza a "fotografia" do estado guardado
    logger::debug("[UI EditNPC] LoadNPCToUI END npc={:08X} linkedPreset='{}' customFaceNif='{}' headParts={} tintLayers={}",
        g_currentNPC->GetFormID(),
        ui_linkedPreset,
        ui_customFaceNif,
        ui_headParts.size(),
        ui_tintLayers.size());
}

void SaveData() {
    EnsureStorageDirectories();
    rapidjson::Document doc;
    auto& allocator = doc.GetAllocator();
    doc.SetObject();

    std::string finalPath;
    std::string savedName;
    std::string savedFolder;
    std::string savedLegacyFolder;

    if (isEditingPreset) {
        GenerateJSONFromUI(doc);
        savedName = activePresetName;
        savedFolder = PresetsPath;
        savedLegacyFolder = LegacyPresetsPath;
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
        savedName = editorID;
        savedFolder = NPCPath;
        savedLegacyFolder = LegacyNPCPath;
        finalPath = std::format("{}/{}.json", NPCPath, editorID);
    }

    bool savedOk = false;
    FILE* fp = nullptr;
    fopen_s(&fp, finalPath.c_str(), "wb");
    if (fp) {
        char writeBuffer[65536];
        rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
        rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
        doc.Accept(writer);
        fclose(fp);
        savedOk = true;
        logger::info("Data saved to {}", finalPath);
    }
    else {
        logger::error("Failed to save data to {}", finalPath);
    }

    if (savedOk && !savedName.empty()) {
        // A migracao legacy -> pasta nova acontece aqui, e apenas aqui,
        // depois de uma gravacao bem-sucedida.
        FinalizeLegacyMigrationAfterSave(savedFolder, savedLegacyFolder, savedName);
    }

    if (isEditingPreset && !activePresetName.empty()) {
        for (const auto& npcJson : CollectJsonFiles(NPCPath, LegacyNPCPath)) {
            std::filesystem::directory_entry entry(npcJson);
            if (entry.path().extension() == ".json") {
                rapidjson::Document npcDoc;
                if (ReadJsonObjectFromFile(entry.path(), npcDoc, "SaveData/PresetUsers") &&
                    npcDoc.HasMember("preset") && npcDoc["preset"].IsString() &&
                    npcDoc["preset"].GetString() == activePresetName) {

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
                        rapidjson::Document safeDoc = BuildSafeCustomizationDocument(targetNPC, doc);
                        Manager::ApplyNPCCustomizationFromJSON(targetNPC, safeDoc);
                        logger::info("Updated NPC base {} with modified preset '{}'", filename, activePresetName);

                        auto processLists = RE::ProcessLists::GetSingleton();
                        if (processLists) {
                            for (auto& actorHandle : processLists->highActorHandles) {
                                auto ref = actorHandle.get();
                                if (ref) {
                                    if (auto actor = ref->As<RE::Actor>()) {
                                        if (!actor->IsPlayerRef() && actor->GetActorBase() == targetNPC) {
                                            actor->UpdateHairColor();
                                            actor->UpdateSkinColor();
                                            //actor->DoReset3D(true);
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
    logger::debug("[UI Apply] === BEGIN npc={:08X} npcPtr={:X} actorPtr={:X} force3DReset={} linkedPreset='{}' ===",
        g_currentNPC->GetFormID(),
        reinterpret_cast<std::uintptr_t>(g_currentNPC),
        reinterpret_cast<std::uintptr_t>(g_currentActor),
        force3DReset,
        ui_linkedPreset);
    rapidjson::Document doc;

    if (!ui_linkedPreset.empty()) {
        std::string pPath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, ui_linkedPreset).string();
        logger::debug("[UI Apply] Carregando preset linkado '{}': {}", ui_linkedPreset, pPath);
        rapidjson::Document loadedPreset;
        if (ReadJsonObjectFromFile(pPath, loadedPreset, "UI/ApplyPreset")) {
            doc = BuildSafeCustomizationDocument(g_currentNPC, loadedPreset);
            logger::debug("[UI Apply] Preset parse OK: isObject={} parseError={} members={}",
                doc.IsObject(),
                doc.HasParseError(),
                doc.IsObject() ? doc.MemberCount() : 0);
        }
        else {
            logger::error("[UI Apply] Falha ao abrir preset linkado: {}", pPath);
            doc.SetObject();
        }
    }
    else {
        logger::debug("[UI Apply] Gerando JSON a partir da UI...");
        GenerateJSONFromUI(doc);
        logger::debug("[UI Apply] JSON UI gerado: isObject={} members={}", doc.IsObject(), doc.IsObject() ? doc.MemberCount() : 0);
    }
    logger::debug("[UI Apply] Chamando Manager::ApplyNPCCustomizationFromJSON...");
    Manager::ApplyNPCCustomizationFromJSON(g_currentNPC, doc);
    logger::debug("[UI Apply] ApplyNPCCustomizationFromJSON retornou para npc={:08X}.", g_currentNPC->GetFormID());


    std::string nifToApply = "";
    if (doc.HasMember("customFaceNif") && doc["customFaceNif"].IsString()) {
        nifToApply = doc["customFaceNif"].GetString();
    }
    logger::debug("[UI Apply] customFaceNif resolvido: '{}'", nifToApply);

    if (!nifToApply.empty()) {
        Manager::GetSingleton()->RegisterAffectedNPC(g_currentNPC->GetFormID(), nifToApply);
        logger::debug("[UI Apply] RegisterAffectedNPC retornou para base={:08X}.", g_currentNPC->GetFormID());
    }
    else {
        Manager::GetSingleton()->UnregisterAffectedNPC(g_currentNPC->GetFormID());
        logger::debug("[UI Apply] customFaceNif vazio; AffectedNPC removido para base={:08X}.", g_currentNPC->GetFormID());
    }

    if (force3DReset) {

        if (g_currentActor) {
            logger::debug("[UI Apply] Actor runtime antes do reset: actor={:08X} actorPtr={:X} loaded={} actor3D={:X} faceNode={:X}",
                g_currentActor->GetFormID(),
                reinterpret_cast<std::uintptr_t>(g_currentActor),
                g_currentActor->Is3DLoaded(),
                reinterpret_cast<std::uintptr_t>(g_currentActor->Get3D(false)),
                reinterpret_cast<std::uintptr_t>(g_currentActor->GetFaceNodeSkinned()));
            logger::debug("[UI Apply] Calling UpdateHairColor...");
            g_currentActor->UpdateHairColor();
            logger::debug("[UI Apply] UpdateHairColor OK. Calling UpdateSkinColor...");
            g_currentActor->UpdateSkinColor();
            logger::debug("[UI Apply] UpdateSkinColor OK. Calling DoReset3D(true)...");
            g_currentActor->DoReset3D(true);
            logger::debug("[UI Apply] DoReset3D(true) retornou actor={:08X} loaded={} actor3D={:X}",
                g_currentActor->GetFormID(),
                g_currentActor->Is3DLoaded(),
                reinterpret_cast<std::uintptr_t>(g_currentActor->Get3D(false)));

            // 3. SCHEDULER: Queue deformation 500ms after Reset3D
            if (!nifToApply.empty()) {
                logger::debug("[UI Apply] Agendando ScheduleFaceDeform actor={:08X} nif='{}'", g_currentActor->GetFormID(), nifToApply);
                Manager::ScheduleFaceDeform(g_currentActor->GetFormID(), nifToApply);
                logger::debug("[UI Apply] ScheduleFaceDeform retornou actor={:08X}.", g_currentActor->GetFormID());
            }
            else {
                logger::debug("[UI Apply] Nenhuma customFaceNif; deformacao nao agendada.");
            }
        }
        else {
            logger::warn("[UI Apply] force3DReset ativo, mas g_currentActor null.");
        }

        //auto processLists = RE::ProcessLists::GetSingleton();
        //if (processLists) {
        //    for (auto& actorHandle : processLists->highActorHandles) {
        //        auto ref = actorHandle.get();
        //        if (ref) {
        //            if (auto actor = ref->As<RE::Actor>()) {
        //                if (!actor->IsPlayerRef() && actor != g_currentActor && actor->GetActorBase() == g_currentNPC) {
        //                    actor->UpdateHairColor();
        //                    actor->UpdateSkinColor();
        //                    //actor->DoReset3D(true);

        //                    // Queue deformation for all instanced actors using this base!
        //                    if (!nifToApply.empty()) {
        //                        Manager::ScheduleFaceDeform(actor->GetFormID(), nifToApply);
        //                    }
        //                }
        //            }
        //        }
        //    }
        //}
    }
    else {
        logger::debug("[UI Apply] force3DReset=false; reset/deformacao nao executados.");
    }

    logger::debug("[UI Apply] === END npc={:08X} ===", g_currentNPC->GetFormID());
}

// Bot�o "Voltar ao Padr�o"
void RevertNPC() {
    if (!g_currentNPC || !originalEngineState.IsObject()) return;

    // Volta UI e Motor para o que estava gravado no Backup quando lemos o NPC
    ui_linkedPreset = "";
    Manager::ApplyNPCCustomizationFromJSON(g_currentNPC, originalEngineState);

    // Recarrega a UI baseada na engine rec�m-restaurada
    LoadNPCToUI(g_currentNPC, g_currentActor);

    if (g_currentActor) {
        g_currentActor->UpdateSkinColor();
        g_currentActor->UpdateHairColor();
        g_currentActor->Update3DModel();
        g_currentActor->DoReset3D(true);
    }
}


void RestoreDefaultNPC() {
    if (!g_currentNPC) return;

    // 1. Apaga o ficheiro JSON que define as altera��es customizadas deste NPC
    std::string editorID = clib_util::editorID::get_editorID(g_currentNPC);
    if (editorID.empty()) editorID = std::format("{:08X}", g_currentNPC->GetFormID());
    RemoveJsonInBothLocations(NPCPath, LegacyNPCPath, editorID);

    ui_linkedPreset = "";
    ui_customFaceNif = "";

    // 2. Aplica o estado LIMPO E ORIGINAL � mem�ria (Se estiver no Cofre global)
    if (g_vanillaNPCStates.contains(g_currentNPC->GetFormID())) {
        rapidjson::Document doc;
        doc.Parse(g_vanillaNPCStates[g_currentNPC->GetFormID()].c_str());
        Manager::ApplyNPCCustomizationFromJSON(g_currentNPC, doc);
    }
    else {
        // Fallback de seguran�a 
        Manager::ApplyNPCCustomizationFromJSON(g_currentNPC, originalEngineState);
    }

    // 3. Atualiza a UI para refletir os valores base restaurados
    LoadNPCToUI(g_currentNPC, g_currentActor);
    Manager::GetSingleton()->UnregisterAffectedNPC(g_currentNPC->GetFormID());
    // 4. D� reset ao modelo 3D do personagem no mapa
    if (g_currentActor) {
        g_currentActor->UpdateSkinColor();
        g_currentActor->UpdateHairColor();
        g_currentActor->Update3DModel();
        g_currentActor->DoReset3D(true);
    }
    logger::info("Restored {} to absolute default (JSON deleted and Base Form reverted).", editorID);
}

void ScanFaceGeom() {
    scannedFaces.clear();
    auto* manager = Manager::GetSingleton();
    manager->ClearFaceGenGeometryIndex();
    std::filesystem::path geomPath = "Data/meshes/actors/character/FaceGenData/FaceGeom";

    std::error_code ec;
    if (!std::filesystem::exists(geomPath, ec) || ec) {
        logger::warn("[ScanFaceGeom] FaceGeom path does not exist or cannot be accessed.");
        return;
    }

    logger::debug("[ScanFaceGeom] Starting FaceGeom scan...");

    // Adiciona flag para pular arquivos/pastas sem permiss�o
    auto options = std::filesystem::directory_options::skip_permission_denied;

    for (auto it = std::filesystem::recursive_directory_iterator(geomPath, options, ec);
        it != std::filesystem::recursive_directory_iterator();
        it.increment(ec)) {

        if (ec) {
            logger::error("[ScanFaceGeom] Filesystem iteration error: {}", ec.message());
            continue;
        }

        if (it->is_regular_file() && it->path().extension() == ".nif") {
            CustomFaceInfo info;

            try {
                // A convers�o .string() foi movida para DENTRO do try. 
                // Evita o crash "No mapping for the Unicode character"
                std::string fullPath = it->path().string();

                // Log detalhado para rastrear onde o scanner est� (pode mudar para logger::info se preferir)
                logger::debug("[ScanFaceGeom] Analyzing NIF: {}", fullPath);

                std::string normalizedPath = fullPath;
                std::replace(normalizedPath.begin(), normalizedPath.end(), '/', '\\');

                // 1. Path Relativo para a Game Engine
                size_t meshPos = normalizedPath.find("meshes\\");
                if (meshPos != std::string::npos) info.nifPath = normalizedPath.substr(meshPos);
                else info.nifPath = normalizedPath;

                // 2. Path para a UI
                size_t faceGeomPos = normalizedPath.find("FaceGeom\\");
                if (faceGeomPos != std::string::npos) {
                    info.displayPath = normalizedPath.substr(faceGeomPos + 9);
                }
                else {
                    info.displayPath = info.nifPath;
                }

                // 3. Resolu��o do Nome
                std::string stem = it->path().stem().string();
                std::string pluginFolder = it->path().parent_path().filename().string();

                info.originFormID = 0;

                // --- CHECAGEM ESTRITA DE HEXADECIMAL ---
                // Verifica se o nome � 100% FormID. Evita que nomes como "bela_rosto" 
                // sejam lidos parcialmente como hex (0xBE).
                bool isStrictHex = true;
                if (stem.empty() || stem.length() > 8) {
                    isStrictHex = false;
                }
                else {
                    for (char c : stem) {
                        if (!std::isxdigit(static_cast<unsigned char>(c))) {
                            isStrictHex = false;
                            break;
                        }
                    }
                }

                if (isStrictHex) {
                    // � um FormID de NPC
                    uint32_t rawID = std::stoul(stem, nullptr, 16);
                    auto dataHandler = RE::TESDataHandler::GetSingleton();

                    if (dataHandler) {
                        const RE::TESFile* modFile = dataHandler->LookupModByName(pluginFolder);
                        if (modFile) {
                            uint32_t localID = 0;

                            if (modFile->IsLight()) {
                                localID = rawID & 0x00000FFF;
                            }
                            else {
                                localID = rawID & 0x00FFFFFF;
                            }

                            info.originFormID = dataHandler->LookupFormID(localID, pluginFolder);
                        }
                    }

                    if (info.originFormID == 0) info.originFormID = rawID;

                    if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(info.originFormID)) {
                        info.displayName = std::format("{} [{:08X}]", npc->GetFullName() ? npc->GetFullName() : "Unnamed", info.originFormID);
                    }
                    else {
                        info.displayName = std::format("{} [{:08X}]", pluginFolder.empty() ? "FaceGeom" : pluginFolder, info.originFormID);
                    }
                }
                else {
                    // � UM ARQUIVO COM NOME CUSTOMIZADO (Ex: "Rosto_da_Lydia")
                    logger::debug("[ScanFaceGeom] Custom Face detected (Non-FormID): {}", stem);
                    info.originFormID = 0;
                    info.displayName = stem;
                }

                // Carregamento de Textura (PNG)
                std::filesystem::path pngPath = it->path();
                pngPath.replace_extension(".png");

                if (std::filesystem::exists(pngPath, ec)) {
                    info.textureID = SKSEMenuFramework::LoadTexture(pngPath.string());
                }
                else {
                    info.textureID = nullptr;
                }

                scannedFaces.push_back(info);
                manager->IndexFaceGenNif(info.nifPath, info.originFormID);

            }
            // CATCH ESPEC�FICO PARA O ERRO DE UNICODE DO WINDOWS
            catch (const std::system_error& se) {
                logger::error("[ScanFaceGeom] Filesystem/Unicode encoding error reading a file. Skipping it. Info: {}", se.what());
            }
            catch (const std::exception& e) {
                logger::error("[ScanFaceGeom] Standard Exception processing file: {}", e.what());
            }
            catch (...) {
                logger::error("[ScanFaceGeom] Unknown critical exception processing a FaceGeom file.");
            }
        }
    }

    logger::debug("[ScanFaceGeom] Scan completed. Faces found: {}. Baked geometries indexed: {}. Duplicates ignored: {}.",
        scannedFaces.size(),
        manager->GetFaceGenGeometryIndexSize(),
        manager->GetFaceGenGeometryDuplicateCount());
    needFaceScan = false;
}

void DrawMainEditorUI() {
    bool isLocked = (!ui_linkedPreset.empty() && !isEditingPreset);
    bool hasHeadPartWarnings = false;
    std::string headPartWarningMsgs = "";

    // Deteta se o utilizador alterou qualquer coisa no ecr�
    bool isDirty = HasUnsavedChanges();

    // Novo mapa para agrupar HeadParts pela categoria e identificar duplicatas
    std::map<RE::BGSHeadPart::HeadPartType, std::vector<RE::BGSHeadPart*>> hpByType;

    for (auto hp : ui_headParts) {
        if (!hp) continue;

        // 1. Checa Incompatibilidade (Ra�a e Sexo)
        if (!IsHeadPartValid(hp, ui_race, ui_isFemale)) {
            hasHeadPartWarnings = true;

            std::string allowedRaces = GetValidRacesString(hp);

            bool hpFemale = hp->flags.all(RE::BGSHeadPart::Flag::kFemale);
            bool hpMale = hp->flags.all(RE::BGSHeadPart::Flag::kMale);
            std::string allowedSex = "Any";
            if (hpFemale && !hpMale) allowedSex = "Female";
            else if (hpMale && !hpFemale) allowedSex = "Male";

            std::string edid = clib_util::editorID::get_editorID(hp);
            if (edid.empty()) edid = std::format("{:08X}", hp->GetFormID());

            headPartWarningMsgs += std::format("- Incompatible: {} (Races: {} | Sex: {})\n", edid, allowedRaces, allowedSex);
        }

        // 2. Agrupa as HeadParts pelo Tipo Base (Ignorando kIsExtraPart)
        if (!hp->flags.all(RE::BGSHeadPart::Flag::kIsExtraPart)) {
            hpByType[hp->type.get()].push_back(hp);
        }
    }

    // 3. Checa conflitos de m�ltiplas pe�as BASE do mesmo tipo
    for (const auto& [type, parts] : hpByType) {
        // kMisc e kScar s�o exce��es, o Skyrim permite m�ltiplos deles
        if (parts.size() > 1 && type != RE::BGSHeadPart::HeadPartType::kMisc && type != RE::BGSHeadPart::HeadPartType::kScar) {
            hasHeadPartWarnings = true;

            std::string typeName = "Unknown";
            switch (type) {
            case RE::BGSHeadPart::HeadPartType::kHair: typeName = "Hair"; break;
            case RE::BGSHeadPart::HeadPartType::kEyes: typeName = "Eyes"; break;
            case RE::BGSHeadPart::HeadPartType::kFace: typeName = "Face"; break;
            case RE::BGSHeadPart::HeadPartType::kEyebrows: typeName = "Eyebrows"; break;
            case RE::BGSHeadPart::HeadPartType::kFacialHair: typeName = "Facial Hair"; break;
            }

            std::string conflictNames = "";
            for (auto p : parts) {
                std::string edid = clib_util::editorID::get_editorID(p);
                if (edid.empty()) edid = std::format("{:08X}", p->GetFormID());
                conflictNames += edid + ", ";
            }
            if (!conflictNames.empty()) { conflictNames.pop_back(); conflictNames.pop_back(); }

            headPartWarningMsgs += std::format("- Conflict ({}): Multiple base parts -> [{}]\n", typeName, conflictNames);
        }
    }

    if (hasHeadPartWarnings) {
        ImGuiMCP::TextColored({ 1.0f, 0.65f, 0.15f, 1.0f }, "Warning: incompatible HeadParts are selected.");
        ImGuiMCP::TextWrapped("These can cause invisible heads, broken eyes or hair, neck seams, bad morphs, or missing geometry. Use this only for replacers whose FaceGen/NIF was built for that HeadPart, verified cross-race/sex conversions, or special modded parts that intentionally omit vanilla race flags.");
        if (!headPartWarningMsgs.empty()) ImGuiMCP::TextColored({ 1.0f, 0.45f, 0.25f, 1.0f }, headPartWarningMsgs.c_str());
    }

    if (!isEditingPreset && ImGuiMCP::Button("Apply in-game")) { ApplyNPC(); }
    ImGuiMCP::SameLine();

    // D� uma cor de aviso ao bot�o de "SALVAR" se estiver "sujo"
    if (isDirty) ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, { 0.6f, 0.4f, 0.1f, 1.0f });
    if (ImGuiMCP::Button("Save data")) { SaveData(); }
    if (isDirty) ImGuiMCP::PopStyleColor();

    if (!isEditingPreset) {
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Undo Changes")) { RevertNPC(); }

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Restore Default")) { RestoreDefaultNPC(); }

        if (g_currentNPC) {
            std::string editorID = clib_util::editorID::get_editorID(g_currentNPC);
            if (editorID.empty()) editorID = std::format("{:08X}", g_currentNPC->GetFormID());
            std::string filePath = ResolveJsonPath(NPCPath, LegacyNPCPath, editorID).string();

            if (std::filesystem::exists(filePath)) {
                ImGuiMCP::SameLine();

                // Bloqueio do bot�o caso tenha edi��es por guardar
                if (isDirty) {
                    ImGuiMCP::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "[Save to Export]");
                    if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("There are pending changes. Click Save data first.");
                }
                else {
                    if (ImGuiMCP::Button("Export")) {
                        ExportNPCAsZip(g_currentNPC, ui_linkedPreset);
                    }
                    if (ImGuiMCP::IsItemHovered()) {
                        ImGuiMCP::SetTooltip(GetLoc("export.output_hint", "ZIP files are saved to Data/Viny Mods/NPC Visual/Export."));
                    }
                }
            }
        }
    }
    else if (isEditingPreset && !activePresetName.empty()) {
        std::string presetPath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, activePresetName).string();
        if (std::filesystem::exists(presetPath)) {
            ImGuiMCP::SameLine();

            // Bloqueio do bot�o caso tenha edi��es por guardar
            if (isDirty) {
                ImGuiMCP::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "[Save to Export]");
                if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("There are pending changes. Click SAVE DATA first.");
            }
            else {
                if (ImGuiMCP::Button("Export")) {
                    ExportPresetAsZip(activePresetName);
                }
                if (ImGuiMCP::IsItemHovered()) {
                    ImGuiMCP::SetTooltip(GetLoc("export.output_hint", "ZIP files are saved to Data/Viny Mods/NPC Visual/Export."));
                }
            }
        }
    }
    ImGuiMCP::Separator();

    if (isLocked) {
        ImGuiMCP::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f }, "This NPC is locked and using PRESET: %s", ui_linkedPreset.c_str());
        if (ImGuiMCP::Button("Unlink Preset (Free Edit)")) ui_linkedPreset = "";

        // PONTO 1: Bot�o para Editar o Preset linkado
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
            ImGuiMCP::Text("Apply Existing Preset:"); ImGuiMCP::SameLine();
            if (ImGuiMCP::Button("Browse Presets")) {
                RefreshAvailablePresets();
                openPresetSelectModal = true;
            }
        }
        ImGuiMCP::Separator();
    }

    // ==========================================
    // MODAL DE SELE��O DE PRESETS COM IMAGEM
    // ==========================================
    if (openPresetSelectModal) ImGuiMCP::OpenPopup("Select Preset");
    if (ImGuiMCP::BeginPopupModal("Select Preset", &openPresetSelectModal, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
        static char presetSearch[256] = "";
        ImGuiMCP::InputText("Search Preset", presetSearch, sizeof(presetSearch));
        ImGuiMCP::Separator();

        std::string searchStr(presetSearch);
        std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), [](unsigned char c) { return std::tolower(c); });

        std::vector<size_t> filteredIdx;
        for (size_t i = 0; i < scannedPresets.size(); i++) {
            std::string lowerName = scannedPresets[i].name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return std::tolower(c); });

            if (!searchStr.empty() && lowerName.find(searchStr) == std::string::npos) {
                continue;
            }
            filteredIdx.push_back(i);
        }

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_BordersInnerH | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_ScrollY;

        if (ImGuiMCP::BeginTable("PresetSelectTable", 3, tableFlags, ImGuiMCP::ImVec2(800, 600))) {
            ImGuiMCP::TableSetupColumn("Image", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 160.0f);
            ImGuiMCP::TableSetupColumn("Details", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Action", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 130.0f);

            int totalItems = static_cast<int>(filteredIdx.size());
            static auto clipper = ImGuiMCP::ImGuiListClipperManager::Create();
            ImGuiMCP::ImGuiListClipperManager::Begin(clipper, totalItems, 160.0f);

            while (ImGuiMCP::ImGuiListClipperManager::Step(clipper)) {
                for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
                    if (i < 0 || i >= totalItems) continue;

                    size_t sourceIdx = filteredIdx[i];
                    if (sourceIdx >= scannedPresets.size()) continue;

                    const auto& preset = scannedPresets[sourceIdx];

                    ImGuiMCP::PushID(static_cast<int>(sourceIdx));
                    ImGuiMCP::TableNextRow(ImGuiMCP::ImGuiTableRowFlags_None, 160.0f);

                    // Coluna 0: Imagem
                    ImGuiMCP::TableSetColumnIndex(0);
                    if (preset.textureID) {
                        ImGuiMCP::Image(preset.textureID, ImGuiMCP::ImVec2(150, 150));
                    }
                    else {
                        ImGuiMCP::Dummy(ImGuiMCP::ImVec2(150, 150));
                    }

                    // Coluna 1: Texto
                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0, 10.0f));
                    ImGuiMCP::Text("%s", preset.name.c_str());

                    // Coluna 2: Bot�o A��o
                    ImGuiMCP::TableSetColumnIndex(2);
                    ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0, 10.0f));
                    if (ImGuiMCP::Button("Apply Preset", ImGuiMCP::ImVec2(120))) {
                        std::string pPath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, preset.name).string();
                        rapidjson::Document doc;
                        if (ReadJsonObjectFromFile(pPath, doc, "UI/ApplyPresetButton")) {
                            ParseJSONToUI(doc); // Aplica JSON a Interface
                        }
                        ui_linkedPreset = preset.name;
                        SaveData();
                        ApplyNPC();

                        openPresetSelectModal = false;
                        ImGuiMCP::CloseCurrentPopup();
                    }
                    ImGuiMCP::PopID();
                }
            }
            ImGuiMCP::ImGuiListClipperManager::End(clipper);
            ImGuiMCP::EndTable();
        }

        ImGuiMCP::Separator();

        if (ImGuiMCP::Button("Cancel", ImGuiMCP::ImVec2(120, 0))) {
            openPresetSelectModal = false;
            ImGuiMCP::CloseCurrentPopup();
        }

        ImGuiMCP::EndPopup();
    }

    ImGuiMCP::Text("Basic Attributes");
    bool prevIsFemale = ui_isFemale;
    RE::TESRace* prevRace = ui_race;
    if (isLocked) ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "[LOCKED]");
    else {
        ImGuiMCP::SetNextItemWidth(200.0f);
        ImGuiMCP::InputFloat("Height", &ui_height, 0.01f, 0.1f, "%.3f");
        ImGuiMCP::SetNextItemWidth(200.0f);
        ImGuiMCP::InputFloat("Weight", &ui_weight, 1.0f, 10.0f, "%.1f");
        ImGuiMCP::ColorEdit4("Body Color (QNAM)", ui_bodyColor);
        ImGuiMCP::Checkbox("Female", &ui_isFemale);
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox("Opposite Gender Anims", &ui_oppositeGenderAnim);
    }

    static int s_raceIdx = 0, s_skinIdx = 0, s_outfitIdx = 0, s_sleepOutfitIdx = 0, s_hairColorIdx = 0, s_voiceIdx = 0;
    DrawDropdown("Race", "Race", &ui_race, s_raceIdx, isLocked);
    DrawDropdown("Voice Type", "Voice", &ui_voice, s_voiceIdx, isLocked);
    DrawDropdown("Worn Skin", "Armor", &ui_skin, s_skinIdx, isLocked);
    DrawDropdown("Default Outfit", "Outfit", &ui_outfit, s_outfitIdx, isLocked);
    DrawDropdown("Sleep Outfit", "Outfit", &ui_sleepOutfit, s_sleepOutfitIdx, isLocked);
    DrawDropdown("Hair Color", "ColorForm", &ui_hairColor, s_hairColorIdx, isLocked);
    if (prevRace != ui_race || prevIsFemale != ui_isFemale) {
        for (auto& tl : ui_tintLayers) {
            tl.name = GetSkinToneName(ui_race, ui_isFemale ? RE::SEX::kFemale : RE::SEX::kMale, tl.index);
        }
    }

    ImGuiMCP::Separator();
    ImGuiMCP::Text("Custom Face");
    if (!ui_customFaceNif.empty()) {
        std::string displayFaceName = ResolveFaceDisplayName(ui_customFaceNif);

        ImGuiMCP::TextColored({ 0.2f, 1.0f, 0.2f, 1.0f }, "Loaded Sculpt: %s", displayFaceName.c_str());
        if (ImGuiMCP::Button("Remove Sculpt")) {
            ui_customFaceNif = "";
        }
    }
    else {
        ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "No custom face loaded.");
    }

    if (!isLocked && ImGuiMCP::Button("Browse Faces")) {
        if (needFaceScan) ScanFaceGeom();
        openFaceSelectModal = true;
    }

    // --- NIF SELECT MODAL ---
    if (openFaceSelectModal) ImGuiMCP::OpenPopup("Select Face");
    if (ImGuiMCP::BeginPopupModal("Select Face", &openFaceSelectModal, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
        static char faceSearch[256] = "";
        ImGuiMCP::InputText("Search Name/FormID", faceSearch, sizeof(faceSearch));
        ImGuiMCP::Separator();

        std::string searchStr(faceSearch);
        std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), [](unsigned char c) { return std::tolower(c); });

        // 1. Filtrar previamente os �ndices baseados na busca
        std::vector<size_t> filteredIdx;
        for (size_t i = 0; i < scannedFaces.size(); i++) {
            std::string lowerName = scannedFaces[i].displayName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return std::tolower(c); });

            if (!searchStr.empty() && lowerName.find(searchStr) == std::string::npos && scannedFaces[i].nifPath.find(searchStr) == std::string::npos) {
                continue;
            }
            filteredIdx.push_back(i);
        }

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_BordersInnerH | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_ScrollY;

        // 2. Usar BeginTable e Clipper
        if (ImGuiMCP::BeginTable("FaceNIFTable", 3, tableFlags, ImGuiMCP::ImVec2(800, 600))) {
            ImGuiMCP::TableSetupColumn("Image", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 160.0f);
            ImGuiMCP::TableSetupColumn("Details", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Action", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 130.0f);

            int totalItems = static_cast<int>(filteredIdx.size());
            static auto clipper = ImGuiMCP::ImGuiListClipperManager::Create();
            ImGuiMCP::ImGuiListClipperManager::Begin(clipper, totalItems, 160.0f); // 160.0f for�a altura m�nima da linha

            while (ImGuiMCP::ImGuiListClipperManager::Step(clipper)) {
                for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
                    if (i < 0 || i >= totalItems) continue;

                    size_t sourceIdx = filteredIdx[i];
                    if (sourceIdx >= scannedFaces.size()) continue;

                    const auto& face = scannedFaces[sourceIdx];

                    ImGuiMCP::PushID(static_cast<int>(sourceIdx));
                    ImGuiMCP::TableNextRow(ImGuiMCP::ImGuiTableRowFlags_None, 160.0f);

                    // Coluna 0: Imagem
                    ImGuiMCP::TableSetColumnIndex(0);
                    if (face.textureID) {
                        ImGuiMCP::Image(face.textureID, ImGuiMCP::ImVec2(150, 150));
                    }
                    else {
                        ImGuiMCP::Dummy(ImGuiMCP::ImVec2(150, 150));
                    }

                    // Coluna 1: Textos Alinhados
                    ImGuiMCP::TableSetColumnIndex(1);
                    ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0, 10.0f)); // Desce um pouco o texto
                    ImGuiMCP::Text("%s", face.displayName.c_str());

                    // Mostra o path reduzido (sem a diretoria da engine)
                    ImGuiMCP::TextDisabled("%s", face.displayPath.c_str());

                    // Coluna 2: Bot�o de A��o
                    ImGuiMCP::TableSetColumnIndex(2);
                    ImGuiMCP::Dummy(ImGuiMCP::ImVec2(0, 10.0f)); // Desce um pouco o bot�o
                    if (ImGuiMCP::Button("Select Face", ImGuiMCP::ImVec2(120))) {
                        ui_customFaceNif = face.nifPath; // O jogo ainda usa o path original

                        RE::BGSHeadPart* targetHp = Manager::ExtractHeadPartFromNif(ui_customFaceNif);
                        if (targetHp) {
                            const auto* model = targetHp->GetModel();
                            if (targetHp->type == RE::BGSHeadPart::HeadPartType::kFace &&
                                model && model[0] != '\0') {
                                auto it = std::remove_if(ui_headParts.begin(), ui_headParts.end(), [](RE::BGSHeadPart* hp) {
                                    return hp && hp->type == RE::BGSHeadPart::HeadPartType::kFace;
                                    });
                                ui_headParts.erase(it, ui_headParts.end());
                                ui_headParts.push_back(targetHp);
                                logger::debug("[UI] Custom face selecionada com troca de Face HeadPart para '{}'.",
                                    targetHp->GetFormEditorID() ? targetHp->GetFormEditorID() : "");
                                if (!IsHeadPartValid(targetHp, ui_race, ui_isFemale)) {
                                    logger::warn("[UI] Custom face '{}' aplicou Face HeadPart incompatível por override: '{}'.",
                                        ui_customFaceNif,
                                        targetHp->GetFormEditorID() ? targetHp->GetFormEditorID() : "");
                                }
                            }
                            else {
                                logger::warn("[UI] Custom face '{}' possui Face HeadPart sem modelo valido '{}' (type={} model='{}'). Mantendo Face HeadPart atual.",
                                    ui_customFaceNif,
                                    targetHp->GetFormEditorID() ? targetHp->GetFormEditorID() : "",
                                    targetHp->type.underlying(),
                                    model ? model : "");
                            }
                        }
                        else {
                            logger::warn("[UI] Custom face selecionada, mas nenhuma Face HeadPart foi encontrada na NIF: {}", ui_customFaceNif);
                        }
                        openFaceSelectModal = false;
                        ImGuiMCP::CloseCurrentPopup();
                    }
                    ImGuiMCP::PopID();
                }
            }
            ImGuiMCP::ImGuiListClipperManager::End(clipper);
            ImGuiMCP::EndTable();
        }

        ImGuiMCP::Separator();

        if (ImGuiMCP::Button("Cancel", ImGuiMCP::ImVec2(120, 0))) {
            openFaceSelectModal = false;
            ImGuiMCP::CloseCurrentPopup();
        }

        ImGuiMCP::EndPopup();
    }

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

        static bool showIncompatibleHeadParts = false;
        ImGuiMCP::Checkbox("Show incompatible HeadParts", &showIncompatibleHeadParts);
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip("Allows selecting HeadParts outside the current race/sex filters. Use only for verified replacers or special modded parts.");
        }

        DrawDropdown(showIncompatibleHeadParts ? "Parts (All)##Add" : "Parts (Compatible)##Add",
            ui_categories[s_hpCatIdx],
            &newHp,
            s_newHpIdx,
            false,
            !showIncompatibleHeadParts,
            340.0f);
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
    EnsureMenuListsPopulated();

    ImGuiMCP::Text("Preset Manager");
    ImGuiMCP::Separator();

    ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "TIP: To create a new Preset, go to the 'NPC Editor' tab, load an NPC,\nmodify it and use the 'Save as Preset' button.");
    ImGuiMCP::Separator();

    RefreshAvailablePresets();

    static std::map<std::string, std::vector<std::string>> presetUsageDB;
    static bool needsUsageScan = true;
    if (ImGuiMCP::Button("Refresh Usage List") || needsUsageScan) {
        presetUsageDB.clear();
        for (const auto& npcJson : CollectJsonFiles(NPCPath, LegacyNPCPath)) {
            std::filesystem::directory_entry entry(npcJson);
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
        needsUsageScan = false;
    }

    auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable;

    static bool openApplyModal = false;
    static std::string presetToApply = "";
    static std::set<RE::FormID> selectedNPCs;
    static std::set<RE::FormID> originalSelectedNPCs;

    // Vari�veis para o Delete Modal
    static bool openDeleteModal = false;
    static std::string presetToDelete = "";
    static bool deleteLinkedNPCs = false;


    if (ImGuiMCP::BeginTable("PresetDB", 7, tableFlags)) {
        ImGuiMCP::TableSetupColumn("Image", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGuiMCP::TableSetupColumn("Edit", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGuiMCP::TableSetupColumn("Manage", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGuiMCP::TableSetupColumn("Export", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGuiMCP::TableSetupColumn("Delete", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGuiMCP::TableSetupColumn("Preset Name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableSetupColumn("Affected NPCs", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableHeadersRow();

        for (const auto& pName : ui_availablePresets) {
            ImGuiMCP::PushID(pName.c_str());
            ImGuiMCP::TableNextRow(ImGuiMCP::ImGuiTableRowFlags_None, 100.0f); // For�a altura da linha

            // Encontra a imagem do Preset (se existir)
            void* texID = nullptr;
            auto it = std::find_if(scannedPresets.begin(), scannedPresets.end(), [&](const CustomPresetInfo& p) { return p.name == pName; });
            if (it != scannedPresets.end()) texID = it->textureID;

            // Coluna 0 - Imagem
            ImGuiMCP::TableSetColumnIndex(0);
            if (texID) ImGuiMCP::Image(texID, ImGuiMCP::ImVec2(100, 100));
            else ImGuiMCP::Dummy(ImGuiMCP::ImVec2(100, 100));

            // Coluna 1 - Edit
            ImGuiMCP::TableSetColumnIndex(1);
            if (ImGuiMCP::Button("Edit")) {
                LoadPresetToUI(pName);
            }

            // Coluna 2 - Manage (Antigo Apply)
            ImGuiMCP::TableSetColumnIndex(2);
            if (ImGuiMCP::Button("Manage")) {
                presetToApply = pName;
                selectedNPCs.clear();
                originalSelectedNPCs.clear();

                auto manager = Manager::GetSingleton();
                const auto& npcList = manager->GetList("NPC");

                // Pr�-popula as checkboxes com os NPCs que j� utilizam este preset
                for (const auto& u : presetUsageDB[pName]) {
                    for (const auto& item : npcList) {
                        if (item.editorID == u || std::format("{:08X}", item.formID) == u) {
                            selectedNPCs.insert(item.formID);
                            originalSelectedNPCs.insert(item.formID);
                            break;
                        }
                    }
                }
                openApplyModal = true;
            }

            // Coluna 3 - Export
            ImGuiMCP::TableSetColumnIndex(3);
            if (isEditingPreset && activePresetName == pName && HasUnsavedChanges()) {
                ImGuiMCP::TextColored({ 0.5f, 0.5f, 0.5f, 1.0f }, "Save First");
                if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("This Preset is currently being edited and has unsaved changes.\nSave the data first.");
            }
            else {
                if (ImGuiMCP::Button("Export")) ExportPresetAsZip(pName);
                if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip(GetLoc("export.output_hint", "ZIP files are saved to Data/Viny Mods/NPC Visual/Export."));
            }

            // Coluna 4 - Delete
            ImGuiMCP::TableSetColumnIndex(4);
            ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, { 0.8f, 0.2f, 0.2f, 1.0f });
            if (ImGuiMCP::Button("Delete")) {
                presetToDelete = pName;
                deleteLinkedNPCs = false;
                openDeleteModal = true;
            }
            ImGuiMCP::PopStyleColor();

            // Coluna 5 - Name
            ImGuiMCP::TableSetColumnIndex(5);
            ImGuiMCP::Text("%s", pName.c_str());

            // Coluna 6 - Affected NPCs
            ImGuiMCP::TableSetColumnIndex(6);
            std::string users = "";
            for (const auto& u : presetUsageDB[pName]) users += u + ", ";
            if (!users.empty()) { users.pop_back(); users.pop_back(); }
            ImGuiMCP::TextWrapped("%s", users.empty() ? "None" : users.c_str());

            ImGuiMCP::PopID();
        }
        ImGuiMCP::EndTable();
    }

    // =====================================
        // MODAL DE GERENCIAR PRESET
        // =====================================
    if (openApplyModal) ImGuiMCP::OpenPopup("Manage Preset Links");

    if (ImGuiMCP::BeginPopupModal("Manage Preset Links", &openApplyModal, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGuiMCP::Text("Manage NPCs linked to preset: %s", presetToApply.c_str());
        ImGuiMCP::Separator();

        static char modalFilter[256] = "";
        ImGuiMCP::SetNextItemWidth(300.0f);
        ImGuiMCP::InputText("Search NPC", modalFilter, sizeof(modalFilter));

        auto manager = Manager::GetSingleton();
        const auto& npcList = manager->GetList("NPC");

        std::string search(modalFilter);
        std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return std::tolower(c); });
        std::vector<size_t> filteredIdx;
        for (size_t i = 0; i < npcList.size(); i++) {
            if (!search.empty()) {
                std::string n = npcList[i].name; std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
                std::string e = npcList[i].editorID; std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
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
        if (ImGuiMCP::Button("Uncheck All")) selectedNPCs.clear();

        ImGuiMCP::Spacing();

        std::string btnApplyText = std::format("Save Changes ({})", selectedNPCs.size());
        if (ImGuiMCP::Button(btnApplyText.c_str(), ImGuiMCP::ImVec2(180, 0))) {

            // 1. Desvincula os NPCs que foram desmarcados e restaura o estado Vanilla
            for (auto formID : originalSelectedNPCs) {
                if (!selectedNPCs.contains(formID)) {
                    Manager::GetSingleton()->UnregisterAffectedNPC(formID);
                    if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(formID)) {
                        std::string editorID = clib_util::editorID::get_editorID(npc);
                        if (editorID.empty()) editorID = std::format("{:08X}", formID);
                        std::string finalPath = ResolveJsonPath(NPCPath, LegacyNPCPath, editorID).string();
                        RemoveJsonInBothLocations(NPCPath, LegacyNPCPath, editorID);

                        // Apaga a liga��o
                        if (std::filesystem::exists(finalPath)) {
                            std::filesystem::remove(finalPath);
                        }

                        // Restaura o estado em mem�ria para Vanilla
                        if (g_vanillaNPCStates.contains(formID)) {
                            rapidjson::Document doc;
                            doc.Parse(g_vanillaNPCStates[formID].c_str());
                            Manager::ApplyNPCCustomizationFromJSON(npc, doc);
                        }
                    }
                }
            }

            // 2. Aplica o Preset aos NPCs marcados
            rapidjson::Document presetDoc;
            std::string pPath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, presetToApply).string();
            ReadJsonObjectFromFile(pPath, presetDoc, "UI/MassApplyPreset");

            std::string presetNif = "";
            if (presetDoc.HasMember("customFaceNif") && presetDoc["customFaceNif"].IsString()) {
                presetNif = presetDoc["customFaceNif"].GetString();
            }

            for (auto formID : selectedNPCs) {
                if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(formID)) {

                    Manager::GetSingleton()->RegisterAffectedNPC(formID, presetNif);
                    // Aplica em Memoria
                    if (presetDoc.IsObject()) {
                        rapidjson::Document safePresetDoc = BuildSafeCustomizationDocument(npc, presetDoc);
                        Manager::ApplyNPCCustomizationFromJSON(npc, safePresetDoc);
                    }

                    // Salva JSON no Disco
                    rapidjson::Document doc;
                    auto& allocator = doc.GetAllocator();
                    doc.SetObject();
                    doc.AddMember("preset", rapidjson::Value(presetToApply.c_str(), allocator), allocator);

                    std::string editorID = clib_util::editorID::get_editorID(npc);
                    if (editorID.empty()) editorID = std::format("{:08X}", npc->GetFormID());

                    EnsureStorageDirectories();
                    std::string finalPath = std::format("{}/{}.json", NPCPath, editorID);

                    FILE* fp = nullptr;
                    fopen_s(&fp, finalPath.c_str(), "wb");
                    if (fp) {
                        char writeBuffer[65536];
                        rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));
                        rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
                        doc.Accept(writer);
                        fclose(fp);
                        FinalizeLegacyMigrationAfterSave(NPCPath, LegacyNPCPath, editorID);
                    }
                }
            }

            needsUsageScan = true;
            selectedNPCs.clear();
            originalSelectedNPCs.clear();
            modalFilter[0] = '\0';
            openApplyModal = false;
            ImGuiMCP::CloseCurrentPopup();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Cancel", ImGuiMCP::ImVec2(120, 0))) {
            selectedNPCs.clear();
            originalSelectedNPCs.clear();
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
            std::string presetPath = ResolveJsonPath(PresetsPath, LegacyPresetsPath, presetToDelete).string();
            if (std::filesystem::exists(presetPath)) {
                std::filesystem::remove(presetPath);
            }
            RemoveJsonInBothLocations(PresetsPath, LegacyPresetsPath, presetToDelete);

            // Deleta os NPCs vinculados se o utilizador confirmou no checkbox
            if (deleteLinkedNPCs && !users.empty()) {
                for (const auto& u : users) {
                    RE::FormID formID = 0;
                    if (auto form = RE::TESForm::LookupByEditorID(u)) {
                        formID = form->GetFormID();
                    }
                    else {
                        try { formID = std::stoul(u, nullptr, 16); }
                        catch (...) {}
                    }
                    if (formID != 0) {
                        Manager::GetSingleton()->UnregisterAffectedNPC(formID);
                    }

                    std::string nPath = ResolveJsonPath(NPCPath, LegacyNPCPath, u).string();
                    if (std::filesystem::exists(nPath)) {
                        std::filesystem::remove(nPath);
                    }
                    RemoveJsonInBothLocations(NPCPath, LegacyNPCPath, u);
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
    EnsureMenuListsPopulated();

    auto manager = Manager::GetSingleton();
    const auto& npcList = manager->GetList("NPC");

    if (npcList.empty()) {
        ImGuiMCP::Text("No NPCs loaded into memory. Force a scan.");
        if (ImGuiMCP::Button("Force Scan")) manager->PopulateAllLists(true);
        return;
    }

    static std::map<std::string, std::string> affectedDB;
    static bool needScan = true;

    if (needScan) {
        affectedDB.clear();
        for (const auto& npcJson : CollectJsonFiles(NPCPath, LegacyNPCPath)) {
            std::filesystem::directory_entry entry(npcJson);
            if (entry.path().extension() == ".json") {
                std::string stem = entry.path().stem().string();
                std::string presetLinked = "Custom";

                rapidjson::Document doc;
                if (ReadJsonObjectFromFile(entry.path(), doc, "UI/NPCListScan") &&
                    doc.HasMember("preset") && doc["preset"].IsString()) {
                    presetLinked = doc["preset"].GetString();
                }
                affectedDB[stem] = presetLinked;
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
        std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return std::tolower(c); });

        for (size_t i = 0; i < npcList.size(); i++) {
            const auto& item = npcList[i];
            std::string edidHex = std::format("{:08X}", item.formID);
            bool isAffected = affectedDB.contains(item.editorID) || affectedDB.contains(edidHex);

            if (showOnlyAffected && !isAffected) continue;

            if (!search.empty()) {
                std::string n = item.name; std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
                std::string e = item.editorID; std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
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
                        // A UI n�o troca de aba automaticamente pois SKSEMenuFramework n�o suporta isso em runtime.
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
    EnsureMenuListsPopulated();


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
            g_currentNPC = nullptr; // Limpa para for�ar carregar outro
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

void NSettings::Export() {
    EnsureMenuListsPopulated();
    EnsureStorageDirectories();

    static std::set<std::string> selectedPresets;
    static std::set<std::string> selectedNPCs;
    static char packageName[128] = "NPCVisual_Export";

    const auto presetFiles = CollectJsonFiles(PresetsPath, LegacyPresetsPath);
    const auto npcFiles = CollectJsonFiles(NPCPath, LegacyNPCPath);

    ImGuiMCP::Text("%s", GetLoc("export.title", "Export Package"));
    ImGuiMCP::Separator();
    ImGuiMCP::TextWrapped("%s", GetLoc("export.description", "Select presets and NPC edits to export into one ZIP package."));
    ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, "%s", GetLoc("export.output_hint", "ZIP files are saved to Data/Viny Mods/NPC Visual/Export."));
    ImGuiMCP::Separator();

    ImGuiMCP::SetNextItemWidth(260.0f);
    ImGuiMCP::InputText("Package Name", packageName, sizeof(packageName));
    ImGuiMCP::SameLine();
    if (ImGuiMCP::Button(GetLoc("export.refresh", "Refresh Lists"))) {
        selectedPresets.clear();
        selectedNPCs.clear();
    }
    ImGuiMCP::SameLine();
    if (ImGuiMCP::Button(GetLoc("export.export_selected", "Export Selected"))) {
        ExportSelectedPackage(packageName, selectedPresets, selectedNPCs);
    }

    ImGuiMCP::Separator();

    auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable;
    if (ImGuiMCP::BeginTable("ExportSelection", 2, tableFlags)) {
        ImGuiMCP::TableSetupColumn("Presets", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableSetupColumn("NPC Edits", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
        ImGuiMCP::TableHeadersRow();
        ImGuiMCP::TableNextRow();

        ImGuiMCP::TableSetColumnIndex(0);
        ImGuiMCP::BeginChild("ExportPresetList", ImGuiMCP::ImVec2(0, 520), true);
        if (presetFiles.empty()) {
            ImGuiMCP::TextDisabled("No presets found.");
        }
        for (const auto& file : presetFiles) {
            const auto name = file.stem().string();
            bool selected = selectedPresets.contains(name);
            ImGuiMCP::PushID(("preset_" + name).c_str());
            if (ImGuiMCP::Checkbox(name.c_str(), &selected)) {
                if (selected) selectedPresets.insert(name);
                else selectedPresets.erase(name);
            }
            ImGuiMCP::PopID();
        }
        ImGuiMCP::EndChild();

        ImGuiMCP::TableSetColumnIndex(1);
        ImGuiMCP::BeginChild("ExportNPCList", ImGuiMCP::ImVec2(0, 520), true);
        if (npcFiles.empty()) {
            ImGuiMCP::TextDisabled("No NPC edits found.");
        }
        for (const auto& file : npcFiles) {
            const auto name = file.stem().string();
            bool selected = selectedNPCs.contains(name);
            ImGuiMCP::PushID(("npc_" + name).c_str());
            if (ImGuiMCP::Checkbox(name.c_str(), &selected)) {
                if (selected) selectedNPCs.insert(name);
                else selectedNPCs.erase(name);
            }
            const auto linkedPreset = ReadLinkedPresetName(file);
            if (!linkedPreset.empty()) {
                ImGuiMCP::SameLine();
                ImGuiMCP::TextColored({ 0.55f, 0.75f, 1.0f, 1.0f }, "Preset: %s", linkedPreset.c_str());
            }
            ImGuiMCP::PopID();
        }
        ImGuiMCP::EndChild();

        ImGuiMCP::EndTable();
    }

    ImGuiMCP::Text("Selected: %d preset(s), %d NPC edit(s)", static_cast<int>(selectedPresets.size()), static_cast<int>(selectedNPCs.size()));
}

void NSettings::Debug()
{
    auto* manager = Manager::GetSingleton();

    ImGuiMCP::Text("%s", GetLoc("debug.title", "Debug Tools"));
    ImGuiMCP::Separator();
    ImGuiMCP::TextWrapped("%s", GetLoc("debug.reload_data_hint", "Refreshes the internal form database. Use this after dynamic form mods have injected or updated forms."));
    ImGuiMCP::Text("Form database populated: %s", manager && manager->_isPopulated ? "yes" : "no");
    ImGuiMCP::Separator();

    if (ImGuiMCP::Button(GetLoc("debug.reload_data", "Reload Data"))) {
        if (manager) {
            logger::debug("[DebugMenu] Reload Data clicked: forcing PopulateAllLists after dynamic form update.");
            manager->_isPopulated = false;
            logger::debug("[DebugMenu] PopulateAllLists BEGIN reason=manual_reload_data");
            manager->PopulateAllLists(true);
            logger::debug("[DebugMenu] PopulateAllLists END reason=manual_reload_data");
        }
        else {
            logger::debug("[DebugMenu] Reload Data clicked but manager is null.");
        }
    }
}



void NSettings::MmRegister() {
    logger::debug("[MainMenuBoot] MmRegister entered SKSEMenuFrameworkInstalled={}", SKSEMenuFramework::IsInstalled());
    if (SKSEMenuFramework::IsInstalled()) {
        logger::debug("[MainMenuBoot] EnsureStorageDirectories BEGIN");
        EnsureStorageDirectories();
        logger::debug("[MainMenuBoot] EnsureStorageDirectories END");
        logger::debug("[MainMenuBoot] LoadLanguage BEGIN");
        LoadLanguage();
        logger::debug("[MainMenuBoot] LoadLanguage END");
        logger::debug("[MainMenuBoot] SetSection BEGIN");
        SKSEMenuFramework::SetSection("NPC Visual Editor");
        logger::debug("[MainMenuBoot] SetSection END");
        logger::debug("[MainMenuBoot] AddSectionItem Editor BEGIN");
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.editor", "Editor"), NPCMenu);
        logger::debug("[MainMenuBoot] AddSectionItem Editor END");
        logger::debug("[MainMenuBoot] AddSectionItem Presets BEGIN");
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.presets", "Presets"), Presets);
        logger::debug("[MainMenuBoot] AddSectionItem Presets END");
        logger::debug("[MainMenuBoot] AddSectionItem Database BEGIN");
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.database", "Database"), NPCList);
        logger::debug("[MainMenuBoot] AddSectionItem Database END");
        logger::debug("[MainMenuBoot] AddSectionItem Export BEGIN");
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.export", "Export"), Export);
        logger::debug("[MainMenuBoot] AddSectionItem Export END");
        logger::debug("[MainMenuBoot] AddSectionItem Debug BEGIN");
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.debug", "Debug"), Debug);
        logger::debug("[MainMenuBoot] AddSectionItem Debug END");
        logger::debug("[MmRegister] Menu sections registered successfully.");
    }
    logger::debug("[MainMenuBoot] MmRegister exit");
}

void NSettings::Load() {
    logger::debug("[LoadSavedData] BEGIN");
    logger::debug("[LoadSavedData] Inicializando sistema de arquivos...");
    EnsureStorageDirectories();
    LoadLanguage();
    logger::debug("[LoadSavedData] ClearAffectedNPCs BEGIN");
    Manager::GetSingleton()->ClearAffectedNPCs();
    logger::debug("[LoadSavedData] ClearAffectedNPCs END");
    refreshListsOnNextMenuOpen = true;

    int countPresetsCarregados = 0;
    int countNPCsModificados = 0;

    std::map<std::string, rapidjson::Document> presetCache;

    const auto presetFiles = CollectJsonFiles(PresetsPath, LegacyPresetsPath);
    const auto npcFiles = CollectJsonFiles(NPCPath, LegacyNPCPath);

    logger::debug("[LoadSavedData] Found preset JSONs count={} primary='{}' legacy='{}'",
        presetFiles.size(),
        PresetsPath,
        LegacyPresetsPath);
    logger::debug("[LoadSavedData] Found NPC JSONs count={} primary='{}' legacy='{}'",
        npcFiles.size(),
        NPCPath,
        LegacyNPCPath);

    logger::debug("[LoadSavedData] Passo 1: Lendo arquivos na pasta de Presets...");
    for (const auto& presetJson : presetFiles) {
        std::filesystem::directory_entry entry(presetJson);
        if (entry.path().extension() != ".json") {
            continue;
        }

        std::string presetName = entry.path().stem().string();
        std::error_code fileEc;
        const auto fileSize = std::filesystem::file_size(entry.path(), fileEc);
        logger::debug("[LoadSavedData] Preset JSON begin name='{}' path='{}' size={} legacy={}",
            presetName,
            entry.path().string(),
            fileEc ? 0 : fileSize,
            entry.path().string().find(LegacyPresetsPath) == 0);

        rapidjson::Document doc;
        if (!ReadJsonObjectFromFile(entry.path(), doc, "Load/Preset")) {
            logger::warn("[LoadSavedData] Preset ignorado por JSON invalido: {}", presetName);
            continue;
        }

        presetCache[presetName] = std::move(doc);
        countPresetsCarregados++;
        logger::debug("[LoadSavedData] Preset cacheado com sucesso: {}", presetName);
    }

    logger::debug("[LoadSavedData] Passo 2: Lendo arquivos na pasta de NPCs...");
    for (const auto& npcJson : npcFiles) {
        std::filesystem::directory_entry entry(npcJson);
        if (entry.path().extension() != ".json") {
            continue;
        }

        std::string filename = entry.path().stem().string();
        std::error_code fileEc;
        const auto fileSize = std::filesystem::file_size(entry.path(), fileEc);
        logger::debug("[LoadSavedData] NPC JSON begin file='{}' path='{}' size={} legacy={}",
            filename,
            entry.path().string(),
            fileEc ? 0 : fileSize,
            entry.path().string().find(LegacyNPCPath) == 0);

        RE::TESNPC* targetNPC = nullptr;
        const char* resolveMode = "none";

        if (auto edidForm = RE::TESForm::LookupByEditorID(filename)) {
            targetNPC = edidForm->As<RE::TESNPC>();
            resolveMode = targetNPC ? "editorID" : "editorID-wrong-type";
        }
        else {
            try {
                RE::FormID id = std::stoul(filename, nullptr, 16);
                if (auto idForm = RE::TESForm::LookupByID(id)) {
                    targetNPC = idForm->As<RE::TESNPC>();
                    resolveMode = targetNPC ? "formID" : "formID-wrong-type";
                }
            }
            catch (...) {
                logger::warn("[LoadSavedData] Arquivo '{}' tem nome invalido (Nao eh EditorID nem Hex FormID).", filename);
            }
        }

        if (!targetNPC) {
            logger::error("[LoadSavedData] Falha! NPC base '{}' nao encontrado na memoria do jogo resolveMode={}.", filename, resolveMode);
            continue;
        }

        logger::debug("[LoadSavedData] NPC '{}' resolvido para FormID {:08X} mode={}. Lendo JSON...",
            filename,
            targetNPC->GetFormID(),
            resolveMode);

        rapidjson::Document doc;
        if (!ReadJsonObjectFromFile(entry.path(), doc, "Load/NPC")) {
            logger::warn("[LoadSavedData] NPC '{}' ignorado por JSON invalido.", filename);
            continue;
        }

        std::string nifPath = "";

        // Verifica se este NPC usa um Preset linkado.
        if (doc.HasMember("preset") && doc["preset"].IsString()) {
            std::string presetName = doc["preset"].GetString();
            logger::debug("[LoadSavedData] NPC {:08X} vinculado ao Preset '{}'.", targetNPC->GetFormID(), presetName);

            auto presetIt = presetCache.find(presetName);
            if (presetIt != presetCache.end()) {
                logger::debug("[LoadSavedData] BuildSafeCustomizationDocument preset BEGIN preset='{}' npc={:08X}", presetName, targetNPC->GetFormID());
                rapidjson::Document safePresetDoc = BuildSafeCustomizationDocument(targetNPC, presetIt->second);
                logger::debug("[LoadSavedData] BuildSafeCustomizationDocument preset END preset='{}' npc={:08X}", presetName, targetNPC->GetFormID());

                logger::debug("[LoadSavedData] BEGIN Apply preset '{}' em NPC {:08X}.", presetName, targetNPC->GetFormID());
                Manager::ApplyNPCCustomizationFromJSON(targetNPC, safePresetDoc);
                logger::debug("[LoadSavedData] END Apply preset '{}' em NPC {:08X}.", presetName, targetNPC->GetFormID());

                if (safePresetDoc.HasMember("customFaceNif") && safePresetDoc["customFaceNif"].IsString()) {
                    nifPath = safePresetDoc["customFaceNif"].GetString();
                }

                if (!nifPath.empty()) {
                    logger::debug("[LoadSavedData] RegisterAffectedNPC NPC {:08X} nif='{}'", targetNPC->GetFormID(), nifPath);
                    Manager::GetSingleton()->RegisterAffectedNPC(targetNPC->GetFormID(), nifPath);
                    logger::debug("[LoadSavedData] RegisterAffectedNPC OK NPC {:08X}.", targetNPC->GetFormID());
                }
                else {
                    Manager::GetSingleton()->UnregisterAffectedNPC(targetNPC->GetFormID());
                    logger::debug("[LoadSavedData] Preset sem customFaceNif; NPC {:08X} nao registrado como affected face.", targetNPC->GetFormID());
                }
                countNPCsModificados++;
            }
            else {
                logger::error("[LoadSavedData] ABORTANDO: NPC {} aponta para preset '{}' que NAO FOI ENCONTRADO no cache.", filename, presetName);
            }
        }
        else {
            logger::debug("[LoadSavedData] BuildSafeCustomizationDocument JSON unico BEGIN npc={:08X}", targetNPC->GetFormID());
            rapidjson::Document safeDoc = BuildSafeCustomizationDocument(targetNPC, doc);
            logger::debug("[LoadSavedData] BuildSafeCustomizationDocument JSON unico END npc={:08X}", targetNPC->GetFormID());

            logger::debug("[LoadSavedData] Aplicando JSON customizado unico para NPC {:08X}.", targetNPC->GetFormID());
            logger::debug("[LoadSavedData] BEGIN Apply JSON unico em NPC {:08X}.", targetNPC->GetFormID());
            Manager::ApplyNPCCustomizationFromJSON(targetNPC, safeDoc);
            logger::debug("[LoadSavedData] END Apply JSON unico em NPC {:08X}.", targetNPC->GetFormID());

            if (safeDoc.HasMember("customFaceNif") && safeDoc["customFaceNif"].IsString()) {
                nifPath = safeDoc["customFaceNif"].GetString();
            }

            if (!nifPath.empty()) {
                logger::debug("[LoadSavedData] RegisterAffectedNPC NPC {:08X} nif='{}'", targetNPC->GetFormID(), nifPath);
                Manager::GetSingleton()->RegisterAffectedNPC(targetNPC->GetFormID(), nifPath);
                logger::debug("[LoadSavedData] RegisterAffectedNPC OK NPC {:08X}.", targetNPC->GetFormID());
            }
            else {
                Manager::GetSingleton()->UnregisterAffectedNPC(targetNPC->GetFormID());
                logger::debug("[LoadSavedData] JSON sem customFaceNif; NPC {:08X} nao registrado como affected face.", targetNPC->GetFormID());
            }
            countNPCsModificados++;
        }
    }

    logger::debug("[LoadSavedData] ScanFaceGeom BEGIN");
    ScanFaceGeom();
    logger::debug("[LoadSavedData] ScanFaceGeom END");

    logger::debug("[LoadSavedData] END presets={} npcs={}", countPresetsCarregados, countNPCsModificados);
    logger::info("[NPC Replacer] BOOT CONCLUIDO: {} presets em cache, {} NPCs modificados com sucesso.", countPresetsCarregados, countNPCsModificados);
}
