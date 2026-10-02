// Mod de PARRY con BARRA DE POSTURA (estilo Sekiro) para Dusklight
//
// - Empujas el escudo (procGuardAttackInit) -> ventana corta de parry.
// - Cada parry NO aturde al enemigo: sigue atacando. Sube su barra blanca.
// - Con PARRIES_TO_STUN parries seguidos la barra se llena y el enemigo queda aturdido.
// - Aturdido: B = tajo relampago (Mortal Draw), B otra vez = segundo tajo.
// - Tras los tajos (o si se acaba el tiempo) la barra se reinicia.
// - Sin escudo automatico al fijar. Alternar: mantener R + presionar Z.

#define ENABLE_BAR 1   // 1 = dibuja la barra sobre el enemigo, 0 = sin barra (solo sonidos)

#include <unordered_map>
#include <vector>
#include <cstdint>

#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_controller_pad.h"
#include "SSystem/SComponent/c_math.h"

#if ENABLE_BAR
#include "d/d_drawlist.h"
#include "m_Do/m_Do_lib.h"
#include <dolphin/gx.h>
#endif

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

DEFINE_HOOK(&daAlink_c::execute, LinkExecute);
DEFINE_HOOK(&daAlink_c::procGuardAttackInit, GuardAttackInit);
DEFINE_HOOK(&daAlink_c::setGuardSe, GuardSe);
DEFINE_HOOK(&daAlink_c::procGuardSlipInit, GuardSlipInit);
DEFINE_HOOK(&daAlink_c::procGuardBreakInit, GuardBreakInit);
DEFINE_HOOK(&daAlink_c::setSmallGuard, SmallGuard);
DEFINE_HOOK(&daAlink_c::setShieldGuard, ShieldGuard);
#if ENABLE_BAR
DEFINE_HOOK(&daAlink_c::draw, LinkDraw);
#endif

// ---- Ajustes (ticks de logica: 30 por segundo) ----
static const int PARRY_WINDOW_TICKS = 5;     // ventana tras empujar el escudo
static const int PARRIES_TO_STUN = 3;        // parries para llenar la barra
static const int STUN_TICKS = 120;           // tiempo aturdido para empezar los tajos (4 s)
static const int SECOND_SLASH_TICKS = 30;    // tiempo para el segundo tajo
static const int HOLD_AFTER_TICKS = 20;      // enemigo quieto mientras cae el segundo tajo

static const float BAR_HEIGHT_ABOVE_HEAD = 60.0f;  // altura de la barra sobre la cabeza
static const float BAR_WIDTH = 60.0f;              // ancho en pixeles
static const float BAR_THICKNESS = 6.0f;           // alto en pixeles

// Valores del enum de tajos finales (Mortal Draw A y B)
static const int MORTAL_DRAW_A = 3;
static const int MORTAL_DRAW_B = 4;

// ---- Estado ----
struct EnemyState {
    int parries = 0;        // parries acumulados (llenado de la barra)
    int stunTimer = 0;      // aturdido: esperando el primer tajo
    int secondTimer = 0;    // ventana del segundo tajo
    int holdTimer = 0;      // quieto mientras cae el segundo tajo
    bool lastWasA = false;
};

static std::unordered_map<uint32_t, EnemyState> g_enemies;

static bool g_noAutoShield = true;   // true = sin escudo automatico al fijar
static int g_parryTimer = 0;
static bool g_parryHitThisTick = false;

static fopAc_ac_c* actor_by_id(uint32_t id) {
    fopAc_ac_c* a = nullptr;
    fopAcM_SearchByID((fpc_ProcID)id, &a);
    return a;
}

static void on_bash_post(ModContext*, void*, void*, void*) {
    g_parryTimer = PARRY_WINDOW_TICKS;
}

// Un golpe pego en el escudo.
static HookAction on_guard_se_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    if (g_parryTimer <= 0) return HOOK_CONTINUE;

    if (link->mProcID == daAlink_c::PROC_SIDESTEP ||
        link->mProcID == daAlink_c::PROC_FRONT_ROLL ||
        link->mProcID == daAlink_c::PROC_SIDE_ROLL ||
        link->mProcID == daAlink_c::PROC_BACK_JUMP) {
        return HOOK_CONTINUE;
    }

    // PARRY!
    g_parryTimer = 0;
    g_parryHitThisTick = true;

    bool stunnedNow = false;
    fopAc_ac_c* target = link->mTargetedActor;
    if (target) {
        EnemyState& st = g_enemies[fopAcM_GetID(target)];
        bool exposed = st.stunTimer > 0 || st.secondTimer > 0 || st.holdTimer > 0;
        if (!exposed) {
            st.parries++;
            if (st.parries >= PARRIES_TO_STUN) {
                st.parries = PARRIES_TO_STUN;
                st.stunTimer = STUN_TICKS;
                stunnedNow = true;
            }
        }
    }

    link->setPlayerSe(stunnedNow ? Z2SE_TITLE_ENTER : Z2SE_MIDNA_JUMP);
    dComIfGp_getVibration().StartShock(VIBMODE_S_POWER4, 1, cXyz(0.0f, 1.0f, 0.0f));

    svc_log->info(mod_ctx, stunnedNow ? "PARRY: enemigo aturdido" : "PARRY");
    return HOOK_CONTINUE;
}

// En un parry cancelamos el retroceso / la guardia rota.
static HookAction skip_if_parry(ModContext*, void*, void* retval, void*) {
    if (!g_parryHitThisTick) return HOOK_CONTINUE;
    if (retval != nullptr) *static_cast<int*>(retval) = 0;
    return HOOK_SKIP_ORIGINAL;
}

// Quita el escudo automatico al fijar enemigos (la estructura derivada permite
// usar los miembros internos de Link).
struct GuardHelper : daAlink_c {
    static void remove_auto_guard(daAlink_c* l) {
        GuardHelper* h = static_cast<GuardHelper*>(l);
        if (h->checkAttentionLock() &&
            h->mProcID != PROC_GUARD_SLIP &&
            !h->checkSmallUpperGuardAnime()) {
            h->offNoResetFlg2(FLG2_UNK_8000000);
        }
    }
};

static void on_shield_guard_post(ModContext*, void* args, void*, void*) {
    if (!g_noAutoShield) return;
    GuardHelper::remove_auto_guard(mods::arg<daAlink_c*>(args, 0));
}

// Cada tick de Link.
static HookAction on_execute_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    // Alternar escudo automatico: mantener R + presionar Z
    if (mDoCPd_c::getHoldR(PAD_1) && mDoCPd_c::getTrigZ(PAD_1)) {
        g_noAutoShield = !g_noAutoShield;
        link->setPlayerSe(g_noAutoShield ? Z2SE_MIDNA_JUMP : Z2SE_AL_ITEM_TAKEOUT);
        svc_log->info(mod_ctx, g_noAutoShield ? "escudo automatico: QUITADO" : "escudo automatico: NORMAL");
    }

    if (g_parryTimer > 0) g_parryTimer--;

    // Actualizar enemigos: limpiar muertos, mantener quietos a los aturdidos.
    for (auto it = g_enemies.begin(); it != g_enemies.end();) {
        fopAc_ac_c* a = actor_by_id(it->first);
        if (!a) {
            it = g_enemies.erase(it);
            continue;
        }
        EnemyState& st = it->second;

        if (st.stunTimer > 0) {
            a->speedF = 0.0f;
            if (--st.stunTimer == 0) st.parries = 0;       // se acabo el tiempo: reinicia
        } else if (st.secondTimer > 0) {
            a->speedF = 0.0f;
            if (--st.secondTimer == 0) st.parries = 0;     // no hubo segundo tajo: reinicia
        } else if (st.holdTimer > 0) {
            a->speedF = 0.0f;
            st.holdTimer--;
        }
        ++it;
    }

    // Tajos relampago sobre el enemigo fijado, si esta aturdido.
    fopAc_ac_c* target = link->mTargetedActor;
    if (target && mDoCPd_c::getTrigB(PAD_1)) {
        auto it = g_enemies.find(fopAcM_GetID(target));
        if (it != g_enemies.end()) {
            EnemyState& st = it->second;
            if (st.stunTimer > 0) {
                st.lastWasA = cM_rndF(1.0f) < 0.5f;
                link->procCutFinishInit(st.lastWasA ? MORTAL_DRAW_A : MORTAL_DRAW_B);
                st.stunTimer = 0;
                st.secondTimer = SECOND_SLASH_TICKS;
            } else if (st.secondTimer > 0) {
                link->procCutFinishInit(st.lastWasA ? MORTAL_DRAW_B : MORTAL_DRAW_A);
                st.secondTimer = 0;
                st.holdTimer = HOLD_AFTER_TICKS;
                st.parries = 0;                            // barra reiniciada
            }
        }
    }
    return HOOK_CONTINUE;
}

static void on_execute_post(ModContext*, void*, void*, void*) {
    g_parryHitThisTick = false;
}

// ======================= BARRA SOBRE EL ENEMIGO =======================
#if ENABLE_BAR

struct BarInfo {
    float x, y;      // posicion en pantalla (centro de la barra)
    float ratio;     // 0..1
};
static std::vector<BarInfo> g_bars;

static void draw_rect(float x0, float y0, float x1, float y1, u8 r, u8 g, u8 b, u8 a) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x0, y0, 0.0f); GXColor4u8(r, g, b, a);
    GXPosition3f32(x1, y0, 0.0f); GXColor4u8(r, g, b, a);
    GXPosition3f32(x1, y1, 0.0f); GXColor4u8(r, g, b, a);
    GXPosition3f32(x0, y1, 0.0f); GXColor4u8(r, g, b, a);
    GXEnd();
}

class ParryBarDraw : public dDlst_base_c {
public:
    virtual void draw() {
        if (g_bars.empty()) return;

        Mtx44 proj;
        C_MTXOrtho(proj, 0.0f, 448.0f, 0.0f, 608.0f, 0.0f, 10.0f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx ident;
        PSMTXIdentity(ident);
        GXLoadPosMtxImm(ident, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);

        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(0);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
        GXSetCullMode(GX_CULL_NONE);

        for (const BarInfo& bar : g_bars) {
            float half = BAR_WIDTH * 0.5f;
            float left = bar.x - half;
            // marco oscuro
            draw_rect(left - 2.0f, bar.y - 2.0f, left + BAR_WIDTH + 2.0f, bar.y + BAR_THICKNESS + 2.0f,
                      0, 0, 0, 170);
            // relleno blanco
            if (bar.ratio > 0.0f) {
                draw_rect(left, bar.y, left + BAR_WIDTH * bar.ratio, bar.y + BAR_THICKNESS,
                          255, 255, 255, 255);
            }
        }
    }
};

static ParryBarDraw g_barDraw;

// Despues de dibujar a Link: calcula donde va cada barra y pide dibujarla.
static void on_link_draw_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    fopAc_ac_c* target = link->mTargetedActor;

    g_bars.clear();

    auto add_bar = [&](fopAc_ac_c* a, const EnemyState* st) {
        cXyz p(a->eyePos.x, a->eyePos.y + BAR_HEIGHT_ABOVE_HEAD, a->eyePos.z);
        Vec s;
        mDoLib_project(&p, &s);
        if (s.x < 0.0f || s.x > 608.0f || s.y < 0.0f || s.y > 448.0f) return;

        float ratio = 0.0f;
        if (st) {
            bool exposed = st->stunTimer > 0 || st->secondTimer > 0 || st->holdTimer > 0;
            ratio = exposed ? 1.0f : (float)st->parries / (float)PARRIES_TO_STUN;
        }
        g_bars.push_back({s.x, s.y, ratio});
    };

    // Enemigos con barra en juego
    for (auto& kv : g_enemies) {
        fopAc_ac_c* a = actor_by_id(kv.first);
        if (!a) continue;
        const EnemyState& st = kv.second;
        bool show = st.parries > 0 || st.stunTimer > 0 || st.secondTimer > 0 || st.holdTimer > 0;
        if (show || a == target) add_bar(a, &st);
    }
    // El fijado siempre muestra su barra (vacia si aun no tiene estado)
    if (target && g_enemies.find(fopAcM_GetID(target)) == g_enemies.end()) {
        add_bar(target, nullptr);
    }

    if (!g_bars.empty()) {
        dComIfGd_set2DXlu(&g_barDraw);
    }
}

#endif  // ENABLE_BAR

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r;
    if ((r = mods::hook::add_post<GuardAttackInit>(on_bash_post)) != MOD_OK)
        return mods::set_error(error, r, "hook empuje de escudo");
    if ((r = mods::hook::add_pre<GuardSe>(on_guard_se_pre)) != MOD_OK)
        return mods::set_error(error, r, "hook golpe en escudo");
    if ((r = mods::hook::add_pre<GuardSlipInit>(skip_if_parry)) != MOD_OK)
        return mods::set_error(error, r, "hook retroceso");
    if ((r = mods::hook::add_pre<GuardBreakInit>(skip_if_parry)) != MOD_OK)
        return mods::set_error(error, r, "hook guardia rota");
    if ((r = mods::hook::add_pre<SmallGuard>(skip_if_parry)) != MOD_OK)
        return mods::set_error(error, r, "hook guardia chica");
    if ((r = mods::hook::add_post<ShieldGuard>(on_shield_guard_post)) != MOD_OK)
        return mods::set_error(error, r, "hook escudo automatico");
    if ((r = mods::hook::add_pre<LinkExecute>(on_execute_pre)) != MOD_OK)
        return mods::set_error(error, r, "hook Link execute");
    if ((r = mods::hook::add_post<LinkExecute>(on_execute_post)) != MOD_OK)
        return mods::set_error(error, r, "hook Link execute post");
#if ENABLE_BAR
    if ((r = mods::hook::add_post<LinkDraw>(on_link_draw_post)) != MOD_OK)
        return mods::set_error(error, r, "hook dibujo de Link");
#endif
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {   // se llama cada frame
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    // El loader quita los hooks solo al desactivar el mod.
    g_enemies.clear();
#if ENABLE_BAR
    g_bars.clear();
#endif
    g_parryTimer = 0;
    g_parryHitThisTick = false;
    g_noAutoShield = true;
    return MOD_OK;
}

}
