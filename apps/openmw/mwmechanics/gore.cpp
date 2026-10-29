#include "gore.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include <osg/Matrixf>
#include <osg/Vec3f>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/animation.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/worldmodel.hpp"

#include "actorutil.hpp"
#include "creaturestats.hpp"
#include "spells.hpp"

namespace MWMechanics
{
    namespace
    {
        // Where the middle of the head is (the head bone is at the top of the neck), or nothing without one.
        std::optional<osg::Vec3f> getHeadCenter(const MWRender::Animation& animation)
        {
            const std::optional<osg::Matrixf> head = animation.getBoneWorldMatrix("Bip01 Head");
            if (!head)
                return std::nullopt;
            osg::Vec3f up(0, 0, 1);
            if (const std::optional<osg::Matrixf> neck = animation.getBoneWorldMatrix("Bip01 Neck"))
            {
                up = head->getTrans() - neck->getTrans();
                if (up.normalize() < 1e-3f)
                    up = osg::Vec3f(0, 0, 1);
            }
            // About half the head's height (see the head part in ragdoll.cpp).
            return head->getTrans() + up * 7.f;
        }

        // A neck still pumping blood after the head is gone. Kept by reference number, so a body that is unloaded
        // (or deleted) simply stops.
        struct Wound
        {
            ESM::RefNum mActor;
            float mAge = 0;
            float mNextPulse = 0.15f; // the first a moment after the burst
        };

        constexpr float sBleedTime = 9.f;

        std::vector<Wound>& getWounds()
        {
            static std::vector<Wound> wounds;
            return wounds;
        }

        // From the top of the neck, along it, out of the body.
        bool getNeckOpening(const MWRender::Animation& animation, osg::Vec3f& position, osg::Vec3f& direction)
        {
            const std::optional<osg::Matrixf> head = animation.getBoneWorldMatrix("Bip01 Head");
            const std::optional<osg::Matrixf> neck = animation.getBoneWorldMatrix("Bip01 Neck");
            if (!head || !neck)
                return false;
            position = head->getTrans();
            direction = head->getTrans() - neck->getTrans();
            if (direction.normalize() < 1e-3f)
                direction = osg::Vec3f(0, 0, 1);
            return true;
        }

        void pulse(const MWWorld::Ptr& actor, const Wound& wound)
        {
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            const MWRender::Animation* animation = world.getAnimation(actor);
            osg::Vec3f position;
            osg::Vec3f direction;
            if (animation == nullptr || !getNeckOpening(*animation, position, direction))
                return;
            // The heart giving out: each spurt weaker than the last.
            const float strength = std::exp(-wound.mAge / 2.5f);
            const int droplets = Settings::modernPhysics().mGoreBloodDroplets;
            const int drops = std::max(1, static_cast<int>(static_cast<float>(droplets) * 0.05f * (0.3f + strength)));
            world.spillBlood(position + direction * 1.5f, direction * (40.f + 160.f * strength), drops, 0,
                15.f + 25.f * strength, 0.5f + 5.f * strength);
        }

        // The blood and bits, simulated (see MWPhysics::BloodSim), with a fine mist of Morrowind's own blood splash
        // (the kind the actor bleeds) where the head was.
        void spawnHeadBurst(const MWWorld::Ptr& actor, const osg::Vec3f& center)
        {
            MWBase::World& world = *MWBase::Environment::get().getWorld();
            Misc::Rng::Generator& prng = world.getPrng();

            const int bloodType = actor.get<ESM::NPC>()->mBase->mBloodType;
            std::string texture(Fallback::Map::getString("Blood_Texture_" + std::to_string(bloodType)));
            if (texture.empty())
                texture = Fallback::Map::getString("Blood_Texture_0");

            // Red blood only: the other kinds (bones' dust, sparks) aren't liquid.
            const int droplets = Settings::modernPhysics().mGoreBloodDroplets;
            if (bloodType == 0 && droplets > 0)
            {
                // About half a litre (a litre is ~343 cubic units), thrown at up to ~4 m/s: a burst from inside,
                // not a blast.
                world.spillBlood(center, osg::Vec3f(), droplets, std::max(droplets / 12, 4), 280.f, 170.f);
            }

            const int bursts = droplets > 0 && bloodType == 0 ? 2 : 5;
            for (int i = 0; i < bursts; ++i)
            {
                const std::string_view model
                    = Fallback::Map::getString("Blood_Model_" + std::to_string(Misc::Rng::rollDice(3, prng)));
                if (model.empty())
                    continue;
                // The first right where the head was, the rest a little around it.
                osg::Vec3f offset;
                if (i > 0)
                    offset = osg::Vec3f(
                        Misc::Rng::deviate(0, 6, prng), Misc::Rng::deviate(0, 6, prng), Misc::Rng::deviate(0, 5, prng));
                const VFS::Path::Normalized path = Misc::ResourceHelpers::correctMeshPath(VFS::Path::Normalized(model));
                world.spawnEffect(path, texture, center + offset, i == 0 ? 1.6f : 1.1f, false);
            }

            MWBase::Environment::get().getSoundManager()->playSound3D(
                actor, ESM::RefId::stringRefId("critical damage"), 1.f, Misc::Rng::deviate(0.85f, 0.1f, prng));
        }
    }

    const ESM::RefId sBurstHeadEffect = ESM::RefId::stringRefId("BurstHead");
    const ESM::RefId sBurstHeadSpell = ESM::RefId::stringRefId("gore_burst_head");

    bool goreEnabled()
    {
        return Settings::modernPhysics().mGore;
    }

    void addGoreRecords(MWWorld::ESMStore& store)
    {
        const ESM::MagicEffect* damageHealth = store.get<ESM::MagicEffect>().search(ESM::MagicEffect::DamageHealth);
        if (damageHealth == nullptr || store.get<ESM::MagicEffect>().search(sBurstHeadEffect) != nullptr)
            return;

        ESM::MagicEffect effect = *damageHealth;
        effect.mId = sBurstHeadEffect;
        effect.mName = "Burst Head";
        effect.mDescription = "The target's head bursts. Only people's heads.";
        // All or nothing, once, on someone else.
        effect.mData.mFlags = ESM::MagicEffect::Harmful | ESM::MagicEffect::NoMagnitude | ESM::MagicEffect::NoDuration
            | ESM::MagicEffect::AppliedOnce | ESM::MagicEffect::CastTouch | ESM::MagicEffect::CastTarget;
        effect.mData.mBaseCost = 50.f;
        store.insertStatic(effect);

        ESM::Spell spell;
        spell.blank();
        spell.mId = sBurstHeadSpell;
        spell.mName = "Burst Head";
        spell.mData.mType = ESM::Spell::ST_Spell;
        spell.mData.mCost = 25;
        spell.mData.mFlags = 0; // nobody else gets it from autocalc
        ESM::IndexedENAMstruct burst;
        burst.mData.mEffectID = sBurstHeadEffect;
        burst.mData.mRange = ESM::RT_Target;
        burst.mData.mArea = 0;
        burst.mData.mDuration = 0;
        burst.mData.mMagnMin = 0;
        burst.mData.mMagnMax = 0;
        burst.mIndex = 0;
        spell.mEffects.mList.push_back(burst);
        store.insertStatic(spell);
    }

    void updateBleeding(float duration)
    {
        std::vector<Wound>& wounds = getWounds();
        MWWorld::WorldModel& worldModel = *MWBase::Environment::get().getWorldModel();
        for (auto it = wounds.begin(); it != wounds.end();)
        {
            Wound& wound = *it;
            wound.mAge += duration;
            const MWWorld::Ptr actor = worldModel.getPtr(wound.mActor);
            if (wound.mAge > sBleedTime || actor.isEmpty() || !actor.getRefData().isEnabled()
                || !actor.getClass().getCreatureStats(actor).hasLostBodyPart(CreatureStats::LostBodyPart_Head))
            {
                it = wounds.erase(it);
                continue;
            }
            wound.mNextPulse -= duration;
            if (wound.mNextPulse <= 0)
            {
                pulse(actor, wound);
                // About 75 beats a minute, slowing.
                wound.mNextPulse += 0.8f + 0.5f * wound.mAge / sBleedTime;
            }
            ++it;
        }
    }

    void clearBleeding()
    {
        getWounds().clear();
    }

    void teachGoreSpells(const MWWorld::Ptr& player)
    {
        if (!goreEnabled() || !Settings::modernPhysics().mGoreSpell || player.isEmpty())
            return;
        if (MWBase::Environment::get().getESMStore()->get<ESM::Spell>().search(sBurstHeadSpell) == nullptr)
            return;
        Spells& spells = player.getClass().getCreatureStats(player).getSpells();
        if (!spells.hasSpell(sBurstHeadSpell))
            spells.add(sBurstHeadSpell);
    }

    bool explodeHead(const MWWorld::Ptr& actor)
    {
        if (!goreEnabled() || actor.isEmpty() || !actor.getClass().isNpc() || actor == getPlayer())
            return false;
        CreatureStats& stats = actor.getClass().getCreatureStats(actor);
        if (stats.hasLostBodyPart(CreatureStats::LostBodyPart_Head))
            return false;
        MWBase::World& world = *MWBase::Environment::get().getWorld();
        MWRender::Animation* animation = world.getAnimation(actor);
        if (animation == nullptr)
            return false;
        const std::optional<osg::Vec3f> headCenter = getHeadCenter(*animation);
        if (!headCenter)
            return false; // Not built like a person: nothing would know what the head is.

        stats.setLostBodyParts(stats.getLostBodyParts() | CreatureStats::LostBodyPart_Head);
        animation->updateLostBodyParts();
        spawnHeadBurst(actor, *headCenter);
        if (actor.get<ESM::NPC>()->mBase->mBloodType == 0 && Settings::modernPhysics().mGoreBloodDroplets > 0)
            getWounds().push_back(Wound{ actor.getCellRef().getRefNum() });

        if (!stats.isDead())
        {
            // It just drops: dead on its feet, gravity does the rest (CharacterController makes the ragdoll as it
            // dies, from the bones as they are now).
            stats.setDeathKick(osg::Vec3f());
            DynamicStat<float> health = stats.getHealth();
            health.setCurrent(0.f, true);
            stats.setHealth(health);
        }
        else if (stats.isDeathAnimationFinished())
        {
            // A body lying dead: its ragdoll made again without the head, from where its bones are now (as it
            // would be if picked up; see World::grabObject). One still playing its death animation keeps it.
            world.removeRagdoll(actor);
            if (!world.makeRagdoll(actor, osg::Vec3f(), true, false))
                Log(Debug::Warning) << "Gore: no ragdoll for the body of " << actor.getCellRef().getRefId();
        }
        return true;
    }
}
