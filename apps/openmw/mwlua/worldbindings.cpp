#include "worldbindings.hpp"

#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadench.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadlevlist.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>
#include <components/lua/luastate.hpp>
#include <components/misc/finitevalues.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/gore.hpp"
#include "../mwmechanics/stuckprojectiles.hpp"
#include "../mwrender/animation.hpp"

#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/impactsounds.hpp"
#include "../mwworld/materials.hpp"
#include "../mwworld/store.hpp"
#include "../mwworld/worldmodel.hpp"

#include "luamanagerimp.hpp"

#include "animationbindings.hpp"
#include "context.hpp"
#include "corebindings.hpp"
#include "mwscriptbindings.hpp"

namespace MWLua
{
    struct CellsStore
    {
    };
}

namespace sol
{
    template <>
    struct is_automagical<MWLua::CellsStore> : std::false_type
    {
    };
}

namespace MWLua
{

    static void checkGameInitialized(LuaUtil::LuaState* lua)
    {
        if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_NoGame)
            throw std::runtime_error(
                "This function cannot be used until the game is fully initialized.\n" + lua->debugTraceback());
    }

    static void addWorldTimeBindings(sol::table& api, const Context& context)
    {
        using Misc::FiniteFloat;

        Misc::NotNullPtr<MWBase::World> world = MWBase::Environment::get().getWorld();
        MWWorld::DateTimeManager* timeManager = world->getTimeManager();

        api["advanceTime"] = [context, world](const FiniteFloat hours) {
            if (hours <= 0.0f)
                throw std::runtime_error("Time may only be advanced forward");

            context.mLuaManager->addAction([world, hours] {
                world->advanceTime(hours);
                MWBase::Environment::get().getMechanicsManager()->fastForwardAi();
            });
        };

        api["setGameTimeScale"] = [timeManager](const FiniteFloat scale) { timeManager->setGameTimeScale(scale); };
        api["setSimulationTimeScale"] = [context, timeManager](const FiniteFloat scale) {
            context.mLuaManager->addAction([scale, timeManager] { timeManager->setSimulationTimeScale(scale); });
        };

        api["pause"]
            = [timeManager](sol::optional<std::string_view> tag) { timeManager->pause(tag.value_or("paused")); };
        api["unpause"]
            = [timeManager](sol::optional<std::string_view> tag) { timeManager->unpause(tag.value_or("paused")); };
        api["getPausedTags"] = [timeManager](sol::this_state lua) {
            sol::table res(lua, sol::create);
            for (const std::string& tag : timeManager->getPausedTags())
                res[tag] = tag;
            return res;
        };
    }

    static void addCellGetters(sol::table& api, const Context& context)
    {
        api["getCellByName"] = [](std::string_view name) {
            return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(name, /*forceLoad=*/false) };
        };
        api["getCellById"] = [](std::string_view stringId) {
            return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                ESM::RefId::deserializeText(stringId), /*forceLoad=*/false) };
        };
        api["getExteriorCell"] = [](int x, int y, sol::object cellOrName) {
            ESM::RefId worldspace;
            if (cellOrName.is<GCell>())
                worldspace = cellOrName.as<GCell>().mStore->getCell()->getWorldSpace();
            else if (cellOrName.is<std::string_view>() && !cellOrName.as<std::string_view>().empty())
                worldspace = MWBase::Environment::get()
                                 .getWorldModel()
                                 ->getCell(cellOrName.as<std::string_view>())
                                 .getCell()
                                 ->getWorldSpace();
            else
                worldspace = ESM::Cell::sDefaultWorldspaceId;
            return GCell{ &MWBase::Environment::get().getWorldModel()->getExterior(
                ESM::ExteriorCellLocation(x, y, worldspace), /*forceLoad=*/false) };
        };

        const MWWorld::Store<ESM::Cell>* cells3Store = &MWBase::Environment::get().getESMStore()->get<ESM::Cell>();
        const MWWorld::Store<ESM4::Cell>* cells4Store = &MWBase::Environment::get().getESMStore()->get<ESM4::Cell>();
        auto view = context.sol();
        sol::usertype<CellsStore> cells = view.new_usertype<CellsStore>("Cells");
        cells[sol::meta_function::length]
            = [cells3Store, cells4Store](const CellsStore&) { return cells3Store->getSize() + cells4Store->getSize(); };
        cells[sol::meta_function::index]
            = [cells3Store, cells4Store](const CellsStore&, size_t index) -> sol::optional<GCell> {
            if (index > cells3Store->getSize() + cells4Store->getSize() || index == 0)
                return sol::nullopt;

            index--; // Translate from Lua's 1-based indexing.
            if (index < cells3Store->getSize())
            {
                const ESM::Cell* cellRecord = cells3Store->at(index);
                return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                    cellRecord->mId, /*forceLoad=*/false) };
            }
            else
            {
                const ESM4::Cell* cellRecord = cells4Store->at(index - cells3Store->getSize());
                return GCell{ &MWBase::Environment::get().getWorldModel()->getCell(
                    cellRecord->mId, /*forceLoad=*/false) };
            }
        };
        cells[sol::meta_function::pairs] = view["ipairsForArray"].template get<sol::function>();
        cells[sol::meta_function::ipairs] = view["ipairsForArray"].template get<sol::function>();
        api["cells"] = CellsStore{};
    }

    sol::table initWorldPackage(const Context& context)
    {
        sol::table api(context.mLua->unsafeState(), sol::create);

        addCoreTimeBindings(api, context);
        addWorldTimeBindings(api, context);
        addCellGetters(api, context);
        api["mwscript"] = initMWScriptBindings(context);

        ObjectLists* objectLists = context.mObjectLists;
        api["activeActors"] = GObjectList{ objectLists->getActorsInScene() };
        api["players"] = GObjectList{ objectLists->getPlayers() };

        api["getObjectsInRange"] = [](osg::Vec3f position, Misc::FiniteFloat range) -> GObjectList {
            std::vector<MWWorld::Ptr> objects;
            MWBase::Environment::get().getMechanicsManager()->getObjectsInRange(position, range, objects);
            GObjectList objList;
            objList.mIds = std::make_shared<std::vector<ObjectId>>();
            objList.mIds->reserve(objects.size());
            for (const auto& object : objects)
                objList.mIds->push_back(getId(object));
            return objList;
        };
        api["createObject"] = [lua = context.mLua](std::string_view recordId, sol::optional<int> count) -> GObject {
            checkGameInitialized(lua);
            MWWorld::ManualRef mref(*MWBase::Environment::get().getESMStore(), ESM::RefId::deserializeText(recordId));
            const MWWorld::Ptr& ptr = mref.getPtr();
            ptr.getRefData().disable();
            MWWorld::CellStore& cell = MWBase::Environment::get().getWorldModel()->getDraftCell();
            MWWorld::Ptr newPtr = ptr.getClass().copyToCell(ptr, cell, count.value_or(1));
            return GObject(newPtr);
        };
        api["getObjectByFormId"] = [](std::string_view formIdStr) -> GObject {
            ESM::RefId refId = ESM::RefId::deserializeText(formIdStr);
            if (!refId.is<ESM::FormId>())
                throw std::runtime_error("FormId expected, got " + std::string(formIdStr) + "; use core.getFormId");
            return GObject(*refId.getIf<ESM::FormId>());
        };
        api["getObjectsByRecordId"]
            = [](sol::this_state thisState, std::string_view serializedId, std::optional<std::string_view> worldSpace,
                  std::optional<bool> loadedOnly) -> sol::table {
            ESM::RefId id = ESM::RefId::deserializeText(serializedId);
            sol::table objects(thisState, sol::create);
            if (id.empty())
                return objects;
            ESM::RefId worldSpaceId;
            if (worldSpace)
            {
                worldSpaceId = ESM::RefId::deserializeText(*worldSpace);
                if (worldSpaceId.empty())
                    return objects;
            }
            const bool searchUnloaded = !loadedOnly.value_or(false);
            std::vector<MWWorld::Ptr> ptrs
                = MWBase::Environment::get().getWorldModel()->getAll(id, worldSpaceId, searchUnloaded);
            for (const MWWorld::Ptr& ptr : ptrs)
                objects.add(GObject(ptr));
            return objects;
        };

        // Creates a new record in the world database.
        api["createRecord"] = sol::overload(
            [lua = context.mLua](const ESM::Activator& activator) -> const ESM::Activator* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(activator);
            },
            [lua = context.mLua](const ESM::Armor& armor) -> const ESM::Armor* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(armor);
            },
            [lua = context.mLua](const ESM::Clothing& clothing) -> const ESM::Clothing* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(clothing);
            },
            [lua = context.mLua](const ESM::CreatureLevList& list) -> const ESM::CreatureLevList* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(list);
            },
            [lua = context.mLua](const ESM::Book& book) -> const ESM::Book* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(book);
            },
            [lua = context.mLua](const ESM::Enchantment& enchantment) -> const ESM::Enchantment* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(enchantment);
            },
            [lua = context.mLua](const ESM::Ingredient& ingred) -> const ESM::Ingredient* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(ingred);
            },
            [lua = context.mLua](const ESM::ItemLevList& list) -> const ESM::ItemLevList* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(list);
            },
            [lua = context.mLua](const ESM::Miscellaneous& misc) -> const ESM::Miscellaneous* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(misc);
            },
            [lua = context.mLua](const ESM::Potion& potion) -> const ESM::Potion* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(potion);
            },
            [lua = context.mLua](const ESM::Probe& probe) -> const ESM::Probe* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(probe);
            },
            [lua = context.mLua](const ESM::Spell& spell) -> const ESM::Spell* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(spell);
            },
            [lua = context.mLua](const ESM::Static& stat) -> const ESM::Static* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(stat);
            },
            [lua = context.mLua](const ESM::NPC& npc) -> const ESM::NPC* {
                checkGameInitialized(lua);
                if (npc.mId.empty())
                    return MWBase::Environment::get().getESMStore()->insert(npc);
                ESM::NPC copy = npc;
                copy.mId = {};
                return MWBase::Environment::get().getESMStore()->insert(copy);
            },
            [lua = context.mLua](const ESM::Creature& crea) -> const ESM::Creature* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(crea);
            },
            [lua = context.mLua](const ESM::Container& cont) -> const ESM::Container* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(cont);
            },
            [lua = context.mLua](const ESM::Door& cont) -> const ESM::Door* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(cont);
            },
            [lua = context.mLua](const ESM::Weapon& weapon) -> const ESM::Weapon* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(weapon);
            },
            [lua = context.mLua](const ESM::Light& light) -> const ESM::Light* {
                checkGameInitialized(lua);
                return MWBase::Environment::get().getESMStore()->insert(light);
            });

        // Physics fork: blow simulated objects away from a spell explosion (called by the built-in spellcasting).
        api["_pushObjectsFromExplosion"] = [context](const osg::Vec3f& position, const Misc::FiniteFloat radius) {
            context.mLuaManager->addAction(
                [position, radius = static_cast<float>(radius)] {
                    MWBase::Environment::get().getWorld()->pushObjectsFromExplosion(position, radius);
                },
                "_pushObjectsFromExplosion");
        };

        // Physics fork: carrying and ragdolls, for scripted tests (the player does this with Activate/Attack).
        // The queries read physics state: call them with "lua num threads = 0".
        api["_grabObject"] = [context](const GObject& object) {
            context.mLuaManager->addAction(
                [object] { MWBase::Environment::get().getWorld()->grabObject(object.ptr()); }, "_grabObject");
        };
        api["_releaseGrabbedObject"] = [context](bool throwObject) {
            context.mLuaManager->addAction(
                [throwObject] { MWBase::Environment::get().getWorld()->releaseGrabbedObject(throwObject); },
                "_releaseGrabbedObject");
        };
        api["_isGrabbingObject"] = []() { return MWBase::Environment::get().getWorld()->isGrabbingObject(); };
        api["_ragdollBoneCount"] = [](const GObject& actor) {
            return MWBase::Environment::get().getWorld()->getRagdollBonePoses(actor.ptr()).size();
        };
        // Where the ragdoll's main part (the pelvis, say: bones are parents first) is; nil without a ragdoll.
        api["_ragdollCenter"] = [](const GObject& actor) -> sol::optional<osg::Vec3f> {
            const auto poses = MWBase::Environment::get().getWorld()->getRagdollBonePoses(actor.ptr());
            if (poses.empty())
                return sol::nullopt;
            return osg::Vec3f(poses.front().second.getTrans());
        };
        // Gore: blowing a person's head apart (as the console's ExplodeHead), and whether it is gone.
        api["_explodeHead"] = [context](const GObject& actor) {
            context.mLuaManager->addAction([actor] { MWMechanics::explodeHead(actor.ptr()); }, "_explodeHead");
        };
        api["_hasLostHead"] = [](const GObject& actor) {
            const MWWorld::Ptr ptr = actor.ptr();
            return ptr.getClass().isActor()
                && ptr.getClass().getCreatureStats(ptr).hasLostBodyPart(MWMechanics::CreatureStats::LostBodyPart_Head);
        };
        // Recoverable stuck projectiles: what a stored projectile hit does (without the hit and its chance), and how
        // many are shown in / embedded in an actor.
        api["_embedProjectile"] = [context](const GObject& actor, std::string_view ammo, const osg::Vec3f& hitPosition,
                                      const osg::Vec3f& direction) {
            const ESM::RefId ammoId = ESM::RefId::deserializeText(ammo);
            context.mLuaManager->addAction(
                [actor, ammoId, hitPosition, direction] {
                    MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), ammoId);
                    MWMechanics::embedProjectile(actor.ptr(), ammoId);
                    MWMechanics::showEmbeddedProjectile(actor.ptr(), ref.getPtr(), hitPosition, direction);
                },
                "_embedProjectile");
        };
        // Fire a projectile (an arrow, say, as if thrown: no bow) from a point along a direction (normalized).
        api["_shootProjectile"] = [context](const GObject& shooter, std::string_view ammo, const osg::Vec3f& from,
                                      const osg::Vec3f& direction, float speed) {
            const ESM::RefId ammoId = ESM::RefId::deserializeText(ammo);
            context.mLuaManager->addAction(
                [shooter, ammoId, from, direction, speed] {
                    MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), ammoId);
                    MWWorld::Ptr actor = shooter.ptr();
                    MWWorld::Ptr projectile = ref.getPtr();
                    MWWorld::Ptr bow = projectile;
                    osg::Quat orient;
                    orient.makeRotate(osg::Vec3f(0, 1, 0), direction);
                    MWBase::Environment::get().getWorld()->launchProjectile(
                        actor, projectile, from, orient, bow, speed, 1.f, 1.f);
                },
                "_shootProjectile");
        };
        // What an object is made of, as impact sounds and arrows judge it ("wood", "metal"...): items by type and
        // name, anything else as a surface by its model.
        api["_material"] = [](const GObject& object) {
            const MWWorld::Ptr ptr = object.ptr();
            if (ptr.getClass().isItem(ptr))
                return std::string(MWWorld::getMaterialName(MWWorld::classifyItem(ptr)));
            return std::string(MWWorld::getMaterialName(
                MWWorld::classifySurface({}, ptr.getClass().getCorrectedModel(ptr), MWWorld::Material::Unknown)));
        };
        // Impact sounds: the last impacts and what was done about them, oldest first.
        api["_impactLog"] = []() {
            const auto& log = MWWorld::ImpactSounds::get().getLog();
            return std::vector<std::string>(log.begin(), log.end());
        };
        api["_clearImpactLog"] = []() { MWWorld::ImpactSounds::get().clearLog(); };
        // What a melee swing that hits no one does to the world around it (knocks loose things, or sounds where it
        // meets the world), as if swung with this weapon record (none: a fist), at strength (0 to 1).
        api["_meleeSwing"] = [context](const GObject& attacker, sol::optional<std::string_view> weapon, float strength) {
            const ESM::RefId weaponId = weapon ? ESM::RefId::deserializeText(*weapon) : ESM::RefId();
            context.mLuaManager->addAction(
                [attacker, weaponId, strength] {
                    if (weaponId.empty())
                    {
                        MWBase::Environment::get().getWorld()->knockObjectsWithMeleeAttack(
                            attacker.ptr(), MWWorld::Ptr(), ESM::Weapon::AT_Chop, strength);
                        return;
                    }
                    MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), weaponId);
                    MWBase::Environment::get().getWorld()->knockObjectsWithMeleeAttack(
                        attacker.ptr(), ref.getPtr(), ESM::Weapon::AT_Chop, strength);
                },
                "_meleeSwing");
        };
        api["_stuckProjectiles"] = [](const GObject& actor) {
            MWMechanics::CreatureStats& stats = actor.ptr().getClass().getCreatureStats(actor.ptr());
            int embedded = 0;
            for (const auto& [ammo, count] : stats.getEmbeddedAmmo())
                embedded += count;
            const MWRender::Animation* animation = MWBase::Environment::get().getWorld()->getAnimation(actor.ptr());
            return std::make_tuple(static_cast<int>(stats.getStuckProjectiles().size()), embedded,
                animation ? static_cast<int>(animation->getStuckProjectileCount()) : -1);
        };

        api["_runStandardActivationAction"] = [context](const GObject& object, const GObject& actor) {
            if (!object.ptr().getRefData().activate())
                return;
            context.mLuaManager->addAction(
                [object, actor] {
                    const MWWorld::Ptr& objPtr = object.ptr();
                    const MWWorld::Ptr& actorPtr = actor.ptr();
                    objPtr.getClass().activate(objPtr, actorPtr)->execute(actorPtr);
                },
                "_runStandardActivationAction");
        };
        api["_runStandardUseAction"] = [context](const GObject& object, const GObject& actor, bool force) {
            context.mLuaManager->addAction(
                [object, actor, force] {
                    const MWWorld::Ptr& actorPtr = actor.ptr();
                    const MWWorld::Ptr& objectPtr = object.ptr();
                    if (actorPtr == MWBase::Environment::get().getWorld()->getPlayerPtr())
                        MWBase::Environment::get().getWindowManager()->useItem(objectPtr, force);
                    else
                    {
                        std::unique_ptr<MWWorld::Action> action = objectPtr.getClass().use(objectPtr, force);
                        action->execute(actorPtr, true);
                    }
                },
                "_runStandardUseAction");
        };

        api["vfx"] = initWorldVfxBindings(context);

        return LuaUtil::makeReadOnly(api);
    }
}
