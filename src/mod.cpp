// Nightmare Lizalfos: mod entry point. Declares the hooks, owns the difficulty setting and its
// UI row, and forwards everything to lizalfos_nm.cpp.
//
// All hooks target file-local statics of the vanilla src/d/actor/d_a_e_dn.cpp through the SDK's
// "source.cpp#function" aliases. Nothing in the game source is changed.

#include "lizalfos_nm.h"

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include "d/dolzel_rel.h"
#include "d/actor/d_a_e_dn.h"

#include <cstdarg>
#include <cstdio>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
// The settings row is a convenience: without a UI service the difficulty can still be changed in
// config.json, so the mod does not hard-fail on it.
IMPORT_OPTIONAL_SERVICE(UiService, svc_ui);

DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#action", void(e_dn_class*), DnAction);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#damage_check", void(e_dn_class*), DnDamageCheck);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#daE_DN_Delete", int(e_dn_class*), DnDelete);
DEFINE_HOOK_SYMBOL("src/d/actor/d_a_e_dn.cpp#daE_DN_Execute", int(e_dn_class*), DnExecute);

namespace {

ConfigVarHandle g_var_difficulty = 0;
ConfigVarHandle g_var_damage = 0;

// Index in the 'Lizalfos damage' dropdown -> multiplier on its sword damage (vanilla = x1).
const int kDamageMult[] = {1, 2, 3, 4, 6};

void logf(LogLevel level, const char* format, ...) {
    char buffer[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    svc_log->write(mod_ctx, level, buffer);
}

void dbg(const char* format, ...) {
    char buffer[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    svc_log->write(mod_ctx, LOG_LEVEL_INFO, buffer);
}

const char* level_name(int level) {
    return level <= 0 ? "Disabled" : (level == 1 ? "Advanced" : "Nightmare");
}

int clamp_level(int64_t v) { return v < 0 ? 0 : (v > 2 ? 2 : (int)v); }
int damage_mult_of(int64_t v) { return kDamageMult[v < 0 ? 0 : (v > 4 ? 4 : (int)v)]; }

void on_damage_changed(ModContext*, ConfigVarHandle, const ConfigVarValue* value, const ConfigVarValue*, void*) {
    lnm::g_damage_mult = damage_mult_of(value->int_value);
    logf(LOG_LEVEL_INFO, "Lizalfos damage: x%d", lnm::g_damage_mult);
}

void on_difficulty_changed(ModContext*, ConfigVarHandle, const ConfigVarValue* value, const ConfigVarValue*, void*) {
    lnm::g_level = clamp_level(value->int_value);
    logf(LOG_LEVEL_INFO, "Lizalfos difficulty: %s", level_name(lnm::g_level));
}

// --- hook callbacks (each just forwards; the logic lives in lizalfos_nm.cpp) ---------------------
HookAction on_action_pre(ModContext*, void* args, void*, void*) {
    lnm::action_pre(mods::arg<e_dn_class*>(args, 0));
    return HOOK_CONTINUE;
}
void on_action_post(ModContext*, void* args, void*, void*) {
    lnm::action_post(mods::arg<e_dn_class*>(args, 0));
}
HookAction on_damage_pre(ModContext*, void* args, void*, void*) {
    lnm::damage_pre(mods::arg<e_dn_class*>(args, 0));
    return HOOK_CONTINUE;
}
void on_damage_post(ModContext*, void* args, void*, void*) {
    lnm::damage_post(mods::arg<e_dn_class*>(args, 0));
}
void on_execute_post(ModContext*, void* args, void*, void*) {
    lnm::execute_post(mods::arg<e_dn_class*>(args, 0));
}
HookAction on_delete_pre(ModContext*, void* args, void*, void*) {
    lnm::actor_delete(mods::arg<e_dn_class*>(args, 0));
    return HOOK_CONTINUE;
}

// --- settings panel -------------------------------------------------------------------------
const char* const kDifficultyOptions[] = {"Disabled", "Advanced", "Nightmare"};
const char* const kDamageOptions[] = {"x1 (normal)", "x2", "x3", "x4", "x6"};

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Nightmare Lizalfos");

    UiControlDesc difficulty = UI_CONTROL_DESC_INIT;
    difficulty.kind = UI_CONTROL_DROPDOWN;
    difficulty.label = "Lizalfos difficulty";
    difficulty.help_rml =
        "<p><b>Disabled</b>: vanilla Lizalfos.</p>"
        "<p><b>Advanced</b>: longer sight, a little faster, raises its shield and sidesteps your swings.</p>"
        "<p><b>Nightmare</b>: reads every swing you press and guards from any angle or dodges, "
        "punishes misses, chains up to 5-7 fast swings, runs faster, resists hitstun and gets fiercer "
        "with every hit you land. After a run of reads it leaves a short opening. Its health is not changed.</p>";
    difficulty.binding = UI_BINDING_CONFIG_VAR;
    difficulty.config_var = g_var_difficulty;
    difficulty.options = kDifficultyOptions;
    difficulty.option_count = 3;
    svc_ui->pane_add_control(mod_ctx, panel, &difficulty, nullptr);

    UiControlDesc damage = UI_CONTROL_DESC_INIT;
    damage.kind = UI_CONTROL_DROPDOWN;
    damage.label = "Lizalfos damage (Nightmare)";
    damage.help_rml =
        "<p>Multiplies the damage its sword and tail deal to you on Nightmare. x1 is the normal game.</p>";
    damage.binding = UI_BINDING_CONFIG_VAR;
    damage.config_var = g_var_damage;
    damage.options = kDamageOptions;
    damage.option_count = 5;
    svc_ui->pane_add_control(mod_ctx, panel, &damage, nullptr);

    char status[200];
    std::snprintf(status, sizeof(status), "Active hooks: AI %s, shield/poise %s, damage %s, cleanup %s.",
                  lnm::g_caps.action ? "on" : "OFF", lnm::g_caps.damage_check ? "on" : "OFF",
                  lnm::g_caps.execute ? "on" : "OFF", lnm::g_caps.actor_delete ? "on" : "OFF");
    svc_ui->pane_add_text(mod_ctx, panel, status, nullptr);
    return MOD_OK;
}

}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError* error) {
    // 1) The difficulty setting (saved in config.json; default Nightmare).
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = "difficulty";
    desc.type = CONFIG_VAR_INT;
    desc.default_int = 2;
    ModResult result = svc_config->register_var(mod_ctx, &desc, &g_var_difficulty);
    if (result != MOD_OK) {
        return mods::set_error(error, result, "could not register the 'difficulty' setting");
    }
    int64_t stored = 2;
    svc_config->get_int(mod_ctx, g_var_difficulty, &stored);
    lnm::g_level = clamp_level(stored);
    svc_config->subscribe(mod_ctx, g_var_difficulty, on_difficulty_changed, nullptr, nullptr);

    ConfigVarDesc damage_desc = CONFIG_VAR_DESC_INIT;
    damage_desc.name = "damage_level";
    damage_desc.type = CONFIG_VAR_INT;
    damage_desc.default_int = 2;  // x3
    if (svc_config->register_var(mod_ctx, &damage_desc, &g_var_damage) == MOD_OK) {
        int64_t dmg = 2;
        svc_config->get_int(mod_ctx, g_var_damage, &dmg);
        lnm::g_damage_mult = damage_mult_of(dmg);
        svc_config->subscribe(mod_ctx, g_var_damage, on_damage_changed, nullptr, nullptr);
    }

    // Diagnostics: 'debug_log' (1 = on) prints what the Lizalfos AI decides to the Dusklight log.
    ConfigVarDesc dbg_desc = CONFIG_VAR_DESC_INIT;
    dbg_desc.name = "debug_log";
    dbg_desc.type = CONFIG_VAR_INT;
    dbg_desc.default_int = 1;
    ConfigVarHandle dbg_var = 0;
    int64_t dbg_on = 1;
    if (svc_config->register_var(mod_ctx, &dbg_desc, &dbg_var) == MOD_OK) {
        svc_config->get_int(mod_ctx, dbg_var, &dbg_on);
    }
    lnm::g_log = dbg;
    lnm::g_debug = dbg_on != 0;

    // 2) Hooks. The AI hook is essential: without it there is nothing to do, so fail loudly.
    result = mods::hook::add_pre<DnAction>(on_action_pre);
    if (result == MOD_OK) {
        result = mods::hook::add_post<DnAction>(on_action_post);
    }
    if (result != MOD_OK) {
        return mods::set_error(error, result,
            "cannot hook the Lizalfos action() function: this Dusklight build has no symbol "
            "'src/d/actor/d_a_e_dn.cpp#action' (symbol manifest missing, or the function was inlined)");
    }
    lnm::g_caps.action = true;

    // Shield-by-collision and poise are optional: without them the AI still runs (and the log says so).
    result = mods::hook::add_pre<DnDamageCheck>(on_damage_pre);
    if (result == MOD_OK) {
        result = mods::hook::add_post<DnDamageCheck>(on_damage_post);
    }
    lnm::g_caps.damage_check = result == MOD_OK;
    if (!lnm::g_caps.damage_check) {
        logf(LOG_LEVEL_WARN, "damage_check() could not be hooked (%d): shield blocking and poise are off", (int)result);
    }

    result = mods::hook::add_pre<DnDelete>(on_delete_pre);
    lnm::g_caps.actor_delete = result == MOD_OK;
    if (!lnm::g_caps.actor_delete) {
        logf(LOG_LEVEL_WARN, "daE_DN_Delete() could not be hooked (%d): state slots are recycled lazily", (int)result);
    }

    result = mods::hook::add_post<DnExecute>(on_execute_post);
    lnm::g_caps.execute = result == MOD_OK;
    if (!lnm::g_caps.execute) {
        logf(LOG_LEVEL_WARN, "daE_DN_Execute() could not be hooked (%d): its damage stays vanilla", (int)result);
    }

    // 3) Settings row.
    if (svc_ui != nullptr) {
        UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
        panel.build = build_panel;
        if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
            logf(LOG_LEVEL_WARN, "could not register the settings panel; edit 'difficulty' in config.json instead");
        }
    }

    logf(LOG_LEVEL_INFO, "Nightmare Lizalfos ready. Difficulty: %s, damage x%d", level_name(lnm::g_level), lnm::g_damage_mult);
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    lnm::shutdown();
    return MOD_OK;
}
}
