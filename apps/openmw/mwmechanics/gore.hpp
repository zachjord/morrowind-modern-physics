#ifndef GAME_MWMECHANICS_GORE_H
#define GAME_MWMECHANICS_GORE_H

// Fork feature "gore" ([Modern Physics] in settings.cfg): body parts can be destroyed, for good. What is lost is
// kept in the actor's stats (CreatureStats::LostBodyPart, saved), and each system reads it from there: the model
// leaves out the parts that went there (NpcAnimation), the ragdoll is made without them (World::makeRagdollBody).
// This file is what makes it happen, and its immediate effects (death, blood).

#include <components/esm/refid.hpp>

namespace MWWorld
{
    class Ptr;
    class ESMStore;
}

namespace MWMechanics
{
    /// Whether the feature is on.
    bool goreEnabled();

    /// The actor's head bursts: it dies at once, whatever its health, and its body falls limp (a ragdoll) where it
    /// stands, with no push from the blow; a body already dead loses its head where it lies. The head goes with
    /// its hair and anything worn on it (which stays in the inventory).
    /// For now only for people (NPCs with the standard skeleton), not the player.
    /// @return false if nothing happened (feature off, not an NPC in the scene, or the head is already gone)
    bool explodeHead(const MWWorld::Ptr& actor);

    /// The "Burst Head" magic effect (explodeHead on whoever it hits) and a spell with it, on target. They are the
    /// engine's own records, made at every start (see addGoreRecords), in no content file and not saved.
    extern const ESM::RefId sBurstHeadEffect;
    extern const ESM::RefId sBurstHeadSpell;

    /// Adds those records, looking like Damage Health (school, icon, visuals, sounds). Call before
    /// ESMStore::setUp. Does nothing without Damage Health (no Morrowind.esm) or if they are already there.
    void addGoreRecords(MWWorld::ESMStore& store);

    /// Teaches the player the Burst Head spell, if gore is on and "gore spell" too.
    void teachGoreSpells(const MWWorld::Ptr& player);
}

#endif
