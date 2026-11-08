#pragma once

/**
 * Engine-facing side of the Nightmare Lizalfos mod. mod.cpp declares the hooks (the SDK needs
 * them in the translation unit that owns DEFINE_MOD) and forwards each callback to one of the
 * functions below. Everything here works on the vanilla `e_dn_class` through its public fields;
 * no game source is modified.
 */

class e_dn_class;

namespace lnm {

// Difficulty, kept in sync with the config var by mod.cpp. 0 = Disabled, 1 = Advanced, 2 = Nightmare.
extern int g_level;

// Hooks around the vanilla file-local statics in d_a_e_dn.cpp.
void action_pre(e_dn_class* dn);     // before action(): perceive, decide, react
void action_post(e_dn_class* dn);    // after action():  footwork, combos, shield and bookkeeping
void damage_pre(e_dn_class* dn);     // before damage_check(): shield block by real collision
void damage_post(e_dn_class* dn);    // after damage_check():  poise (no hitstun, damage kept)
void actor_delete(e_dn_class* dn);   // daE_DN_Delete: free the actor's state slot

// Gives back everything the mod changed on live actors (sight range, held animations, mod
// states) and clears the state pool. Called on mod shutdown and when the mod is disabled.
void shutdown();

// Which of the optional hooks are live. Only used to report honestly in the log / panel.
struct Capabilities {
    bool action;
    bool damage_check;
    bool actor_delete;
};
extern Capabilities g_caps;

}  // namespace lnm
