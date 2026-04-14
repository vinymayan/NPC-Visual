#include "Hooks.h"
#include "Manager.h"
#include "BSFaceGenBaseMorphExtraData.h"


// ==============================================================================
// FUNÇÃO AUXILIAR PARA RASTREIO DE FMD
// ==============================================================================
void LogAllFMDs(RE::NiAVObject* root, RE::FormID actorID, const char* callerContext) {
    if (!root) return;

    auto netObj = static_cast<RE::NiObjectNET*>(root);
    if (auto extra = netObj->GetExtraData("FMD")) {
        if (auto fmdExtra = static_cast<RE::BSFaceGenModelExtraData*>(extra)) {
            std::string nodeName = root->name.c_str() ? root->name.c_str() : "SemNome";
            logger::info("[{}] [Ator {:08X}] FMD encontrado na malha/node '{}' | m_model: {}",
                callerContext, actorID, nodeName, (void*)fmdExtra->m_model);
        }
    }

    // Busca recursivamente nos filhos
    if (auto node = root->AsNode()) {
        for (auto& child : node->GetChildren()) {
            if (child) {
                LogAllFMDs(child.get(), actorID, callerContext);
            }
        }
    }
}

// ==============================================================================
// SISTEMA DE AGENDAMENTO (POLLING PARA 3D CARREGADO)
// ==============================================================================
namespace TaskScheduler {

    static void WaitFor3DAndApplyLoad(RE::FormID refID, int retries) {
        if (retries <= 0) {
            logger::warn("[TaskScheduler] Esgotaram as tentativas para carregar o 3D do FormID {:08X}.", refID);
            return;
        }

        std::thread([refID, retries]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            SKSE::GetTaskInterface()->AddTask([refID, retries]() {
                auto ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refID);
                if (!ref) {
                    logger::warn("[TaskScheduler] Referencia {:08X} deixou de existir durante a espera.", refID);
                    return;
                }

                auto node3D = ref->Get3D();
                if (node3D) {
                    logger::debug("[TaskScheduler] 3D carregado com sucesso para {:08X}! Inspecionando FMDs...", refID);


                   // LogAllFMDs(node3D, refID, "TaskScheduler_Wait");

                    if (ref->GetFormType() == RE::FormType::ActorCharacter || ref->GetFormType() == RE::FormType::NPC) {
                        if (auto actor = ref->As<RE::Actor>()) {
                            if (auto base = actor->GetActorBase()) {
                                std::string nifPath;
                                if (Manager::GetSingleton()->IsNPCAffected(base->GetFormID(), nifPath)) {
                                    Manager::ScheduleFaceDeform(actor->GetFormID(), nifPath, 20);
                                }
                            }
                        }
                    }
                }
                else {
                    logger::debug("[TaskScheduler] 3D ainda ausente para {:08X}. Tentativas restantes: {}", refID, retries - 1);
                    WaitFor3DAndApplyLoad(refID, retries - 1);
                }
                });
            }).detach();
    }

}

RE::NiAVObject* Load3DHook::Hook_Load3D_REFR(RE::TESObjectREFR* a_this, bool a_backgroundLoading) {
    auto result3D = _Load3D_REFR(a_this, a_backgroundLoading);
    ProcessPrismaLoad3D(a_this, result3D);
    return result3D;
}

RE::NiAVObject* Load3DHook::Hook_Load3D_Char(RE::Character* a_this, bool a_backgroundLoading) {
    auto result3D = _Load3D_Char(a_this, a_backgroundLoading);
    ProcessPrismaLoad3D(a_this, result3D);
    return result3D;
}

RE::NiAVObject* Load3DHook::Hook_Load3D_Player(RE::PlayerCharacter* a_this, bool a_backgroundLoading) {
    auto result3D = _Load3D_Player(a_this, a_backgroundLoading);
    ProcessPrismaLoad3D(a_this, result3D);
    return result3D;
}

// Lógica unificada para qualquer objeto que termine de carregar o 3D
void Load3DHook::ProcessPrismaLoad3D(RE::TESObjectREFR* a_this, RE::NiAVObject* result3D) {
    if (result3D && a_this) {
        bool isActor = (a_this->GetFormType() == RE::FormType::ActorCharacter || a_this->GetFormType() == RE::FormType::NPC);
        const char* typeStr = isActor ? "ATOR " : "";

        if (isActor) {
            if (a_this->Is3DLoaded()) {
                //logger::info("[LOAD3D] Malha 3D carregada para {}RefID: {:08X}. Inspecionando FMDs...", typeStr, a_this->GetFormID());

                // --- INJEÇÃO DO LOG DE RASTREIO DIRETO DO RESULT3D ---
                //LogAllFMDs(result3D, a_this->GetFormID(), "ProcessPrismaLoad3D_Imediato");

                if (auto actor = a_this->As<RE::Actor>()) {
                    // VERIFICA O BANCO DE DADOS EM MEMÓRIA
                    if (auto base = actor->GetActorBase()) {
                        std::string nifPath;
                        if (Manager::GetSingleton()->IsNPCAffected(base->GetFormID(), nifPath)) {
                            //logger::info("[LOAD3D] {}RefID: {:08X} e afetado por um NIF custom. Agendando deformacao facial...", typeStr, a_this->GetFormID());
                            Manager::ScheduleFaceDeform(actor->GetFormID(), nifPath, 20);
                        }
                    }
                }
            }
            else {
                logger::debug("[LOAD3D] Load3D foi chamado para {}RefID: {:08X}, mas o 3D ainda nao esta marcado como carregado. Agendando...", typeStr, a_this->GetFormID());
                TaskScheduler::WaitFor3DAndApplyLoad(a_this->GetFormID(), 20);
            }
        }
    }
}