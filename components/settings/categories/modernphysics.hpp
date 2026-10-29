#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_MODERNPHYSICS_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_MODERNPHYSICS_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

namespace Settings
{
    /// Switches for the "Morrowind - Modern Physics" fork's features.
    struct ModernPhysicsCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mRecoverableStuckProjectiles{ mIndex, "Modern Physics",
            "recoverable stuck projectiles" };
        // Impact sounds (sounds and their tuning: resources/vfs/modernphysics/impactsounds.yaml).
        SettingValue<bool> mImpactSounds{ mIndex, "Modern Physics", "impact sounds" };
        SettingValue<bool> mProjectileImpactSounds{ mIndex, "Modern Physics", "projectile impact sounds" };
        SettingValue<bool> mObjectImpactSounds{ mIndex, "Modern Physics", "object impact sounds" };
        SettingValue<bool> mMeleeWorldImpactSounds{ mIndex, "Modern Physics", "melee world impact sounds" };
        SettingValue<bool> mExtendedWaterSplashes{ mIndex, "Modern Physics", "extended water splashes" };
        SettingValue<bool> mImpactSoundDebug{ mIndex, "Modern Physics", "impact sound debug" };
        // Body parts destroyed for good (see apps/openmw/mwmechanics/gore.hpp).
        SettingValue<bool> mGore{ mIndex, "Modern Physics", "gore" };
        // The player knows the Burst Head spell.
        SettingValue<bool> mGoreSpell{ mIndex, "Modern Physics", "gore spell" };
        // How many droplets a burst throws (0: none simulated, only Morrowind's splash effect).
        SettingValue<int> mGoreBloodDroplets{ mIndex, "Modern Physics", "gore blood droplets",
            makeClampSanitizerInt(0, 600) };
        // How many blood marks are kept around, the oldest going first.
        SettingValue<int> mGoreBloodStains{ mIndex, "Modern Physics", "gore blood stains",
            makeClampSanitizerInt(0, 4000) };
    };
}

#endif
