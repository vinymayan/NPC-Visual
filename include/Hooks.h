class Load3DHook {
public:
    static void Install() {
        // 1. Hook para Objetos normais (armas, armaduras no chao, baus, etc)
        REL::Relocation<std::uintptr_t> vtableREFR{ RE::TESObjectREFR::VTABLE[0] };
        _Load3D_REFR = vtableREFR.write_vfunc(0x6A, &Hook_Load3D_REFR);

        // 2. Hook para NPCs / Criaturas (Atores)
        REL::Relocation<std::uintptr_t> vtableChar{ RE::Character::VTABLE[0] };
        _Load3D_Char = vtableChar.write_vfunc(0x6A, &Hook_Load3D_Char);

        // 3. Hook para o Player (Opcional, mas normalmente necessario)
        REL::Relocation<std::uintptr_t> vtablePlayer{ RE::PlayerCharacter::VTABLE[0] };
        _Load3D_Player = vtablePlayer.write_vfunc(0x6A, &Hook_Load3D_Player);

        SKSE::log::info("Hook de Load3D instalado no índice 0x6A");
    }

private:
    static RE::NiAVObject* Hook_Load3D_REFR(RE::TESObjectREFR* a_this, bool a_backgroundLoading);
    static RE::NiAVObject* Hook_Load3D_Char(RE::Character* a_this, bool a_backgroundLoading);
    static RE::NiAVObject* Hook_Load3D_Player(RE::PlayerCharacter* a_this, bool a_backgroundLoading);

    // Funcao centralizada com a nossa logica do Prisma
    static void ProcessPrismaLoad3D(RE::TESObjectREFR* a_this, RE::NiAVObject* result3D);

    // Ponteiros Originais
    static inline REL::Relocation<decltype(&Hook_Load3D_REFR)> _Load3D_REFR;
    static inline REL::Relocation<decltype(&Hook_Load3D_Char)> _Load3D_Char;
    static inline REL::Relocation<decltype(&Hook_Load3D_Player)> _Load3D_Player;
};
