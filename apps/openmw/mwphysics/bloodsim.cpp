#include "bloodsim.hpp"

#include <algorithm>
#include <cmath>

#include <osg/Math>

namespace MWPhysics
{
    namespace
    {
        // Steeper than this (the normal's height) is a wall; flatter, a floor; below the negative, a ceiling.
        constexpr float sFloorNormalZ = 0.7f;
        constexpr float sCeilingNormalZ = -0.3f;
        constexpr float sStepTime = 1.f / 60.f;
        constexpr float sMaxDuration = 0.25f; // a long frame (loading) isn't caught up on
        constexpr float sYoungAge = 0.1f;

        bool isFloor(const osg::Vec3f& normal)
        {
            return normal.z() >= sFloorNormalZ;
        }

        bool isWall(const osg::Vec3f& normal)
        {
            return normal.z() < sFloorNormalZ && normal.z() > sCeilingNormalZ;
        }

        osg::Vec3f normalized(osg::Vec3f v)
        {
            if (v.normalize() < 1e-6f)
                return osg::Vec3f(0, 0, 1);
            return v;
        }

        // Down the surface: gravity without the part into it. Zero (near enough) on a floor.
        osg::Vec3f downSlope(const osg::Vec3f& normal)
        {
            const osg::Vec3f down(0, 0, -1);
            return down - normal * (down * normal);
        }
    }

    BloodSim::BloodSim()
        : BloodSim(Settings())
    {
    }

    BloodSim::BloodSim(const Settings& settings)
        : mSettings(settings)
    {
    }

    float BloodSim::stainRadius(float volume) const
    {
        const float radius = std::sqrt(std::max(volume, 0.f) / (osg::PIf * mSettings.mStainThickness));
        return std::clamp(radius, 0.5f, mSettings.mMaxStainRadius);
    }

    void BloodSim::burst(const osg::Vec3f& center, const osg::Vec3f& velocity, int drops, int chunks, float maxSpeed,
        float volume, Misc::Rng::Generator& prng)
    {
        const int count = std::max(drops, 0) + std::max(chunks, 0);
        if (count == 0)
            return;
        const float dropVolume = drops > 0 ? volume / static_cast<float>(drops) : 0.f;
        for (int i = 0; i < count; ++i)
        {
            if (mDrops.size() >= mSettings.mMaxDrops)
                break;
            const bool chunk = i >= drops;
            // Uniform over the sphere, then tipped upwards: the burst comes from inside, the neck below.
            const float z = Misc::Rng::rollClosedProbability(prng) * 2.f - 1.f;
            const float angle = Misc::Rng::rollProbability(prng) * 2.f * osg::PIf;
            const float ring = std::sqrt(std::max(1.f - z * z, 0.f));
            const osg::Vec3f direction
                = normalized(osg::Vec3f(ring * std::cos(angle), ring * std::sin(angle), z) + osg::Vec3f(0, 0, 0.35f));
            // Most fly fairly fast, some just spill.
            float speed = maxSpeed * (0.25f + 0.75f * std::sqrt(Misc::Rng::rollProbability(prng)));
            BloodDrop drop;
            drop.mChunk = chunk;
            if (chunk)
            {
                speed *= 0.6f;
                drop.mVolume = 8.f * (0.5f + Misc::Rng::rollProbability(prng)); // a lump of a few cm
            }
            else
            {
                // Mostly small, a few big ones (about 1.2 times the average, on average).
                const float a = Misc::Rng::rollProbability(prng);
                const float b = Misc::Rng::rollProbability(prng);
                drop.mVolume = dropVolume * (0.2f + 4.f * a * b);
            }
            drop.mPosition = center;
            drop.mVelocity = velocity + direction * speed;
            mDrops.push_back(drop);
        }
    }

    void BloodSim::step(float duration, const BloodRayCast& castRay)
    {
        duration = std::min(duration, sMaxDuration);
        while (duration > 1e-5f)
        {
            const float dt = std::min(duration, sStepTime);
            stepOnce(dt, castRay);
            duration -= dt;
        }
    }

    void BloodSim::stepOnce(float dt, const BloodRayCast& castRay)
    {
        stepDrops(dt, castRay);
        stepRunners(dt, castRay);
    }

    void BloodSim::stepDrops(float dt, const BloodRayCast& castRay)
    {
        std::vector<BloodDrop> flying;
        flying.reserve(mDrops.size());
        // Moved out first: a hit can add runners and stains, and those can add drops (one running off an edge).
        std::vector<BloodDrop> drops = std::move(mDrops);
        mDrops.clear();
        for (BloodDrop& drop : drops)
        {
            drop.mAge += dt;
            if (drop.mAge > mSettings.mMaxAge)
                continue;
            drop.mVelocity.z() -= mSettings.mGravity * dt;
            // Lumps are heavier for their size: the air slows them less.
            drop.mVelocity *= std::exp(-mSettings.mDrag * (drop.mChunk ? 0.3f : 1.f) * dt);
            const osg::Vec3f to = drop.mPosition + drop.mVelocity * dt;
            const std::optional<BloodHit> hit = castRay(drop.mPosition, to, drop.mAge < sYoungAge);
            if (!hit)
            {
                drop.mPosition = to;
                flying.push_back(drop);
                continue;
            }
            if (hit->mMoving)
                continue; // On someone or something moving: a mark in the world wouldn't go along.
            const osg::Vec3f normal = normalized(hit->mNormal);
            const float into = drop.mVelocity * normal; // negative, towards the surface
            if (drop.mChunk && drop.mBounces < mSettings.mMaxChunkBounces && into < -80.f)
            {
                // A lump bounces, a little, and leaves a smear where it hit.
                const osg::Vec3f along = drop.mVelocity - normal * into;
                drop.mVelocity = along * 0.6f - normal * (into * mSettings.mChunkRestitution);
                drop.mPosition = hit->mPosition + normal * 0.5f;
                ++drop.mBounces;
                addStain(hit->mPosition, normal, drop.mVolume * 0.25f, true, false);
                drop.mVolume *= 0.75f;
                flying.push_back(drop);
                continue;
            }
            addStain(hit->mPosition, normal, drop.mVolume, drop.mChunk, !drop.mChunk);
        }
        // Anything added while stepping (drops running off edges) goes along with them.
        flying.insert(flying.end(), mDrops.begin(), mDrops.end());
        mDrops = std::move(flying);
    }

    void BloodSim::stepRunners(float dt, const BloodRayCast& castRay)
    {
        std::vector<BloodRunner> runners = std::move(mRunners);
        mRunners.clear();
        std::vector<BloodRunner> running;
        running.reserve(runners.size());
        for (BloodRunner& runner : runners)
        {
            runner.mAge += dt;
            osg::Vec3f slope = downSlope(runner.mNormal);
            const float steepness = slope.normalize();
            if (isFloor(runner.mNormal) || steepness < 0.2f || runner.mAge > mSettings.mMaxRunAge)
            {
                // Come to rest: what is left stays.
                addStain(runner.mPosition, runner.mNormal, runner.mVolume + runner.mTrailVolume, false, false);
                continue;
            }
            // Fuller runs run faster.
            const float speed = mSettings.mRunSpeed
                * std::clamp(std::sqrt(runner.mVolume / mSettings.mWallHoldVolume), 0.3f, 1.5f) * steepness;
            const float distance = speed * dt;
            const osg::Vec3f next = runner.mPosition + slope * distance;

            // Something in the way along the surface (the floor at the foot of the wall, a ledge): it stops there.
            const osg::Vec3f lift = runner.mNormal * 0.5f;
            if (const std::optional<BloodHit> blocked = castRay(runner.mPosition + lift, next + lift, false))
            {
                if (!blocked->mMoving)
                    addStain(blocked->mPosition, normalized(blocked->mNormal), runner.mVolume + runner.mTrailVolume,
                        false, false);
                continue;
            }
            // Still on the surface? Looked for from just off it, back into it.
            const std::optional<BloodHit> surface
                = castRay(next + runner.mNormal * 2.f, next - runner.mNormal * 3.f, false);
            if (!surface || surface->mMoving)
            {
                // The surface ends (an edge) or turns away: it drips off, on down.
                if (runner.mTrailVolume > 0)
                    addStain(runner.mPosition, runner.mNormal, runner.mTrailVolume, false, false);
                if (mDrops.size() < mSettings.mMaxDrops)
                {
                    BloodDrop drop;
                    drop.mPosition = next + runner.mNormal * 0.5f;
                    drop.mVelocity = slope * speed;
                    drop.mVolume = runner.mVolume;
                    drop.mAge = sYoungAge; // nothing of its own to leave out
                    mDrops.push_back(drop);
                }
                continue;
            }
            runner.mPosition = surface->mPosition;
            runner.mNormal = normalized(surface->mNormal);

            // Leaving some behind as it goes: a trail of marks.
            const float lost = runner.mVolume * std::min(mSettings.mRunLossPerUnit * distance, 1.f);
            runner.mVolume -= lost;
            runner.mTrailVolume += lost;
            runner.mSinceMark += distance;
            if (runner.mSinceMark >= mSettings.mTrailSpacing)
            {
                addStain(runner.mPosition, runner.mNormal, runner.mTrailVolume, false, false);
                runner.mTrailVolume = 0;
                runner.mSinceMark = 0;
            }
            if (runner.mVolume < mSettings.mMinRunVolume)
            {
                addStain(runner.mPosition, runner.mNormal, runner.mVolume + runner.mTrailVolume, false, false);
                continue;
            }
            running.push_back(runner);
        }
        // Runs started while stepping (marks overflowing) join in.
        running.insert(running.end(), mRunners.begin(), mRunners.end());
        mRunners = std::move(running);
    }

    void BloodSim::addStain(
        const osg::Vec3f& position, const osg::Vec3f& normalIn, float volume, bool chunk, bool canRun)
    {
        if (!(volume > 0.f))
            return;
        const osg::Vec3f normal = normalized(normalIn);

        // Onto a mark already there (of the same surface): they run together, the middle moving towards the
        // bigger.
        BloodStain* target = nullptr;
        const float newRadius = stainRadius(volume);
        const std::size_t search = std::min(mStains.size(), mSettings.mMergeSearch);
        for (std::size_t i = 0; i < search; ++i)
        {
            BloodStain& stain = mStains[mStains.size() - 1 - i];
            if (stain.mNormal * normal < 0.9f)
                continue;
            const osg::Vec3f offset = position - stain.mPosition;
            const float off = offset * normal; // off the stain's surface
            if (std::abs(off) > 3.f)
                continue;
            const float across = (offset - normal * off).length();
            if (across < stain.mRadius + newRadius * 0.5f)
            {
                target = &stain;
                break;
            }
        }
        if (target != nullptr)
        {
            const float total = target->mVolume + volume;
            target->mPosition = (target->mPosition * target->mVolume + position * volume) / total;
            target->mVolume = total;
            target->mChunk = target->mChunk || chunk;
        }
        else
        {
            BloodStain stain;
            stain.mPosition = position;
            stain.mNormal = normal;
            stain.mVolume = volume;
            stain.mChunk = chunk;
            mStains.push_back(stain);
            target = &mStains.back();
        }

        // More than a wall holds: the rest runs down.
        if (canRun && isWall(target->mNormal) && target->mVolume > mSettings.mWallHoldVolume
            && mRunners.size() < mSettings.mMaxRunners)
        {
            BloodRunner runner;
            runner.mPosition = target->mPosition;
            runner.mNormal = target->mNormal;
            runner.mVolume = target->mVolume - mSettings.mWallHoldVolume;
            target->mVolume = mSettings.mWallHoldVolume;
            mRunners.push_back(runner);
        }
        target->mRadius = stainRadius(target->mVolume);

        // target isn't used past here: dropping the oldest may move the deque's elements.
        while (mStains.size() > mSettings.mMaxStains)
            mStains.pop_front();
        ++mStainRevision;
    }

    void BloodSim::clear()
    {
        mDrops.clear();
        mRunners.clear();
        mStains.clear();
        ++mStainRevision;
    }
}
