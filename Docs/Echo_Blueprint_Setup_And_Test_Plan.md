# Echo System — Blueprint/Editor Setup & Test Plan

Covers everything implemented this session: the momentum horde model, melee grab/bite/pull,
the TurnTowardStimulus orient task, the debug map overlay (with click-to-emit sound), and the
new Project Settings. Source compiles to safe defaults, but several things need wiring in-editor
before the behaviour is visible.

---

## PART 1 — Editor / Blueprint changes required

### 0. Compile first
- Build the C++ (new files: `ATR_EchoMeleeTask`, `ATR_EchoOrientTask`; new combat hooks on
  `AATR_ActiveEcho`; Slate enabled in `AllThatRemains.Build.cs`).
- After compile, the two new StateTree tasks appear in the StateTree node picker.

### 1. StateTree asset `BP_ATR_EchoStateTree`  (REQUIRED — attack & orient do nothing without this)

**Attack state** (`StateTreeState_3`, "Attack"):
1. Keep its existing enter conditions (Intent == Attack **AND** ObjectIsValid(ConfirmedVisibleActor)).
2. Keep the existing `Echo Move Request` task (this keeps the echo closing on the target — momentum
   is preserved).
3. **Add** task `Echo Melee (Grab/Bite/Pull)` to the SAME state, after the move task.
4. Bind the melee task's **Target** pin to the Echo Intent evaluator output **Confirmed Visible Actor**
   (same source the ObjectIsValid condition already uses).

**TurnTowardStimulus state** (`StateTreeState_13`, "TurnTowardStimulus"):
1. **Remove** the `Echo Move Request` task from this state (it only ever fails here).
2. **Add** task `Echo Orient (Turn Toward)`.
3. Bind the orient task's **TargetLocation** pin to the Echo Intent evaluator output **Target Location**.
4. Set the state's transition trigger to **On Tick → Root** (the same pattern the Idle state uses),
   so the tree re-evaluates each tick while the echo turns.

> Note: the earlier review found the `ReturnToIdle` intent has no dedicated state — it correctly
> falls through to the conditionless `Idle` state. No action needed unless you want distinct
> wind-down behaviour.

### 2. `AATR_ActiveEcho` Blueprint subclass  (REQUIRED for real combat effects)

The melee task calls three `BlueprintNativeEvent` hooks. C++ defaults are safe stubs (grab/bite
"succeed", pull is a no-op) so the flow runs, but nothing is damaged/animated until you override them
in your BP_ActiveEcho (the class assigned in Project Settings > Echo|Classes > Active Echo Class):

- **Try Grab Target (Target) -> bool** — play grab montage / attach / set anim flag. Return true if
  the grab "takes" (the task then holds it, pulls, and bites). Return false to keep retrying.
- **Try Bite Target (Target) -> bool** — apply damage to the target, play bite montage. Return true
  if it landed (used only for logging today).
- **Pull Target (Target, Strength)** — pull the player toward the echo (root motion / physics
  constraint / movement input). `Strength` comes from `Echo|Combat.MeleePullStrength`.

### 3. Player pawn — perception & team (REQUIRED for echoes to detect the player)

- The echo AI perception detects **enemies and neutrals** (not friendlies). The echo's team is
  `TeamNumber = 2` (`AATR_ActiveEcho`, `IGenericTeamAgentInterface`).
- The **player pawn/controller must implement `IGenericTeamAgentInterface`** and return a team id that
  the echo treats as hostile (i.e. NOT team 2 friendly). If sight never triggers, this is the usual
  cause.
- Confirm the player is within `Echo|Sight.ActiveSightRadius` and inside the sight cone to be seen.

### 4. Drive the horde from real game sounds (gameplay wiring, optional but expected)

- Real noises (gunshots, sprint footsteps, doors) should call
  `UATR_EchoSubsystem::EmitWorldStimulus(FATR_StimulusEvent)` — it is `BlueprintCallable`.
- Fill `Type` (Noise/Combat/etc.), `Location`, `Strength` (0..1), `Radius` (cm). This kicks nearby
  echoes into movement toward the source, which seeds horde momentum.
- The debug map's click-to-emit does exactly this for testing; production sounds need their own calls.

### 5. Obstacle tags (existing feature — tag breakables)

- Tag breakable obstacle actors so the AI can classify a blocked path:
  `Echo.Obstacle.Door`, `Echo.Obstacle.Window`, `Echo.Obstacle.Fence`.

### 6. Project Settings > AllThatRemains > Echo Horde (tuning only)

New categories to review (read once at Initialize — **restart PIE after changing**):
- **Echo|HordeMomentum** — `bEnableHordeMomentum`, `MomentumBuildRate`, `MomentumDecayPerSecond`,
  `MomentumPersistence`, `MomentumMoverSpeedThreshold`, `MomentumRefMoverCount`,
  `MomentumAlignThreshold`, `MomentumMaxStrength`, `SoundImpulseRadius`, `SoundImpulseSpeed`,
  `SoundImpulseStrengthScale`.
- **Echo|HordeShaping** — separation (`bEnableHordeSeparation`, `HordeSeparationRadius`,
  `HordeSeparationStrength`, `HordeApproachJitterDegrees`, `HordeSeparationMaxNeighbors`) and
  detachment (`bEnableHordeDetachment`, `DetachEdgeNeighborCount`, `DetachBackDot`,
  `DetachChance{Edge,Back,Random}PerSec`, `DetachDriftSpeed`).
- **Echo|Agitation** — `bEnableMomentumDiffusion`, `MomentumDiffusionRate`,
  `HordeDirectionJitterDegrees`, `MomentumCellSize`.
- **Echo|Combat** — `bEnableMeleeAttack`, `MeleeAttackRange`, `GrabRange`, `GrabCooldownSeconds`,
  `GrabReleaseMultiplier`, `BiteRange`, `BiteCooldownSeconds`, `MeleePullStrength`,
  `OrientTurnRateDegPerSec`.

### 7. Debug map (already wired — no BP work)

- Open with console `ATR.EchoDebugMap`, the **F8** key, or the on-screen toolbar.
- Non-shipping builds only. If F8 collides with another binding, use the console command.

---

## PART 2 — Detailed test plan

Logging: set `LogATR_EchoAI` to `Verbose` (or `VeryVerbose` for grab-miss spam) via
`Log LogATR_EchoAI Verbose` in the console. The debug map is your primary observation tool.

### A. Build / smoke
- **T1 Compile & PIE** — Project compiles; entering PIE spawns the horde with no errors/asserts.
  Open the debug map (F8). Echoes appear as dots; players appear as cyan triangles.

### B. Debug map overlay
- **T2 Open/close** — `ATR.EchoDebugMap` toggles; F8 toggles; Close button & Esc dismiss it.
- **T3 Pan/zoom** — mouse wheel zooms about the cursor; left-drag pans; Focus frames the echo
  cluster; Reset frames the whole world.
- **T4 Layers** — toggle Echoes / Intent / Momentum / Abstract / Arrows / Flow / Move Dir /
  Targets / Players; each shows/hides correctly; legend updates.
- **T5 Dot scaling** — zoom in: echo dots grow; zoom out: they shrink to a floor size.

### C. Click-to-emit sound  (this is your main horde-test tool)
- **T6 Emit basic** — Turn ON **Emit**. Set Type=Noise, Str=1.0, Rad=4000. Left-click an empty area
  near echoes. Expect: a fading ring at the click; nearby echoes start moving toward the click point;
  a "Momentum cells" count rises on the HUD. Right-drag still pans while Emit is on.
- **T7 Strength/radius** — Lower Str to 0.2 and Rad to 1000: fewer/closer echoes react. Raise Str to
  3 and Rad to 8000: many react. Confirm the spin boxes change behaviour.
- **T8 Type** — switch Type to Combat; confirm it still emits (Combat strength is clamped by
  `CombatAgitationAmount` on the abstract tier, but the movement kick still fires).

### D. Momentum model
- **T9 Build from movement** — With Momentum + Arrows layers on, emit a sound. Expect a momentum
  "blob" to form where echoes move, with cyan flow arrows pointing along their travel. Turn on **Flow**
  for the smooth field.
- **T10 Diffusion** — Watch the momentum heat spread outward over ~1–2 s into neighbouring cells
  (not a hard single-cell stamp). Toggle `bEnableMomentumDiffusion` OFF (restart PIE) → it should
  stay a hard stamp. Turn back ON.
- **T11 Persistence / peter-out** — Emit one strong sound to build a big horde, then stop. A large
  coherent horde keeps moving for a while then dissipates; a tiny one dies fast. Raise
  `MomentumPersistence` → hordes last longer.
- **T12 No sight leak** — Stand still in front of an idle echo so it SEES you (it should chase), but
  confirm distant echoes are NOT pulled toward you by sight alone (only by the chaser's *movement*
  via momentum). With the player not moving and no other movement, far echoes stay calm.

### E. Crowd shaping
- **T13 No perfect ring** — Let many echoes converge on you. With separation ON they form an organic
  mass, not a geometric ring. Toggle `bEnableHordeSeparation` OFF (restart) → the old ring returns.
- **T14 Jitter** — `HordeDirectionJitterDegrees` > 0 makes the horde fan instead of marching in
  lockstep; set to 0 to see sharp columns.

### F. Detachment
- **T15 Edge/back shed** — Form a moving horde (emit a sound to one side so they stream that way).
  Watch echoes at the **rear** and **rim** peel off and wander away (Move Dir arrows help). Raise
  `DetachChanceBackPerSec` to exaggerate.
- **T16 Weak-momentum disperse** — Let a horde's momentum decay; below `MomentumAlignThreshold`
  echoes stop following and scatter.
- **T17 Random trickle** — At full strength a few stragglers still peel off over time
  (`DetachChanceRandomPerSec`). Set it to 0 to confirm they stop shedding.
- **T18 Dead-zone** — A lone echo flagged to detach with no neighbours still drifts off (backward
  against the flow), not stuck in place.

### G. Promoted-actor momentum
- **T19 Actors drive momentum** — Get ~10+ echoes promoted to actors and chasing you (run near a
  dense group). Confirm the momentum field forms from THEIR movement (Momentum layer lights up around
  the actor pack) and that nearby horde-tier echoes fall in behind them.

### H. Attack: grab / bite / pull   (after StateTree + BP hooks wired)
- **T20 Intent flip** — Let a promoted echo reach you. With Intent colours on, its dot turns the
  Attack colour (bright red) within `MeleeAttackRange`. Console shows
  `EchoMelee[id]: enter attack state`.
- **T21 Grab** — Within `GrabRange` the log shows `GRAB <you> (dist …)`; your BP `Try Grab Target`
  fires; the echo is flagged `bBlockDemotion` (won't demote mid-grab).
- **T22 Keeps moving** — During grab/attack the echo does NOT freeze — it keeps pressing forward
  (momentum preserved). Verify visually and that other echoes still flow past it.
- **T23 Bite cadence** — Within `BiteRange` the log shows periodic `BITE … -> hit/miss` at
  `BiteCooldownSeconds` intervals; your damage hook fires each bite.
- **T24 Pull** — `Pull Target` fires each tick while grabbed; the player is pulled in per your hook.
- **T25 Release** — Move away faster than `GrabRange * GrabReleaseMultiplier`; log shows
  `RELEASE grab (target escaped)`; `bBlockDemotion` clears; the echo can demote again.
- **T26 Disable** — Set `bEnableMeleeAttack=false` (restart): echoes chase but never grab/bite.

### I. TurnTowardStimulus orient  (after StateTree wired)
- **T27 Orient only** — Emit a weak/distant sound (low Str) so an echo goes curious but doesn't
  commit to moving. Confirm it ROTATES to face the sound/pressure point without translating. Tune
  `OrientTurnRateDegPerSec`.

### J. Settings sanity
- **T28 Clamps** — In Project Settings, type invalid values (negative, out-of-range); confirm they
  clamp on edit (`ValidateAndClamp`), e.g. `BiteRange` clamps to <= `GrabRange`.
- **T29 Live-restart** — Changing a setting requires a PIE restart to take effect (read once at
  Initialize). Confirm.

### K. Networking (if testing multiplayer)
- **T30 Server authority** — `EmitWorldStimulus` and all intent/momentum run server-side. On a
  listen server the debug map drives the horde; on a pure client, click-to-emit is a no-op (expected).
- **T31 Promotion replication** — Promoted echoes replicate position/yaw/intent; clients animate via
  `ReplicatedIntent`. Confirm no client-side asserts.

### L. Performance
- **T32 Large horde** — With a big `SpawnCount`, watch frame time. `BuildMomentumFromMovement` +
  diffusion run each server tick over the local set + promoted; confirm no runaway cost. The
  momentum field cell count (HUD) should stay bounded (decay prunes it).

---

## Quick reference — intent dot colours (debug map)
Idle grey · Wander light-blue · TurnTowardStimulus cyan · Investigate yellow ·
ChaseVisibleActor red · ChaseLastSeen/Search orange · FanSearch magenta ·
JoinHordePressure green · Attack bright-red · HandleObstacle purple.

## Known follow-ups (not blocking)
- `AddWorldAgitation` was removed (dead). If you want a "seed momentum from script" API, it can be re-added.
- Several `Echo|Agitation` settings now only feed the Abstract tier (`AgitationFieldDecayPerSecond`,
  `CombatAgitationAmount`, `NoiseAgitationAmountScale`) — intended, not dead.
- All of this is self-reviewed, not yet compiled — T1 is the real gate.
