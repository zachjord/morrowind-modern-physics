#include "gore.hpp"

#include <algorithm>
#include <optional>
#include <string>

#include <osg/Matrixf>
#include <osg/Vec3f>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/fallback/fallback.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/animation.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include "actorutil.hpp"
#include "creaturestats.hpp"

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

    bool goreEnabled()
    {
        return Settings::modernPhysics().mGore;
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
