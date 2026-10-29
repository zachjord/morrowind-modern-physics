#include "apps/openmw/mwmechanics/gore.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

#include <components/esm3/loadmgef.hpp>
#include <components/esm3/loadskil.hpp>
#include <components/esm3/loadspel.hpp>

#include <gtest/gtest.h>

namespace
{
    ESM::MagicEffect damageHealth()
    {
        ESM::MagicEffect effect;
        effect.blank();
        effect.mId = ESM::MagicEffect::DamageHealth;
        effect.mName = "Damage Health";
        effect.mData.mSchool = ESM::Skill::Destruction;
        effect.mIcon = "s\\tx_s_dmg_health.tga";
        effect.mData.mFlags = ESM::MagicEffect::Harmful | ESM::MagicEffect::CastSelf;
        return effect;
    }

    TEST(MWMechanicsGoreTest, burstHeadRecordsLookLikeDamageHealth)
    {
        MWWorld::ESMStore store;
        store.insertStatic(damageHealth());
        MWMechanics::addGoreRecords(store);
        store.setUp();

        const ESM::MagicEffect* effect = store.get<ESM::MagicEffect>().search(MWMechanics::sBurstHeadEffect);
        ASSERT_NE(effect, nullptr);
        EXPECT_EQ(effect->mName, "Burst Head");
        EXPECT_EQ(effect->mData.mSchool, ESM::Skill::Destruction);
        EXPECT_EQ(effect->mIcon.getOriginal(), damageHealth().mIcon.getOriginal());
        const int flags = effect->mData.mFlags;
        EXPECT_TRUE(flags & ESM::MagicEffect::Harmful);
        EXPECT_TRUE(flags & ESM::MagicEffect::NoMagnitude);
        EXPECT_TRUE(flags & ESM::MagicEffect::NoDuration);
        EXPECT_TRUE(flags & ESM::MagicEffect::CastTarget);
        EXPECT_FALSE(flags & ESM::MagicEffect::CastSelf);

        const ESM::Spell* spell = store.get<ESM::Spell>().search(MWMechanics::sBurstHeadSpell);
        ASSERT_NE(spell, nullptr);
        EXPECT_EQ(spell->mData.mType, ESM::Spell::ST_Spell);
        EXPECT_EQ(spell->mData.mFlags, 0);
        ASSERT_EQ(spell->mEffects.mList.size(), 1u);
        EXPECT_EQ(spell->mEffects.mList.front().mData.mEffectID, MWMechanics::sBurstHeadEffect);
        EXPECT_EQ(spell->mEffects.mList.front().mData.mRange, ESM::RT_Target);
    }

    TEST(MWMechanicsGoreTest, addingTheRecordsTwiceIsHarmless)
    {
        MWWorld::ESMStore store;
        store.insertStatic(damageHealth());
        MWMechanics::addGoreRecords(store);
        EXPECT_NO_THROW(MWMechanics::addGoreRecords(store));
    }

    TEST(MWMechanicsGoreTest, noRecordsWithoutDamageHealth)
    {
        MWWorld::ESMStore store;
        MWMechanics::addGoreRecords(store);
        store.setUp();
        EXPECT_EQ(store.get<ESM::MagicEffect>().search(MWMechanics::sBurstHeadEffect), nullptr);
        EXPECT_EQ(store.get<ESM::Spell>().search(MWMechanics::sBurstHeadSpell), nullptr);
    }
}
