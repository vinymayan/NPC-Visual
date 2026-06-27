#include "logger.h"
#include "Settings.h"
#include "Manager.h"
#include "Hooks.h"
#include "DelayedDispatcher.h"

#include <chrono>
#include <string>

namespace {
    void RefreshActorByFormID(RE::FormID actorID, std::string nifPath, const std::string& reason);

    void RefreshLoadedAffectedActors(const std::string& reason)
    {
        auto* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) {
            logger::debug("[RuntimeRefresh] ProcessLists ausente reason={}", reason);
            return;
        }

        std::uint32_t refreshed = 0;
        for (auto& actorHandle : processLists->highActorHandles) {
            auto ref = actorHandle.get();
            auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
            auto* base = actor ? actor->GetActorBase() : nullptr;
            if (!actor || !base || actor->IsPlayerRef()) {
                continue;
            }

            std::string nifPath;
            if (!Manager::GetSingleton()->IsNPCAffected(base->GetFormID(), nifPath) || nifPath.empty()) {
                continue;
            }

            RefreshActorByFormID(actor->GetFormID(), nifPath, reason);
            ++refreshed;
        }

        logger::info("[RuntimeRefresh] Loaded actors refreshed reason={} count={}", reason, refreshed);
    }

    void RefreshActorByFormID(RE::FormID actorID, std::string nifPath, const std::string& reason)
    {
        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(actorID);
        auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
        if (!actor || !actor->GetActorBase() || actor->IsPlayerRef()) {
            return;
        }

        logger::debug("[RuntimeRefresh] Refresh loaded actor ref={:08X} base={:08X} reason={} nif='{}'",
            actor->GetFormID(),
            actor->GetActorBase()->GetFormID(),
            reason,
            nifPath);
        actor->UpdateHairColor();
        actor->UpdateSkinColor();
        actor->DoReset3D(true);
        Manager::ScheduleFaceDeform(actorID, nifPath, 24);
    }

    void QueueRuntimeRefresh(const char* reason, std::uint32_t delayMs, bool loadSavedData)
    {
        Utils::DelayedDispatcher::Get().PostDelayed(std::chrono::milliseconds(delayMs), [reason = std::string(reason), delayMs, loadSavedData]() {
            auto* taskInterface = SKSE::GetTaskInterface();
            if (!taskInterface) {
                logger::warn("[RuntimeRefresh] TaskInterface ausente; executando direto reason={} delayMs={}", reason, delayMs);
                Manager::GetSingleton()->PopulateAllLists(true);
                if (loadSavedData) {
                    NSettings::Load();
                }
                RefreshLoadedAffectedActors(reason);
                return;
            }

            taskInterface->AddTask([reason, delayMs, loadSavedData]() {
                logger::info("[RuntimeRefresh] BEGIN reason={} delayMs={} loadSavedData={}", reason, delayMs, loadSavedData);
                Manager::GetSingleton()->PopulateAllLists(true);
                if (loadSavedData) {
                    NSettings::Load();
                }
                RefreshLoadedAffectedActors(reason);
                logger::info("[RuntimeRefresh] END reason={} delayMs={}", reason, delayMs);
            });
        });
    }
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        SKSE::AllocTrampoline(64);
        NSettings::MmRegister();
        Load3DHook::Install();
        QueueRuntimeRefresh("DataLoaded", 0, true);
    }
    if (message->type == SKSE::MessagingInterface::kNewGame || message->type == SKSE::MessagingInterface::kPostLoadGame) {
        QueueRuntimeRefresh(message->type == SKSE::MessagingInterface::kNewGame ? "NewGame" : "PostLoadGame", 0, false);
    }
}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {

    SetupLog();
    logger::info("Plugin loaded");
    SKSE::Init(skse);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
