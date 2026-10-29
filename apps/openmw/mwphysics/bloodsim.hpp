#ifndef OPENMW_MWPHYSICS_BLOODSIM_H
#define OPENMW_MWPHYSICS_BLOODSIM_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <osg/Vec3f>

#include <components/misc/rng.hpp>

namespace MWPhysics
{
    /// What a ray (from, to) hit first: where, the surface's normal (towards from), and whether that is something
    /// that moves (an actor, a body, a loose item), which a stain in world space couldn't follow, or is under water:
    /// either way the blood is lost there.
    struct BloodHit
    {
        osg::Vec3f mPosition;
        osg::Vec3f mNormal;
        bool mMoving = false;
        std::uintptr_t mObject = 0; // what moves there, for whoever made the ray (0 for none, or under water)
    };

    /// Blood that landed on something moving (BloodHit::mObject), for its owner to put on it.
    struct BloodLoss
    {
        osg::Vec3f mPosition;
        osg::Vec3f mNormal;
        float mVolume = 0;
        std::uintptr_t mObject = 0;
    };

    /// The first thing a ray hits, if anything. young: the ray is for blood just thrown, which is still inside the
    /// body it came from (that one, and other moving things, are better left out).
    using BloodRayCast
        = std::function<std::optional<BloodHit>(const osg::Vec3f& from, const osg::Vec3f& to, bool young)>;

    /// Blood in flight: a droplet, or a lump of flesh (a chunk: heavier, bounces, leaves a darker mark).
    struct BloodDrop
    {
        osg::Vec3f mPosition;
        osg::Vec3f mVelocity;
        float mVolume = 0; // cubic units; a stain's size follows from it
        bool mChunk = false;
        int mBounces = 0;
        float mAge = 0;
    };

    /// Blood running down a steep surface under gravity: it follows the surface, leaving a trail and losing
    /// volume, and falls off where the surface ends (or turns away).
    struct BloodRunner
    {
        osg::Vec3f mPosition; // on the surface
        osg::Vec3f mNormal;
        float mVolume = 0;
        float mSinceMark = 0; // distance run since the last trail mark
        float mTrailVolume = 0; // lost since the last trail mark, which it goes into
        float mAge = 0;
    };

    /// Blood left on a surface: a round mark, its size from the volume in it (a thin film). Marks that land on one
    /// another run together; on a wall, one that gets more than the wall can hold runs down (see BloodRunner).
    struct BloodStain
    {
        osg::Vec3f mPosition;
        osg::Vec3f mNormal;
        float mRadius = 0;
        float mVolume = 0;
        bool mChunk = false;
    };

    /// A simulation of spilt blood: droplets and lumps thrown ballistically (gravity, air drag), colliding with
    /// the world through a ray cast each step; what hits a floor pools, what hits a wall runs down it, what hits
    /// something moving is lost. Not saved. Thread: the caller's (it only calls castRay).
    ///
    /// Units are the game's (about 70 to a meter); volumes are cubic units (one is about 3 ml, a litre about 343).
    class BloodSim
    {
    public:
        struct Settings
        {
            std::size_t mMaxDrops = 600;
            std::size_t mMaxRunners = 64;
            std::size_t mMaxStains = 400;
            float mGravity = 627.f; // 8.96 m/s^2, as the game's, in units
            float mDrag = 0.6f; // per second, of the velocity: droplets slow a little in the air
            float mMaxAge = 6.f; // in flight; past that it is dropped (flown off somewhere, never landed)
            float mChunkRestitution = 0.25f;
            int mMaxChunkBounces = 2;
            float mRunSpeed = 25.f; // of a run down a wall with mWallHoldVolume in it, units per second
            float mRunLossPerUnit = 0.03f; // of the volume, left behind per unit run
            float mMinRunVolume = 0.5f; // runs with less stop (they have all been left behind)
            float mMaxRunAge = 20.f;
            float mWallHoldVolume = 2.f; // what a mark on a wall holds before the rest runs down (~6 ml)
            float mTrailSpacing = 2.5f;
            float mStainThickness = 0.07f; // of the film (about 1 mm): radius = sqrt(volume / (pi thickness))
            float mMaxStainRadius = 45.f;
            std::size_t mMergeSearch = 96; // how many of the newest marks a new one may run into
        };

        BloodSim();
        explicit BloodSim(const Settings& settings);

        /// Throws drops droplets and chunks lumps from center, outwards in all directions (a bit more upwards, as
        /// from a head bursting from inside) at speeds up to maxSpeed, on top of velocity. volume is the blood in
        /// all the droplets together.
        void burst(const osg::Vec3f& center, const osg::Vec3f& velocity, int drops, int chunks, float maxSpeed,
            float volume, Misc::Rng::Generator& prng);

        /// Moves everything on by duration (seconds; long frames are taken in steps).
        void step(float duration, const BloodRayCast& castRay);

        void clear();

        const std::vector<BloodDrop>& getDrops() const { return mDrops; }
        const std::vector<BloodRunner>& getRunners() const { return mRunners; }
        const std::deque<BloodStain>& getStains() const { return mStains; }
        /// Blood that landed on moving things since the last call (at most a few hundred kept).
        std::vector<BloodLoss> takeLosses() { return std::exchange(mLosses, {}); }
        /// Changes whenever the stains do (for whoever draws them).
        std::size_t getStainRevision() const { return mStainRevision; }
        const Settings& getSettings() const { return mSettings; }
        Settings& getSettings() { return mSettings; }

        /// A stain's radius for a volume of blood.
        float stainRadius(float volume) const;

        /// Leaves volume on the surface at position (normal: the surface's): it runs into a mark there, or makes a
        /// new one. With canRun, more than a wall holds runs down it.
        void addStain(const osg::Vec3f& position, const osg::Vec3f& normal, float volume, bool chunk, bool canRun);

    private:
        void stepOnce(float dt, const BloodRayCast& castRay);
        void stepDrops(float dt, const BloodRayCast& castRay);
        void stepRunners(float dt, const BloodRayCast& castRay);

        Settings mSettings;
        std::vector<BloodDrop> mDrops;
        std::vector<BloodRunner> mRunners;
        std::deque<BloodStain> mStains; // oldest first
        std::vector<BloodLoss> mLosses;
        std::size_t mStainRevision = 0;
    };
}

#endif
