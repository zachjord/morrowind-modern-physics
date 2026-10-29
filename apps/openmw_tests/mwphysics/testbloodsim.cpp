#include "apps/openmw/mwphysics/bloodsim.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <optional>
#include <vector>

#include <osg/Vec2f>

namespace
{
    using namespace testing;
    using MWPhysics::BloodHit;
    using MWPhysics::BloodRayCast;
    using MWPhysics::BloodSim;

    // A world of flat surfaces, each hit from its front (the side its normal points to), within bounds.
    struct Plane
    {
        osg::Vec3f mPoint;
        osg::Vec3f mNormal;
        osg::Vec3f mMin{ -1e6f, -1e6f, -1e6f };
        osg::Vec3f mMax{ 1e6f, 1e6f, 1e6f };
        bool mMoving = false;
    };

    struct FakeWorld
    {
        std::vector<Plane> mPlanes;
        int mRays = 0;
        int mYoungRays = 0;

        BloodRayCast rayCast()
        {
            return [this](const osg::Vec3f& from, const osg::Vec3f& to, bool young) -> std::optional<BloodHit> {
                ++mRays;
                if (young)
                    ++mYoungRays;
                std::optional<BloodHit> best;
                float bestT = std::numeric_limits<float>::max();
                const osg::Vec3f ray = to - from;
                for (const Plane& plane : mPlanes)
                {
                    const float approach = ray * plane.mNormal;
                    if (approach >= 0)
                        continue;
                    const float t = ((plane.mPoint - from) * plane.mNormal) / approach;
                    if (t < 0 || t > 1 || t >= bestT)
                        continue;
                    const osg::Vec3f at = from + ray * t;
                    bool inside = true;
                    for (int i = 0; i < 3; ++i)
                        inside = inside && at[i] >= plane.mMin[i] - 1e-3f && at[i] <= plane.mMax[i] + 1e-3f;
                    if (!inside)
                        continue;
                    bestT = t;
                    best = BloodHit{ at, plane.mNormal, plane.mMoving };
                }
                return best;
            };
        }
    };

    Plane floorAt(float z)
    {
        return Plane{ osg::Vec3f(0, 0, z), osg::Vec3f(0, 0, 1) };
    }

    // Facing -x, at x.
    Plane wallAt(float x)
    {
        return Plane{ osg::Vec3f(x, 0, 0), osg::Vec3f(-1, 0, 0) };
    }

    float stainVolume(const BloodSim& sim)
    {
        float total = 0;
        for (const MWPhysics::BloodStain& stain : sim.getStains())
            total += stain.mVolume;
        return total;
    }

    float liveVolume(const BloodSim& sim)
    {
        float total = stainVolume(sim);
        for (const MWPhysics::BloodDrop& drop : sim.getDrops())
            total += drop.mVolume;
        for (const MWPhysics::BloodRunner& runner : sim.getRunners())
            total += runner.mVolume + runner.mTrailVolume;
        return total;
    }

    BloodSim::Settings roomySettings()
    {
        BloodSim::Settings settings;
        settings.mMaxStains = 100000;
        return settings;
    }

    TEST(MWPhysicsBloodSimTest, burstThrowsDropsAndChunksFromTheCenter)
    {
        BloodSim sim;
        Misc::Rng::Generator prng(42);
        sim.burst(osg::Vec3f(1, 2, 3), osg::Vec3f(), 50, 5, 200, 100, prng);
        ASSERT_EQ(sim.getDrops().size(), 55u);
        int chunks = 0;
        float dropVolume = 0;
        for (const MWPhysics::BloodDrop& drop : sim.getDrops())
        {
            EXPECT_EQ(drop.mPosition, osg::Vec3f(1, 2, 3));
            EXPECT_LE(drop.mVelocity.length(), 200.f + 1e-3f);
            EXPECT_GT(drop.mVolume, 0.f);
            if (drop.mChunk)
                ++chunks;
            else
                dropVolume += drop.mVolume;
        }
        EXPECT_EQ(chunks, 5);
        // Random sizes around the average: in the right range, not exact.
        EXPECT_GT(dropVolume, 50.f);
        EXPECT_LT(dropVolume, 200.f);
    }

    TEST(MWPhysicsBloodSimTest, burstKeepsToTheMaximum)
    {
        BloodSim::Settings settings;
        settings.mMaxDrops = 10;
        BloodSim sim(settings);
        Misc::Rng::Generator prng(1);
        sim.burst(osg::Vec3f(), osg::Vec3f(), 50, 5, 200, 100, prng);
        sim.burst(osg::Vec3f(), osg::Vec3f(), 50, 5, 200, 100, prng);
        EXPECT_EQ(sim.getDrops().size(), 10u);
    }

    TEST(MWPhysicsBloodSimTest, dropsFallOntoTheFloorAndAllTheBloodStaysThere)
    {
        BloodSim sim(roomySettings());
        FakeWorld world;
        world.mPlanes.push_back(floorAt(0));
        Misc::Rng::Generator prng(7);
        sim.burst(osg::Vec3f(0, 0, 60), osg::Vec3f(), 120, 10, 280, 170, prng);
        const float thrown = liveVolume(sim);

        for (int i = 0; i < 60 * 5; ++i)
            sim.step(1.f / 60, world.rayCast());

        EXPECT_TRUE(sim.getDrops().empty());
        EXPECT_TRUE(sim.getRunners().empty());
        ASSERT_FALSE(sim.getStains().empty());
        for (const MWPhysics::BloodStain& stain : sim.getStains())
        {
            EXPECT_NEAR(stain.mPosition.z(), 0.f, 1e-3f);
            EXPECT_EQ(stain.mNormal, osg::Vec3f(0, 0, 1));
            EXPECT_GT(stain.mRadius, 0.f);
        }
        EXPECT_NEAR(stainVolume(sim), thrown, thrown * 1e-4f);
        // Spread out from under the burst, but not across the room.
        for (const MWPhysics::BloodStain& stain : sim.getStains())
            EXPECT_LT(osg::Vec2f(stain.mPosition.x(), stain.mPosition.y()).length(), 400.f);
    }

    TEST(MWPhysicsBloodSimTest, raysForFreshBloodAreMarkedYoung)
    {
        BloodSim sim;
        FakeWorld world;
        Misc::Rng::Generator prng(3);
        sim.burst(osg::Vec3f(0, 0, 1000), osg::Vec3f(), 1, 0, 0, 1, prng);
        sim.step(1.f / 60, world.rayCast());
        EXPECT_EQ(world.mYoungRays, 1);
        for (int i = 0; i < 12; ++i)
            sim.step(1.f / 60, world.rayCast());
        EXPECT_EQ(world.mRays, 13);
        EXPECT_LT(world.mYoungRays, 13);
    }

    TEST(MWPhysicsBloodSimTest, bloodHittingSomethingMovingIsLost)
    {
        BloodSim sim;
        FakeWorld world;
        Plane body = floorAt(0);
        body.mMoving = true;
        world.mPlanes.push_back(body);
        Misc::Rng::Generator prng(5);
        sim.burst(osg::Vec3f(0, 0, 30), osg::Vec3f(), 30, 0, 50, 30, prng);
        for (int i = 0; i < 60 * 3; ++i)
            sim.step(1.f / 60, world.rayCast());
        EXPECT_TRUE(sim.getDrops().empty());
        EXPECT_TRUE(sim.getStains().empty());
    }

    TEST(MWPhysicsBloodSimTest, dropsThatNeverLandAreDroppedInTime)
    {
        BloodSim sim;
        FakeWorld world;
        Misc::Rng::Generator prng(9);
        sim.burst(osg::Vec3f(), osg::Vec3f(), 20, 2, 100, 20, prng);
        for (int i = 0; i < 60 * 7; ++i)
            sim.step(1.f / 60, world.rayCast());
        EXPECT_TRUE(sim.getDrops().empty());
        EXPECT_TRUE(sim.getStains().empty());
    }

    TEST(MWPhysicsBloodSimTest, longFramesAreTakenInSteps)
    {
        BloodSim sim;
        FakeWorld world;
        Misc::Rng::Generator prng(11);
        sim.burst(osg::Vec3f(0, 0, 1000), osg::Vec3f(), 1, 0, 0, 1, prng);
        sim.step(0.1f, world.rayCast());
        EXPECT_EQ(world.mRays, 6);
        // Not caught up past a quarter of a second.
        sim.step(10.f, world.rayCast());
        EXPECT_EQ(world.mRays, 6 + 15);
    }

    TEST(MWPhysicsBloodSimTest, marksOnOneAnotherRunTogether)
    {
        BloodSim sim;
        sim.addStain(osg::Vec3f(0, 0, 0), osg::Vec3f(0, 0, 1), 4, false, false);
        sim.addStain(osg::Vec3f(1, 0, 0), osg::Vec3f(0, 0, 1), 4, false, false);
        ASSERT_EQ(sim.getStains().size(), 1u);
        const MWPhysics::BloodStain& pool = sim.getStains().front();
        EXPECT_FLOAT_EQ(pool.mVolume, 8.f);
        EXPECT_NEAR(pool.mPosition.x(), 0.5f, 1e-4f);
        EXPECT_FLOAT_EQ(pool.mRadius, sim.stainRadius(8.f));
    }

    TEST(MWPhysicsBloodSimTest, marksApartOrOnOtherSurfacesStayApart)
    {
        BloodSim sim;
        sim.addStain(osg::Vec3f(0, 0, 0), osg::Vec3f(0, 0, 1), 1, false, false);
        sim.addStain(osg::Vec3f(50, 0, 0), osg::Vec3f(0, 0, 1), 1, false, false);
        // Right there, but on a wall.
        sim.addStain(osg::Vec3f(0, 0, 0), osg::Vec3f(-1, 0, 0), 1, false, false);
        // Above it, on a shelf.
        sim.addStain(osg::Vec3f(0, 0, 20), osg::Vec3f(0, 0, 1), 1, false, false);
        EXPECT_EQ(sim.getStains().size(), 4u);
    }

    TEST(MWPhysicsBloodSimTest, theOldestMarksGoFirst)
    {
        BloodSim::Settings settings;
        settings.mMaxStains = 3;
        BloodSim sim(settings);
        const std::size_t revision = sim.getStainRevision();
        for (int i = 0; i < 5; ++i)
            sim.addStain(osg::Vec3f(100.f * i, 0, 0), osg::Vec3f(0, 0, 1), 1, false, false);
        ASSERT_EQ(sim.getStains().size(), 3u);
        EXPECT_FLOAT_EQ(sim.getStains().front().mPosition.x(), 200.f);
        EXPECT_FLOAT_EQ(sim.getStains().back().mPosition.x(), 400.f);
        EXPECT_NE(sim.getStainRevision(), revision);
    }

    TEST(MWPhysicsBloodSimTest, stainSizeFollowsTheVolume)
    {
        BloodSim sim;
        EXPECT_LT(sim.stainRadius(1), sim.stainRadius(2));
        EXPECT_FLOAT_EQ(sim.stainRadius(0), 0.5f);
        EXPECT_FLOAT_EQ(sim.stainRadius(1e9f), sim.getSettings().mMaxStainRadius);
    }

    TEST(MWPhysicsBloodSimTest, tooMuchOnAWallRunsDownItAndPoolsAtTheFoot)
    {
        BloodSim sim(roomySettings());
        FakeWorld world;
        world.mPlanes.push_back(floorAt(0));
        world.mPlanes.push_back(wallAt(100));
        sim.addStain(osg::Vec3f(100, 0, 50), osg::Vec3f(-1, 0, 0), 6, false, true);
        ASSERT_EQ(sim.getRunners().size(), 1u);
        EXPECT_FLOAT_EQ(sim.getRunners().front().mVolume, 6.f - sim.getSettings().mWallHoldVolume);

        float lowest = 50;
        for (int i = 0; i < 60 * 10; ++i)
        {
            sim.step(1.f / 60, world.rayCast());
            for (const MWPhysics::BloodRunner& runner : sim.getRunners())
            {
                EXPECT_NEAR(runner.mPosition.x(), 100.f, 1e-3f); // on the wall
                EXPECT_LE(runner.mPosition.z(), lowest + 1e-3f); // only ever down
                lowest = runner.mPosition.z();
            }
        }
        EXPECT_TRUE(sim.getRunners().empty());
        EXPECT_NEAR(stainVolume(sim), 6.f, 1e-4f);

        // A trail down the wall, and the rest on the floor at its foot.
        int trail = 0;
        bool pool = false;
        for (const MWPhysics::BloodStain& stain : sim.getStains())
        {
            if (stain.mNormal == osg::Vec3f(-1, 0, 0))
            {
                ++trail;
                EXPECT_GE(stain.mPosition.z(), 0.f);
                EXPECT_LE(stain.mPosition.z(), 50.f + 1e-3f);
            }
            else if (stain.mNormal == osg::Vec3f(0, 0, 1))
            {
                pool = true;
                EXPECT_NEAR(stain.mPosition.z(), 0.f, 1e-3f);
                EXPECT_NEAR(stain.mPosition.x(), 100.f, 1.f);
            }
        }
        EXPECT_GT(trail, 3);
        EXPECT_TRUE(pool);
    }

    TEST(MWPhysicsBloodSimTest, aRunDripsOffWhereTheWallEnds)
    {
        BloodSim sim(roomySettings());
        FakeWorld world;
        world.mPlanes.push_back(floorAt(0));
        Plane wall = wallAt(100);
        wall.mMin.z() = 30; // a wall hanging over open space below
        world.mPlanes.push_back(wall);
        sim.addStain(osg::Vec3f(100, 0, 60), osg::Vec3f(-1, 0, 0), 8, false, true);
        ASSERT_EQ(sim.getRunners().size(), 1u);

        bool dripped = false;
        for (int i = 0; i < 60 * 10; ++i)
        {
            sim.step(1.f / 60, world.rayCast());
            dripped = dripped || !sim.getDrops().empty();
        }
        EXPECT_TRUE(dripped);
        EXPECT_TRUE(sim.getRunners().empty());
        EXPECT_TRUE(sim.getDrops().empty());
        EXPECT_NEAR(stainVolume(sim), 8.f, 1e-4f);
        bool onFloor = false;
        for (const MWPhysics::BloodStain& stain : sim.getStains())
            onFloor = onFloor || stain.mNormal == osg::Vec3f(0, 0, 1);
        EXPECT_TRUE(onFloor);
    }

    TEST(MWPhysicsBloodSimTest, aFewDropsOnAWallStayPut)
    {
        BloodSim sim;
        sim.addStain(osg::Vec3f(100, 0, 50), osg::Vec3f(-1, 0, 0), 1, false, true);
        EXPECT_TRUE(sim.getRunners().empty());
        sim.addStain(osg::Vec3f(100, 0, 50), osg::Vec3f(-1, 0, 0), 0.5f, false, true);
        EXPECT_TRUE(sim.getRunners().empty());
        // Enough of them, and it starts to run.
        sim.addStain(osg::Vec3f(100, 0, 50), osg::Vec3f(-1, 0, 0), 1, false, true);
        EXPECT_EQ(sim.getRunners().size(), 1u);
    }

    TEST(MWPhysicsBloodSimTest, chunksBounceBeforeTheySettle)
    {
        BloodSim sim(roomySettings());
        FakeWorld world;
        world.mPlanes.push_back(floorAt(0));
        Misc::Rng::Generator prng(13);
        sim.burst(osg::Vec3f(0, 0, 100), osg::Vec3f(0, 0, -400), 0, 1, 0, 0, prng);
        ASSERT_EQ(sim.getDrops().size(), 1u);
        const float volume = sim.getDrops().front().mVolume;
        bool bounced = false;
        for (int i = 0; i < 60 * 5; ++i)
        {
            sim.step(1.f / 60, world.rayCast());
            for (const MWPhysics::BloodDrop& drop : sim.getDrops())
                bounced = bounced || (drop.mBounces > 0 && drop.mVelocity.z() > 0);
        }
        EXPECT_TRUE(bounced);
        EXPECT_TRUE(sim.getDrops().empty());
        EXPECT_NEAR(stainVolume(sim), volume, 1e-4f);
        bool chunkMark = false;
        for (const MWPhysics::BloodStain& stain : sim.getStains())
            chunkMark = chunkMark || stain.mChunk;
        EXPECT_TRUE(chunkMark);
    }

    TEST(MWPhysicsBloodSimTest, clearTakesEverything)
    {
        BloodSim sim;
        Misc::Rng::Generator prng(17);
        sim.burst(osg::Vec3f(), osg::Vec3f(), 10, 1, 100, 10, prng);
        sim.addStain(osg::Vec3f(), osg::Vec3f(0, 0, 1), 1, false, false);
        const std::size_t revision = sim.getStainRevision();
        sim.clear();
        EXPECT_TRUE(sim.getDrops().empty());
        EXPECT_TRUE(sim.getRunners().empty());
        EXPECT_TRUE(sim.getStains().empty());
        EXPECT_NE(sim.getStainRevision(), revision);
    }
}
