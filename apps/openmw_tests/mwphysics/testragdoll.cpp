#include "apps/openmw/mwphysics/ragdoll.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    using MWPhysics::Ragdoll;

    bool hasBone(const std::vector<Ragdoll::HumanoidPart>& parts, const std::string& bone)
    {
        return std::any_of(parts.begin(), parts.end(), [&](const Ragdoll::HumanoidPart& p) { return p.mBone == bone; });
    }

    int indexOf(const std::vector<Ragdoll::HumanoidPart>& parts, const std::string& bone)
    {
        for (std::size_t i = 0; i < parts.size(); ++i)
            if (parts[i].mBone == bone)
                return static_cast<int>(i);
        return -1;
    }

    // Each part's parent comes before it (parents first), only the first has none.
    void expectWellFormed(const std::vector<Ragdoll::HumanoidPart>& parts)
    {
        ASSERT_FALSE(parts.empty());
        EXPECT_EQ(parts.front().mBone, "Bip01 Pelvis");
        EXPECT_EQ(parts.front().mParent, -1);
        for (std::size_t i = 1; i < parts.size(); ++i)
        {
            EXPECT_GE(parts[i].mParent, 0) << parts[i].mBone;
            EXPECT_LT(parts[i].mParent, static_cast<int>(i)) << parts[i].mBone;
        }
    }

    TEST(MWPhysicsRagdollTest, wholeBodyHasAllElevenParts)
    {
        const auto parts = Ragdoll::getHumanoidParts({});
        EXPECT_EQ(parts.size(), 11u);
        expectWellFormed(parts);
        EXPECT_EQ(parts[indexOf(parts, "Bip01 Head")].mParent, indexOf(parts, "Bip01 Spine1"));
        for (const Ragdoll::HumanoidPart& part : parts)
            EXPECT_NE(std::find(Ragdoll::getRequiredBones().begin(), Ragdoll::getRequiredBones().end(), part.mBone),
                Ragdoll::getRequiredBones().end())
                << part.mBone;
    }

    TEST(MWPhysicsRagdollTest, headlessBodyHasNoHeadAndTheRestJoinedAsBefore)
    {
        const auto whole = Ragdoll::getHumanoidParts({});
        const auto parts = Ragdoll::getHumanoidParts({ "Bip01 Head" });
        EXPECT_EQ(parts.size(), 10u);
        EXPECT_FALSE(hasBone(parts, "Bip01 Head"));
        expectWellFormed(parts);
        // Every other part still hangs off the same bone as in the whole body.
        for (const Ragdoll::HumanoidPart& part : parts)
        {
            const Ragdoll::HumanoidPart& before = whole[indexOf(whole, part.mBone)];
            EXPECT_EQ(part.mDef, before.mDef);
            if (before.mParent >= 0)
                EXPECT_EQ(parts[part.mParent].mBone, whole[before.mParent].mBone) << part.mBone;
        }
    }

    TEST(MWPhysicsRagdollTest, losingALimbTakesWhatHangsOffIt)
    {
        const auto parts = Ragdoll::getHumanoidParts({ "Bip01 L UpperArm" });
        EXPECT_EQ(parts.size(), 9u);
        EXPECT_FALSE(hasBone(parts, "Bip01 L UpperArm"));
        EXPECT_FALSE(hasBone(parts, "Bip01 L Forearm"));
        EXPECT_TRUE(hasBone(parts, "Bip01 R Forearm"));
        expectWellFormed(parts);
    }

    TEST(MWPhysicsRagdollTest, theMainPartAndUnknownBonesAreNeverLeftOut)
    {
        EXPECT_EQ(Ragdoll::getHumanoidParts({ "Bip01 Pelvis" }).size(), 11u);
        EXPECT_EQ(Ragdoll::getHumanoidParts({ "Bip01 Tail", "" }).size(), 11u);
    }
}
