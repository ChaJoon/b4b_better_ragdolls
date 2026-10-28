// Fork (ChaJoon/b4b_better_ragdolls, build 44): a wider first-person field of view than the game's slider allows.
//
// The game's FOV slider stops at 120 (horizontal, at 16:9), and a higher value written into its config is put back to
// 120 when the game starts (players' reports, 2021). So the camera's own output is changed instead: a camera modifier
// of ours on the local player's PlayerCameraManager. UE runs every modifier in the manager's ModifierList on each
// frame's final view (APlayerCameraManager::ApplyCameraModifiers -> UCameraModifier::ModifyCamera), and the base
// UCameraModifier hands the view to BlueprintModifyCamera, a Blueprint event, so through ProcessEvent even with no
// Blueprint behind it (UHT's thunk always calls it): ragdoll.c's ProcessEvent hook calls fov_pe_apply for our modifier
// and we set NewFOV. Nothing of the game's is written: its slider, its camera components and its aim zoom keep their
// own values, and our change is never fed back into them. Only the local view, on this machine: no protocol change.
//
// What is shown, while the camera shows the local hero in first person (its PlayerViewComponent's IsThirdPerson byte
// +0x215 clear, the manager's view target the hero or its controller; cutscenes, spectating and our /thirdperson are
// left alone): FOV x target / base, where base is the game's hip-fire FOV, the FOV coming in on frames without aiming
// (any of the hero's ADSComponents bIsHoldingADS, as thirdperson.c) once it has held for 0.5 s (the first one at once;
// none while the game zooms back out, 0.6 s after aiming). Aiming before any hip fire was seen: the game's zoom as is.
// So without aiming the target is shown, and aiming zooms in by the same share as the game's own (Joon, 28 Oct:
// "zooming in with your sights stays the same share narrower"). The modifier goes last in the list (Priority 255),
// after the game's own, so it has the final say over the FOV.
//
// Logged (discovery for later builds): the modifier list when ours goes in; the first call; the game's hip-fire FOV
// when it changes; the FOV coming in when it changes while not aiming (other FOV effects of the game: sprint, hits);
// the zoom at each start of aiming; a line if the camera never calls our modifier.
// Ini: fov=<60-160> (no key or 0 = the game's). Chat: /fov [<60-160>|off] (anyone, own view). ~ Camera tab: a slider.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ue.h"
#include "log.h"
#include "cmds.h"
#include "overlay.h"

#define FOV_MIN 60.f
#define FOV_MAX 160.f
#define BASE_HOLD 0.5f                                // s the hip-fire FOV holds before it becomes the base
#define LIVE_FLAGS 0x30                               // RF_ClassDefaultObject | RF_ArchetypeObject
#define COMP_OWNER(c)  (*(UObject **)((char *)(c) + 0xD8))   // UActorComponent::OwnerPrivate (unreflected; thirdperson.c)
#define PVC_IS_3P(p)   (*((uint8_t *)(p) + 0x215))            // PlayerViewComponent IsThirdPerson() (third-person.md)
#define FP_STRUCT(f)   (*(UStruct **)((char *)(f) + 0x78))    // FStructProperty::Struct (as ragdoll.c's FP_SUB)

static float fov_target;                       // 0 = the game's
static UObject *mod, *mod_pcm;                 // our modifier and the camera manager it is on
static int32_t modi = -1, mod_pcmi = -1;
static UFunction *bmc_fn;                      // CameraModifier.BlueprintModifyCamera
static int32_t o_in = -1, o_out = -1;          // its FOV / NewFOV parameters
static DWORD game_tid;
static int live, ads, live_logged;             // this frame (from the tick): apply; the hero aims
static float base, base_cand;                  // the game's hip-fire FOV (0 = not seen yet), a candidate
static ULONGLONG cand_ms, ads_ms;              // when the candidate came; the last frame with aiming
static float last_in, last_out, last_nonads_in;
static unsigned n_calls, n_applied, n_add_fail, call_logs, base_logs, in_logs, zoom_logs;
static int ads_was, zoom_pending;
static float live_s;                           // s live with no call (the camera never asks: logged once)
static int never_logged;

typedef struct { UObject *c; int32_t i; } Ref;
static UObject *hero, *pvc;
static int32_t heroi = -1, pvci = -1;
static Ref adsc[16];
static int n_adsc;
static float ads_age = 99.f;

typedef struct { UObject *obj; UFunction *fn; uint8_t p[512]; } FCall;
static void *fc_prep(FCall *c, UObject *obj, const char *fname) {
    c->obj = obj;
    c->fn = obj ? ue_find_function(U_CLASS(obj), fname) : NULL;
    if (!c->fn || UFN_PARMSSIZE(c->fn) > sizeof c->p) return NULL;
    memset(c->p, 0, sizeof c->p);
    return c;
}
static void *fc_arg(FCall *c, const char *name) { FField *f = ue_find_prop(c->fn, name); return f ? c->p + FP_OFFSET(f) : NULL; }
static int alive(UObject *o, int32_t i) { return o && i >= 0 && ue_object_usable(o, i); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- the hero, its view component and its aiming (as thirdperson.c: one scan per hero, the ADS list every second) ----
static UObject *local_hero(UObject *pc) {
    static UClass *hc;
    if (!hc) hc = ue_find_class("HeroCharacter");
    UObject *p = pc ? ue_get_ptr(pc, "Pawn") : NULL;
    return p && hc && ue_is_a(p, hc) ? p : NULL;
}
static void hero_scan(UObject *pawn) {
    static UClass *pvcc;
    if (!pvcc) pvcc = ue_find_class("PlayerViewComponent");
    hero = pawn; heroi = pawn ? U_INDEX(pawn) : -1; pvc = NULL; pvci = -1; n_adsc = 0; ads_age = 99.f;
    base = base_cand = last_nonads_in = 0;   // a new hero: the game's FOV seen again
    for (int32_t i = 0, n = ue_num_objects(); pvcc && pawn && i < n; i++) {
        UObject *x = ue_object_at(i);
        if (!x || (U_FLAGS(x) & LIVE_FLAGS) || !ue_is_a(x, pvcc)) continue;
        if (COMP_OWNER(x) == pawn) { pvc = x; pvci = i; break; }
    }
}
static void ads_refresh(void) {
    static UClass *ac;
    if (!ac) ac = ue_find_class("ADSComponent");
    n_adsc = 0;
    for (int32_t i = 0, n = ue_num_objects(); ac && hero && i < n && n_adsc < 16; i++) {
        UObject *x = ue_object_at(i);
        if (!x || (U_FLAGS(x) & LIVE_FLAGS) || !ue_is_a(x, ac)) continue;
        UObject *w = COMP_OWNER(x);
        if (w && (w == hero || ue_get_ptr(w, "Owner") == hero)) { adsc[n_adsc].c = x; adsc[n_adsc++].i = i; }
    }
}
static int hero_ads(void) {   // any of the hero's weapons held in ADS (ADSComponent.bIsHoldingADS)
    static int32_t off = -2;
    for (int i = 0; i < n_adsc; i++) {
        if (!alive(adsc[i].c, adsc[i].i)) continue;
        if (off == -2) off = ue_prop_offset(adsc[i].c, "bIsHoldingADS");
        if (off >= 0 && *((uint8_t *)adsc[i].c + off)) return 1;
    }
    return 0;
}

// ---- the modifier ----
static UObject *view_target(UObject *pcm) {   // PlayerCameraManager.ViewTarget.Target
    static int32_t off = -2;
    if (off == -2) {
        FField *vt = ue_find_prop(U_CLASS(pcm), "ViewTarget");
        UStruct *st = vt ? FP_STRUCT(vt) : NULL;
        FField *t = st ? ue_find_prop(st, "Target") : NULL;
        off = vt && t ? FP_OFFSET(vt) + FP_OFFSET(t) : -1;
        if (off < 0) LOG("fov: PlayerCameraManager.ViewTarget.Target not found: the FOV is never changed");
    }
    return off >= 0 ? *(UObject **)((char *)pcm + off) : NULL;
}
static void list_log(UObject *pcm, const char *what) {
    char line[900], a[96], b[96];
    int32_t lo = ue_prop_offset(pcm, "ModifierList");
    if (lo < 0) return;
    TArray *l = (TArray *)((char *)pcm + lo);
    int32_t po = mod ? ue_prop_offset(mod, "Priority") : -1;
    size_t k = (size_t)snprintf(line, sizeof line, "fov: %s; the camera's modifiers (%d):", what, l->num);
    for (int i = 0; l->data && i < l->num && i < 16 && k + 120 < sizeof line; i++) {
        UObject *m = ((UObject **)l->data)[i];
        if (!m) { k += (size_t)snprintf(line + k, sizeof line - k, " (none)"); continue; }
        k += (size_t)snprintf(line + k, sizeof line - k, " %s (%s, priority %d)%s", ue_obj_name(m, a, sizeof a),
                              ue_obj_name(U_CLASS(m), b, sizeof b), po >= 0 ? *((uint8_t *)m + po) : -1, m == mod ? " = ours" : "");
    }
    LOG("%s", line);
}
static int mod_add(UObject *pcm) {   // game thread: our modifier on this camera manager, last in its list. 1 = there
    char a[96], b[96];
    static UClass *cmc;
    if (!cmc) cmc = ue_find_class("CameraModifier");
    if (!bmc_fn && cmc) {
        bmc_fn = ue_find_function(cmc, "BlueprintModifyCamera");
        FField *fi = bmc_fn ? ue_find_prop(bmc_fn, "FOV") : NULL, *fo = bmc_fn ? ue_find_prop(bmc_fn, "NewFOV") : NULL;
        o_in = fi ? FP_OFFSET(fi) : -1;
        o_out = fo ? FP_OFFSET(fo) : -1;
        if (!bmc_fn || o_in < 0 || o_out < 0) {
            LOG("fov: CameraModifier.BlueprintModifyCamera (FOV, NewFOV) not found: the FOV can't be changed");
            bmc_fn = NULL;
        }
    }
    FCall c;
    UObject **cls_arg, **ret;
    if (!cmc || !bmc_fn || !fc_prep(&c, pcm, "AddNewCameraModifier") || !(cls_arg = fc_arg(&c, "ModifierClass")) ||
        !(ret = fc_arg(&c, "ReturnValue"))) {
        if (!n_add_fail++) LOG("fov: can't add a camera modifier (CameraModifier %p, AddNewCameraModifier missing)", (void *)cmc);
        return 0;
    }
    *cls_arg = cmc;
    ue_process_event(c.obj, c.fn, c.p);
    if (!*ret) { if (!n_add_fail++) LOG("fov: AddNewCameraModifier gave nothing: the FOV can't be changed"); return 0; }
    mod = *ret; modi = U_INDEX(mod); mod_pcm = pcm; mod_pcmi = U_INDEX(pcm);
    int32_t po = ue_prop_offset(mod, "Priority"), lo = ue_prop_offset(pcm, "ModifierList");
    if (po >= 0) *((uint8_t *)mod + po) = 255;   // modifiers added later with a lower priority go before it
    if (lo >= 0) {                               // and last now: the final say over the FOV
        TArray *l = (TArray *)((char *)pcm + lo);
        UObject **d = l->data;
        for (int i = 0; d && i < l->num - 1; i++)
            if (d[i] == mod) { memmove(d + i, d + i + 1, (size_t)(l->num - 1 - i) * sizeof *d); d[l->num - 1] = mod; break; }
    }
    list_log(pcm, "camera modifier added");
    LOG("fov: ours is %s on %s", ue_obj_name(mod, a, sizeof a), ue_obj_name(pcm, b, sizeof b));
    live_s = 0; never_logged = 0;
    return 1;
}

void fov_tick(float dt) {
    char a[96], b[96];
    game_tid = GetCurrentThreadId();
    live = 0;
    if (fov_target <= 0) return;
    UObject *pc = ue_local_pc(), *pcm = pc ? ue_get_ptr(pc, "PlayerCameraManager") : NULL;
    if (!pcm) return;
    if (!(mod_pcm == pcm && alive(mod_pcm, mod_pcmi) && alive(mod, modi))) {
        if (n_add_fail > 3) return;   // logged; not again every frame
        if (!mod_add(pcm)) return;
        ragdoll_pe_refresh();   // the ProcessEvent hook on (it is ragdoll.c's)
    }
    UObject *p = local_hero(pc);
    if (p != hero || !alive(hero, heroi)) hero_scan(p);
    if (!hero || !pvc || !alive(pvc, pvci)) return;
    if ((ads_age += dt) >= 1.f) { ads_age = 0; ads_refresh(); }
    UObject *vt = view_target(pcm);
    int fp = !PVC_IS_3P(pvc), ours = vt == hero || vt == pc;
    if (!ours && fp && !live_logged) {
        live_logged = 1;
        LOG("fov: the camera's view target is %s (%s), not the hero: the FOV left alone while it is", vt ? ue_obj_name(vt, a, sizeof a) : "none",
            vt && U_CLASS(vt) ? ue_obj_name(U_CLASS(vt), b, sizeof b) : "-");
    }
    ads = hero_ads();
    live = fp && ours;
    if (live && !n_calls && !never_logged && (live_s += dt) > 3.f) {
        never_logged = 1;
        list_log(pcm, "the camera never called our modifier in 3 s: the FOV is NOT changed");
    }
}

// ragdoll.c's ProcessEvent hook, after the call: our modifier's BlueprintModifyCamera (game thread)
int fov_pe_match(UObject *o, UFunction *fn) { return fn && fn == bmc_fn && o == mod; }
void fov_pe_apply(void *params) {
    if (!params || GetCurrentThreadId() != game_tid) return;
    float in = *(float *)((uint8_t *)params + o_in), *out = (float *)((uint8_t *)params + o_out);
    if (!n_calls++) LOG("fov: the camera calls our modifier (first call: FOV %.2f)", in);
    last_in = in;
    last_out = *out;
    if (!live || fov_target <= 0 || !(in > 1.f && in < 179.f)) return;
    ULONGLONG now = GetTickCount64();
    if (ads) ads_ms = now;
    if (!base) {
        if (ads) return;   // aiming before any hip fire was seen (e.g. aiming from /thirdperson): the game's zoom as is
        base = base_cand = in;
        cand_ms = now;
        LOG("fov: the game's hip-fire FOV %.2f (first seen); shown %.0f", in, fov_target);
    }
    if (!ads) {
        int settled = now - ads_ms > 600;   // not the game's zoom back out after aiming
        if (settled && last_nonads_in > 0 && fabsf(in - last_nonads_in) > 0.25f && in_logs < 30) {
            in_logs++;
            LOG("fov: the game's FOV without aiming %.2f -> %.2f (hip fire %.2f)", last_nonads_in, in, base);
        }
        last_nonads_in = in;
        if (fabsf(in - base_cand) > 0.01f) { base_cand = in; cand_ms = now; }
        else if (settled && fabsf(base_cand - base) > 0.01f && now - cand_ms >= (ULONGLONG)(BASE_HOLD * 1000)) {
            if (base_logs++ < 20) LOG("fov: the game's hip-fire FOV %.2f (was %.2f)", base_cand, base);
            base = base_cand;
        }
    }
    if (ads && !ads_was) zoom_pending = 20;   // the zoom settles over a few frames: logged after 20
    ads_was = ads;
    float shown = clampf(in * fov_target / base, 5.f, 170.f);
    *out = shown;
    last_out = shown;
    n_applied++;
    if (zoom_pending && ads && !--zoom_pending && zoom_logs++ < 10)
        LOG("fov: aiming: the game's FOV %.2f -> shown %.2f (hip fire %.2f -> %.0f)", in, shown, base, fov_target);
    if (call_logs < 3) { call_logs++; LOG("fov: frame: the game's FOV %.2f -> shown %.2f (hip fire %.2f, aiming %d)", in, shown, base, ads); }
}
int fov_pe_wanted(void) { return fov_target > 0 && mod != NULL; }

// ---- settings ----
static int setting(const char *k, const char *v) {
    if (strcmp(k, "fov")) return 0;
    float f = v ? (float)atof(v) : 0.f;
    fov_target = f > 0 ? clampf(f, FOV_MIN, FOV_MAX) : 0.f;
    return 1;
}
int fov_live(const char *key, const char *val) {
    if (!setting(key, val)) return 0;
    if (fov_target > 0) LOG("fov: %.0f", fov_target); else LOG("fov: the game's");
    if (fov_target <= 0) live = 0;
    return 1;
}
static void ini_pair(const char *k, const char *v, void *ctx) { (void)ctx; setting(k, v); }

void fov_slash(char *rest, Out *o) {
    char *a = rest ? strtok(rest, " ") : NULL;
    if (a) {
        float f = !_stricmp(a, "off") || !_stricmp(a, "game") ? 0.f : (float)atof(a);
        if (f != 0.f && (f < FOV_MIN || f > FOV_MAX)) { out_printf(o, "usage: /fov <%.0f-%.0f> | off\n", FOV_MIN, FOV_MAX); return; }
        char v[16];
        snprintf(v, sizeof v, "%.0f", f);
        setting("fov", f > 0 ? v : NULL);
        if (cmds_ini_set("fov", f > 0 ? v : NULL)) out_printf(o, "/fov: could not write b4bcoop.ini (see log): this lasts until the game closes\n");
        LOG("fov: /fov %s", f > 0 ? v : "off");
    }
    char t[32] = "the game's";
    if (fov_target > 0) snprintf(t, sizeof t, "%.0f", fov_target);
    out_printf(o, "/fov: first-person field of view %s (%.0f-%.0f, off = the game's slider; your own view only, saved)%s\n", t,
               FOV_MIN, FOV_MAX, fov_target > 0 && n_add_fail ? "; NOT AVAILABLE: see log" : "");
    if (fov_target > 0)
        out_printf(o, "/fov: now %s; the game's hip-fire FOV %.1f; last frame %.1f -> %.1f; aiming %s; %u frames changed\n",
                   live ? "first person, applied" : "not first person (left alone)", base, last_in, last_out, ads ? "yes" : "no", n_applied);
}

// rows for thirdperson.c's ~ Camera tab
void fov_panel(void) {
    float v = fov_target;
    ov_heading("First-person field of view");
    ov_width(14);
    if (ov_slider("FOV (0 = the game's slider)##fpfov", &v, 0, FOV_MAX, "%.0f")) ov_setting_f("fov", v >= FOV_MIN ? v : 0, 0);
    if (ov_edit_done()) ov_setting_f("fov", v >= FOV_MIN ? v : 0, 1);
    ov_tooltip("Wider than the game's slider allows (it stops at 120). Aiming still zooms in by the game's share. Below 60 = the game's own.");
}

int fov_init(void) {
    cmds_ini_each(ini_pair, NULL);
    LOG("fov: first-person field of view %s", fov_target > 0 ? "set by b4bcoop.ini" : "the game's");
    return 0;
}
