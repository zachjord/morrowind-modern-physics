# Gore, dismemberment and blood: architecture notes

Status: Stage 1 (assessment) done, Stage 2 started: a head can be destroyed (console `ExplodeHead`) and the body
drops into the existing ragdoll. Blood is Morrowind's own splash effect for now. Nothing here has been tested in
game yet; see "Testing" at the end.

## 1. The existing ragdoll, as it bears on head destruction

Files: `apps/openmw/mwphysics/ragdoll.{hpp,cpp}`, `physicssystem.cpp`, `mwmechanics/character.cpp`,
`mwworld/worldimp.cpp` (`makeRagdollBody`).

**Lifecycle.** Health reaches 0 → `CharacterController::kill` → `playRandomDeath` → `startRagdoll` →
`World::makeRagdoll` → `World::makeRagdollBody` reads the bones' current world matrices from the `Animation` and
calls `PhysicsSystem::createRagdoll`. Each frame after that, `CharacterController::driveRagdoll` reads the bodies'
interpolated transforms and writes them to the skeleton with `Animation::setBoneWorldMatrix`, and stores them as
the saved pose (`CreatureStats::mRagdollPose`, subrecords `RAGB`/`RAGP`). After loading, `restoreRagdoll` poses
the skeleton from the saved pose and makes a new ragdoll from it, asleep.

**Humanoid body.** 11 capsules (`sPartDefs`): pelvis, chest (`Bip01 Spine1`, up to the neck), head, upper/lower
arms and legs, joined by `btConeTwistConstraint`s with joint friction (`JointFriction`, a `btActionInterface`).
The head is its own body (8 radius, 5 mass) hanging off the chest by a cone-twist at the head bone. `Bip01 Neck`
is not simulated; it rides on the chest. Bones without a body keep their local transform and follow their parent.
Parts don't collide with each other.

**Suitability.** Good. Because the head is a leaf part, removing it needs nothing new in the solver: the ragdoll is
simply built without the head body and its joint. The head bone then rides on the chest/neck like any
unsimulated bone, and since its meshes are gone it doesn't matter where it is. Mass drops by 5 of ~70 units,
which is about right for a lost head. There is no impulse in the explosion: the killing "kick"
(`CreatureStats::mDeathKick`) is cleared, so the body falls from its standing pose with only the velocity the
actor already had, which is the "natural collapse" asked for. A runtime "sever a joint on a live ragdoll" was
considered and rejected for now: `JointFriction::mPairs` is read on the physics thread without a lock, the held
object (grabbing) keeps references to parts, and `hasComeApart` compares against start distances per part.
Rebuilding the ragdoll from its current pose (what grabbing an old corpse already does) avoids all of that.

**Ownership and threading, as reviewed.**

- `Ragdoll::Part` is a `PtrHolder` owned by `shared_ptr` (the grab code holds parts); it owns its shape, motion
  state and body. `~Ragdoll` removes the action, joints and bodies from the world (each under the scheduler's
  collision-world lock) before any of them is destroyed, and removing a held body drops the hold pin
  (`PhysicsTaskScheduler::removeCollisionObject`). Members are destroyed joints first, then parts. This is sound.
- `RagdollMotionState` guards its transforms with a mutex; the physics thread writes, the main thread reads.
- **Fixed:** `Ragdoll::addJoint` read `btRigidBody::getWorldTransform()` of bodies already added to the world,
  without the lock, while the background physics thread (`async num threads = 1` by default) may be stepping
  them. It now reads the (locked) motion states, which hold the same transform.
- **Still open (low risk):** bodies are added to the world before their joints, each in its own lock, so a step
  can run with the parts unjointed for one tick. Fixing it properly needs a "add these bodies and constraints
  together" scheduler call; not done yet, to keep the existing behaviour untouched.
- `liftStrayParts` writes a motion state twice from the main thread to cancel interpolation; harmless.

## 2. Recommended architecture

One fact, many readers:

- **State:** `MWMechanics::CreatureStats::LostBodyPart` bits (only `Head` so far), saved as an optional `LOST`
  subrecord at the end of the actor's creature stats. Old saves load with nothing lost. Cleared on resurrection.
- **Rendering:** `NpcAnimation::isOnLostBodyPart` refuses parts that go on a lost body part (head: `PRT_Head`,
  `PRT_Hair`, which is also where helmets go), and `Animation::updateLostBodyParts()` applies a change. Because
  `addOrReplaceIndividualPart` is the only way NPC parts get attached, the head stays gone through equipment
  changes, rebuilds, cell reloads and saves, with no floating hair, eyes or helmet.
- **Physics:** `World::makeRagdollBody` passes the lost bones to `PhysicsSystem::createRagdoll`, and the humanoid
  `Ragdoll` leaves those parts (and anything hanging off them) out. The restored ragdoll after loading goes through
  the same path, so it is headless too.
- **Events:** `MWMechanics::explodeHead` (`mwmechanics/gore.{hpp,cpp}`) sets the state, updates the model, spawns
  the blood, and kills (health to 0, no kick). A body already dead gets its ragdoll rebuilt from its current pose.
- **Triggers:** console `ExplodeHead` (selected reference), Lua `world._explodeHead(actor)` /
  `world._hasLostHead(actor)` for scripted tests. A magic effect is the next step (see 4).
- **Setting:** `[Modern Physics] gore` (default on).

This keeps the stock death and ragdoll paths unchanged for anyone who never loses a head: the only change on that
path is an empty `lostBones` list.

## 3. Known obstacles and limits

- **Helmets/hoods** are hidden with the head but stay equipped and lootable. Eventually: drop them as a physical
  item at the head's position.
- **Skinned heads or necks**: vanilla heads and hair are rigid parts on the head bone, as are most mod heads.
  Body replacers whose skinned body mesh has vertices weighted to `Bip01 Head` would leave those vertices. The
  general fix (and the basis for limbs) is per-bone triangle removal in `SceneUtil::RigGeometry`: drop triangles
  whose dominant bone is lost, and cap the cut. Not done yet.
- **Creatures**: no part system; the same RigGeometry approach is needed. `explodeHead` refuses non-NPCs.
- **The stump** shows the top of the neck part (open or capped, depending on the mesh). Needs a wound cap mesh and
  a blood decal (Stage 3).
- **Save compatibility**: saves with a headless body carry a `LOST` subrecord that builds without this feature
  don't expect. Saves without one are unchanged.
- **No crime/attacker**: the dev command kills with no attacker (like `SetHealth 0`). The spell version must go
  through the normal damage path so murder, bounty and AI reactions work.
- **Corpse still playing its death animation** (ragdoll failed): its head goes, its pose stays.

## 4. Next steps (Stage 2 → 3)

1. Physical head fragments: a small pool of short-lived Bullet bodies (spheres/convex chunks) owned by a
   `GoreSystem` in mwphysics, rendered as instanced chunk meshes, never saved, capped in number; same collision
   group as ragdoll parts.
2. Our own blood particle emitter (droplets with Bullet ray casts each step, batched) replacing the vanilla splash.
3. Wound cap at `Bip01 Neck`.
4. A magic effect ("Burst Head" or on-target damage with a gore flag) castable as a spell.

## 5. Blood simulation: options and recommendation

The target is blood that sprays, sticks, runs and pools, in a forward OSG renderer with Bullet collision, at
Morrowind's scale (~70 units per metre), with only a few events on screen at a time.

| Technique | What it gives | Cost / fit here |
|---|---|---|
| Ballistic GPU or CPU particles | Spray and droplets | Cheap. Collision is the hard part: Bullet ray casts (batched, on the physics thread) for hundreds of droplets, or depth-buffer collision on the GPU (only what's on screen). |
| SPH / Position-Based Fluids (3D) | Real volume: splashes, merging | Thousands of particles per litre, neighbour search, and screen-space fluid rendering (depth smoothing + normals) on top. Feasible for one short burst; expensive to keep alive or to have everywhere. |
| Surface-adhering particles / "drip agents" | Runs down walls, rivulets | Cheap: a droplet stuck to a surface moves along the surface's tangent under gravity, losing volume into a trail. Needs the surface normal at its position (ray casts against collision). |
| Shallow water / thin film on a height grid | Spreading, pooling, flowing around obstacles on floors | A local 2D grid (say 64×64 cells over 2–3 m) per pool, heights from collision ray casts, simulated on the GPU in a ping-pong render-to-texture (works with GL 3.x, no compute shaders needed) or on the CPU at low res. |
| Decals / stain textures | Persistent wet stains | Projected decals with a wetness/height texture; the cheap, persistent end state of everything above. |

**Recommendation: a hybrid in layers, each a real simulation of what it models.**

1. *Spray:* ballistic droplets (CPU first; a few hundred per burst), collision by batched Bullet ray casts
   against static and dynamic geometry, so they hit walls, floors, furniture and other bodies.
2. *Contact:* a droplet that hits deposits its volume into the surface layer and may bounce/split a little.
3. *Surface layer:* on floors, local shallow-water grids that spread and pool and flow downhill around
   obstacles; on steep surfaces, drip agents that run down and leave trails. Both write a wetness/thickness
   texture that a decal shader renders (darkening, gloss, thickness).
4. *Drying and persistence:* layers settle and freeze into plain decals after some seconds; those (capped per
   cell, oldest dropped) are what gets saved.
5. *Later:* a short-lived PBF burst for the explosion itself, if the layered approach doesn't look wet enough.
   Moving objects: stains on ragdolls and items live in the object's space (triplanar mapped), so they move with it.

Quality settings would scale droplet counts, grid resolution, the number of live grids and decal caps.
Integration points: a `BloodSystem` with its own simulation step on the physics thread (reading collision
under the scheduler lock), and a render-side manager that owns the decal and grid textures.

## Testing

What has been checked so far: each changed source file compiles on its own without warnings (GCC 13, Ubuntu
24.04, system OSG/Bullet). The full engine has **not** been linked, and the unit tests (ESM save/load round trip
of the new subrecord, in `apps/components_tests/esm3/testsaveload.cpp`) have **not** been run. Nothing has been run
in game. To check locally, build `openmw` and `components-tests`, run
`components-tests --gtest_filter='*LostBodyParts*'`, then:

1. Open the console, click an NPC, type `ExplodeHead`. Expect: head, hair and any helmet vanish, blood splashes
   at the head, a crunch, and the body drops limp straight down (no push), settling without jitter.
2. Repeat on several NPCs, including one already lying dead (it should lose its head where it lies) and one
   being dragged (it should drop and be grabbable again).
3. Save, reload: headless bodies stay headless and lie where they were.
4. Change cells and come back; open the body's inventory and loot the helmet.
5. `Resurrect` a headless NPC: the head is back.
6. Run it 50+ times in one place (e.g. a scripted loop) and watch `openmw.log` and memory for leaks or crashes.
