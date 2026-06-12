# All That Remains — Echo System Final Implementation Development Document

## Purpose

This document defines the final implementation direction for the Echo AI/simulation system in **All That Remains**. The goal is to finish the Echo architecture in a form that is scalable, realistic, multiplayer-safe, efficient for dedicated server/client/standalone, and extensible for future mechanics such as obstacle breaking, smell trails, group migration, and horde escalation.

This is not a tuning document. Values such as sight radius, hearing radius, confidence decay, agitation falloff, search duration, replication budgets, and promotion distances will continue to be tuned. This document defines the intended architecture, ownership model, implementation phases, and end states.

---

## Source Basis and Accuracy Notes

### Confirmed from uploaded source files

The following files were directly reviewed:

- `ATR_EchoAIController.h`
- `ATR_EchoAIController.cpp`
- `ATR_EchoMoveToTask.h`
- `ATR_EchoMoveToTask.cpp`
- `ATR_EchoTargetEvaluator.h`
- `ATR_EchoTargetEvaluator.cpp`

### Known from prior repository/design context

The broader project already has, or is expected to have, the following Echo architecture pieces:

- Echo subsystem / manager exists.
- Active Echoes are a smaller promoted pool.
- Only full active Echo actors currently get AIController + `UAIPerceptionComponent`.
- Query-driven ISM rendering/relevancy direction exists.
- Client relevancy mask/list direction exists.
- Coarse/fine grid direction exists.
- Replication scheduler/budgeting direction exists.
- Initial replication warm-up/client seeding is required so new clients see far Echoes immediately.
- PCG is used only for first-time cell generation; after generation, simulation belongs to runtime Echo/Mass/subsystem data.

Any item above should be verified against the current repo before implementation begins, but it is treated as intended architecture for this document.

---

# Executive Decision

The final Echo system should use **Subsystem-Authoritative Echo Intelligence**.

The Echo subsystem/manager owns canonical Echo state, awareness, stimuli, intent, horde agitation, search state, simulation tier, promotion/demotion, and replication relevance.

The active AIController, StateTree, perception component, pawn, and movement stack are execution adapters used only for fully promoted active Echoes.

## Final ownership rule

```text
EchoSubsystem / EchoManager = canonical truth
Active AIController          = local active sensor + executor bridge
StateTree                    = active behavior execution
Pawn / Character             = body, movement, animation, collision, combat presentation
UAIPerceptionComponent       = active-only perception confirmation
Replication scheduler        = network delivery and client seeding, not AI ownership
ISM / visual proxy systems   = rendering/presentation, not AI ownership
```

---

# Current Completed State Checklist

## Confirmed active AIController state

- [x] `AATR_EchoAIController` exists.
- [x] Controller creates `UAIPerceptionComponent`.
- [x] Controller creates sight config.
- [x] Controller creates hearing config.
- [x] Controller creates `UStateTreeComponent`.
- [x] Sight detects enemies and neutrals.
- [x] Sight ignores friendlies.
- [x] Hearing detects enemies and neutrals.
- [x] Hearing ignores friendlies.
- [x] Controller implements `GetGenericTeamId()`.
- [x] Controller implements `SetGenericTeamId()`.
- [x] Echoes are intended to share a team and therefore count each other as friendly.
- [x] Players are neutral and therefore detectable by Echoes.
- [x] `OnPossess()` enables perception ticking.
- [x] `OnPossess()` binds `OnPerceptionUpdated`.
- [x] `OnPossess()` starts StateTree logic.
- [x] `OnUnPossess()` stops movement before `Super::OnUnPossess()`.
- [x] `OnUnPossess()` clears current target.
- [x] `OnUnPossess()` stops StateTree logic.
- [x] `OnUnPossess()` removes perception delegate.
- [x] `OnUnPossess()` disables perception ticking.
- [x] `EnterPool()` exists as a safety cleanup path.
- [x] `EnterPool()` clears current target.
- [x] `EnterPool()` stops StateTree logic.
- [x] `EnterPool()` removes perception delegate.
- [x] `EnterPool()` disables perception ticking.
- [x] Perception callback is authority-only.
- [x] Target selection currently evaluates sight-known actors.
- [x] Target scoring currently favors closer actors.
- [x] Target scoring favors currently sensed actors over stale sight memory.
- [x] Target scoring applies loyalty multiplier for current target.
- [x] Current target is stored as `TWeakObjectPtr<AActor>`.

## Confirmed StateTree evaluator state

- [x] `FATR_EchoTargetEvaluator` exists.
- [x] Evaluator reads `AATR_EchoAIController::GetCurrentTarget()`.
- [x] Evaluator exposes `TargetActor` as bindable StateTree output.
- [x] Evaluator assumes StateTree context owner is the AIController.

## Confirmed StateTree move task state

- [x] `FATR_EchoMoveToTask` exists.
- [x] Move task accepts `TargetActor`.
- [x] Move task accepts `TargetLocation`.
- [x] Move task accepts `AcceptanceRadius`.
- [x] Move task uses `MoveToActor()` when `TargetActor` is set.
- [x] Move task uses `MoveToLocation()` when `TargetActor` is null.
- [x] Move task handles `AlreadyAtGoal`.
- [x] Move task handles request failure.
- [x] Move task currently treats path-following `Idle` as movement success.
- [x] Move task stops movement on StateTree exit.

## Known broader system state from project context

- [x] Echo subsystem/manager exists.
- [x] Active Echo is a smaller pool, not the entire population.
- [x] Lower-level simulation exists or is planned through subsystem/grid/Mass-style runtime data.
- [x] Replication scheduler exists or is planned as part of the networking architecture.
- [x] Query-driven ISM/relevancy model exists or is planned.
- [x] Initial client seeding/warm-up must exist so new clients see far Echoes immediately.
- [x] Coarse/fine grid architecture exists or is planned.

---

# Key Gaps in Current Active AI

## Architectural gaps

- [ ] AIController currently owns active target selection as local state.
- [ ] Canonical target memory is not yet subsystem-owned.
- [ ] Active controller perception is not yet converted into subsystem awareness records.
- [ ] StateTree currently consumes `TargetActor`, not subsystem intent.
- [ ] No durable awareness handoff is visible between active actor pooling and subsystem state.
- [ ] Hearing exists but is not yet modeled as a location-only stimulus in canonical simulation.
- [ ] Horde awareness/agitation spread is not yet represented in the reviewed active AI files.
- [ ] Directional search/fan-out is not yet represented in the reviewed active AI files.
- [ ] Movement failure classification hooks are not yet represented.
- [ ] Obstacle intent hooks are not yet represented.

## Correctness gaps

- [ ] Stale sight memory can still result in `MoveToActor()` if StateTree receives `TargetActor`.
- [ ] Missing target binding can fall back to `FVector::ZeroVector` location movement.
- [ ] `EPathFollowingStatus::Idle` is currently treated as success, even if the move failed, aborted, or was blocked.
- [ ] Perception cleanup disables ticking but does not explicitly disable senses or clear perception memory.
- [ ] Team comment says Echoes are Team 1, but reviewed code default is `TeamNumber = 2`; comment/config mismatch should be resolved.

## Performance gaps

- [ ] Active AI logs use `LogTemp` and some Warning-level logging in hot lifecycle paths.
- [ ] Perception delegate binding should be guarded against duplicate binding if lifecycle bugs occur.
- [ ] No visible throttling/cadence control exists in reviewed active AI files.
- [ ] StateTree evaluator ticks every StateTree tick and only exposes actor target; this should become intent-driven and cheap.

## Realism gaps

- [ ] Hearing should not directly identify an actor for behavior.
- [ ] Lost sight should produce last-seen movement, projected direction, and search behavior.
- [ ] Echoes should not become a perfect hive mind.
- [ ] Edge-pulling one Echo from a crowd must remain possible.
- [ ] Horde behavior should emerge through indirect agitation, not exact shared target data.
- [ ] Obstacle response should be represented even before breaking is implemented.

---

# What Changes and What Remains the Same

| Area | Current State | Change | Remains the Same | Final Owner |
|---|---|---|---|---|
| Active Echo pool | Active Echoes are a smaller promoted pool | Pool becomes executor layer, not canonical brain owner | Active actors still exist for near/full simulation | EchoSubsystem owns canonical state; pool executes |
| AIController | Owns perception and current target | Becomes active sensor/executor bridge | Still used for promoted full actors | EchoSubsystem owns memory/intent; controller reports facts |
| `UAIPerceptionComponent` | Exists on active AIController | Kept only for active truth confirmation | Still used for active sight/hearing confirmation | Controller uses it; subsystem stores results |
| Hearing | Configured but not target-driving | Converts to location-only stimulus events | Active perception can still detect hearing | EchoSubsystem owns stimulus memory |
| Sight | Selects best actor locally | Reports seen/lost-sight facts into subsystem | Active Echoes still use UE sight | EchoSubsystem owns awareness and intent |
| Target evaluator | Exposes `TargetActor` | Replaced/expanded into intent evaluator | StateTree still receives bindable data | Subsystem intent feeds evaluator |
| Move task | Actor or location move | Uses explicit move request type and result tracking | StateTree still issues movement | StateTree task executes subsystem target |
| Search behavior | Not present in reviewed files | Add last-seen, projected direction, fan-out | Search remains active behavior | Subsystem owns search state; StateTree executes |
| Horde behavior | Not present in reviewed files | Add agitation field and indirect spread | Echoes still remain individual | Subsystem/grid owns agitation |
| Obstacle behavior | Not present in reviewed files | Add failure classification and obstacle intent hooks | Actual breaking can come later | Subsystem records intent; tasks execute later |
| Replication | Existing/planned scheduler | Replicate summarized relevant state, not full AI brain | Scheduler remains | Replication layer delivers state/events |
| ISM/proxies | Existing/planned query-driven visual layer | Sample subsystem state | Rendering remains separate from AI | Visual systems consume state |
| PCG | First-time generation | No change to first-generation role | PCG remains generation-only | Runtime simulation owns post-generation Echoes |

---

# Final System Model

## Runtime tiers

```text
Tier 0: Abstract / unloaded Echoes
    No actor
    No AIController
    No UAIPerception
    Cell/population/count/pressure simulation only

Tier 1: Low-detail simulated Echoes
    Usually no actor
    Individual or compact data record
    Cheap stimulus/agitation/search/migration state

Tier 2: Near visual Echoes / proxy Echoes
    May have actor or ISM/proxy representation
    No full AIController unless promoted
    Receives movement/animation presentation state

Tier 3: Full active Echoes
    Pawn/Character
    AIController
    StateTree
    UAIPerceptionComponent
    PathFollowing/CharacterMovement
    Combat/collision/animation
```

## Canonical data flow

```text
World events / player actions / Echo actions
        ↓
EchoSubsystem stimulus/event ingestion
        ↓
Grid/cell agitation + individual awareness updates
        ↓
Intent selection per relevant Echo or cell
        ↓
Promotion/demotion decision
        ↓
Active controller + StateTree execute intent if promoted
        ↓
Controller reports perception/movement/combat results
        ↓
EchoSubsystem updates canonical runtime state
        ↓
Replication/rendering/ISM/client seeding consume summarized state
```

---

# Final Data Structures

Names can be adjusted to match project style, but the concepts should remain stable.

## Echo runtime state

```cpp
struct FATR_EchoRuntimeState
{
    int32 EchoId;

    EATR_EchoSimulationTier Tier;
    EATR_EchoIntent Intent;

    FVector Location;
    FVector Velocity;
    FVector FacingDirection;

    FATR_EchoAwarenessState Awareness;
    FATR_EchoSearchState Search;
    FATR_EchoMovementIntent Movement;
    FATR_EchoObstacleIntent Obstacle;

    float Aggression;
    float Agitation;
    float LastUpdateTime;
};
```

## Simulation tier enum

```cpp
enum class EATR_EchoSimulationTier : uint8
{
    Abstract,
    LowDetail,
    VisualProxy,
    Active
};
```

## Intent enum

```cpp
enum class EATR_EchoIntent : uint8
{
    Dormant,
    Idle,
    Wander,
    TurnTowardStimulus,
    InvestigateLocation,
    ChaseVisibleActor,
    ChaseLastSeenLocation,
    SearchProjectedDirection,
    FanSearchArea,
    JoinHordePressure,
    Attack,
    HandleObstacle,
    ReturnToIdle
};
```

## Awareness mode enum

```cpp
enum class EATR_AwarenessMode : uint8
{
    None,
    HeardLocation,
    SawTarget,
    LostSightSearch,
    SmellTrail,
    HordeAgitated,
    ObstacleBlocked
};
```

## Awareness state

```cpp
struct FATR_EchoAwarenessState
{
    EATR_AwarenessMode Mode = EATR_AwarenessMode::None;

    TWeakObjectPtr<AActor> ConfirmedVisibleActor;

    FVector LastSeenLocation = FVector::ZeroVector;
    FVector LastSeenVelocity = FVector::ZeroVector;
    FVector ProjectedSearchLocation = FVector::ZeroVector;

    FVector LastHeardLocation = FVector::ZeroVector;
    FVector LastSmelledLocation = FVector::ZeroVector;
    FVector HordePressureDirection = FVector::ZeroVector;

    float LastSeenTime = -1.f;
    float LastHeardTime = -1.f;
    float LastSmelledTime = -1.f;

    float Confidence = 0.f;
    float Urgency = 0.f;

    bool bHasCurrentLineOfSight = false;
};
```

## Search state

```cpp
struct FATR_EchoSearchState
{
    FVector Origin = FVector::ZeroVector;
    FVector PrimaryDirection = FVector::ForwardVector;

    int32 SearchStepIndex = 0;

    float StartedTime = -1.f;
    float SearchRadius = 600.f;
    float MaxSearchDuration = 12.f;

    bool bSearchActive = false;
};
```

## Stimulus event

```cpp
enum class EATR_StimulusType : uint8
{
    Noise,
    Smell,
    Blood,
    EchoAgitation,
    DoorImpact,
    WindowImpact,
    Combat,
    Scripted
};

struct FATR_StimulusEvent
{
    EATR_StimulusType Type;
    FVector Location;
    FVector Direction;

    float Strength;
    float Radius;
    float TimeSeconds;

    // Optional metadata only. Do not use for direct target knowledge.
    TWeakObjectPtr<AActor> SourceActor_DebugOnly;
};
```

## Move request

```cpp
enum class EATR_EchoMoveTargetType : uint8
{
    None,
    Actor,
    Location
};

struct FATR_EchoMoveRequest
{
    EATR_EchoMoveTargetType Type = EATR_EchoMoveTargetType::None;

    TWeakObjectPtr<AActor> Actor;
    FVector Location = FVector::ZeroVector;

    float AcceptanceRadius = 50.f;
};
```

## Move failure reason

```cpp
enum class EATR_MoveFailureReason : uint8
{
    None,
    InvalidTarget,
    NoPath,
    BlockedByDynamicActor,
    BlockedByDoor,
    BlockedByWindow,
    BlockedByFence,
    TargetUnreachable,
    NavmeshMissing,
    AbortedByNewIntent
};
```

## Obstacle intent hook

```cpp
struct FATR_EchoObstacleIntent
{
    bool bHasObstacle = false;

    EATR_MoveFailureReason Reason = EATR_MoveFailureReason::None;

    TWeakObjectPtr<AActor> ObstacleActor;
    FVector ObstacleLocation = FVector::ZeroVector;

    float LastObstacleTime = -1.f;
};
```

---

# Hearing Decision

Hearing should be implemented as **location-only stimulus behavior**.

## Rule

An Echo may hear a location. It does not know the actor that made the sound unless sight or another explicit confirmation establishes that actor.

## Allowed metadata

Stimulus events may carry a debug/source actor pointer for analytics, debugging, noise ownership, or gameplay credit. This pointer must not be used as behavioral target knowledge.

```text
Correct:
    Echo hears location X and investigates X.

Incorrect:
    Echo hears actor A and chases A's live position.
```

---

# Sight and Lost-Sight Decision

Sight may confirm an actor only while the actor is currently visible.

When sight is lost, the system must transition to memory/location behavior.

```text
Saw target:
    Intent = ChaseVisibleActor
    Move target = Actor

Lost sight:
    Intent = ChaseLastSeenLocation
    Move target = LastSeenLocation

Reached last seen location:
    Intent = SearchProjectedDirection
    Move target = ProjectedSearchLocation

Projected search exhausted:
    Intent = FanSearchArea
    Move target = generated search point
```

Projection must be clamped to avoid supernatural prediction.

Example tuning values:

```text
ProjectionSeconds = 1.0 to 3.0
MaxProjectionDistance = 400 to 1200 cm
```

---

# Horde Behavior Decision

Horde behavior should be indirect.

Echoes should not share exact player target data with nearby Echoes. Instead, they should spread agitation and pressure.

## Supported behavior

- A single edge Echo can be pulled away from a crowd.
- Nearby Echoes can become curious or agitated.
- Repeated or strong agitation can pull more Echoes.
- Large groups can migrate toward strong stimuli.
- Horde behavior emerges without hive-mind targeting.

## Agitation cell concept

```cpp
struct FATR_AgitationCell
{
    float Agitation = 0.f;
    FVector WeightedDirection = FVector::ZeroVector;
    float LastUpdatedTime = -1.f;
};
```

## Agitation sources

- Gunshot
- Door impact
- Window impact
- Player sprinting/noise
- Combat
- Echo vocalization
- Echo chasing nearby
- Blood/smell event
- Scripted event

---

# Search Behavior Decision

Directional search and fan-out should be implemented now, not deferred.

## Search progression

```text
1. Chase visible actor.
2. On lost sight, move to last seen location.
3. From last seen location, move along projected direction.
4. Search left/right offsets.
5. Widen fan search.
6. Decay confidence.
7. Return to idle, wander, or join horde pressure.
```

## Example fan pattern

```text
Origin + Forward * 500
Origin + Forward * 350 + Right * 300
Origin + Forward * 350 - Right * 300
Origin + Forward * 700 + Right * 500
Origin + Forward * 700 - Right * 500
```

Each Echo should have variation so groups do not search with identical robotic paths.

Variation inputs:

- Aggression
- Persistence
- Hearing sensitivity
- Random angular offset
- Search radius
- Search duration
- Recent horde pressure

---

# Obstacle Hook Decision

Obstacle breaking should not be fully implemented in this pass, but hooks must exist now.

## Required now

- Movement failure classification.
- Obstacle intent record.
- StateTree transition hook for `HandleObstacle`.
- Placeholder fallback behavior.

## Deferred

- Door breaking.
- Window breaking.
- Fence climbing/breaking.
- Obstacle damage model.
- Group pounding/escalation behavior.
- Audio/FX for obstacle assault.

## Placeholder behavior

```text
If movement fails due to suspected obstacle:
    Record obstacle hook.
    Set Intent = HandleObstacle.
    Current HandleObstacle implementation may repath, search nearby, or fail gracefully.
```

---

# Implementation Phases

## Phase 0 — Baseline Audit and Naming Cleanup

### Goal

Freeze the current baseline and remove ambiguity before architectural changes.

### Tasks

- [ ] Confirm actual Echo team ID value.
- [ ] Resolve comment/config mismatch: comment says Team 1, code defaults to `TeamNumber = 2`.
- [ ] Add dedicated log category, e.g. `LogATREchoAI`.
- [ ] Replace hot `LogTemp` usage in Echo AI files.
- [ ] Demote lifecycle/move spam to `VeryVerbose` or guarded debug logs.
- [ ] Confirm StateTree context owner is AIController in assets.
- [ ] Confirm active pool lifecycle calls `OnUnPossess()` and `EnterPool()` in intended order.
- [ ] Confirm subsystem/manager has stable Echo IDs for active Echoes.
- [ ] Confirm active actor can map back to `EchoId`.
- [ ] Confirm replication scheduler/client seeding remains separate from AI intent ownership.

### Dead code / cleanup

- [ ] Remove misleading comments claiming a different team ID than the code uses.
- [ ] Remove or guard possess/move Warning logs.
- [ ] Remove any duplicate target-selection utilities not used by StateTree or subsystem.

### End state

- Current system still behaves as before.
- Team identity is unambiguous.
- Logging is safe for scale.
- Every active Echo can be traced back to a canonical `EchoId`.

---

## Phase 1 — Canonical Runtime State in EchoSubsystem

### Goal

Create subsystem-owned canonical Echo state without changing behavior yet.

### Tasks

- [ ] Add `FATR_EchoRuntimeState`.
- [ ] Add `EATR_EchoSimulationTier`.
- [ ] Add `EATR_EchoIntent`.
- [ ] Add `FATR_EchoAwarenessState`.
- [ ] Add `FATR_EchoSearchState`.
- [ ] Add `FATR_EchoMovementIntent` or equivalent.
- [ ] Add `FATR_EchoObstacleIntent`.
- [ ] Add API to get mutable runtime state by `EchoId`.
- [ ] Add API to get read-only runtime state by `EchoId`.
- [ ] Add active controller registration API.
- [ ] Add active controller unregistration API.
- [ ] Add validation checks for invalid/missing Echo IDs.

### Required APIs

```cpp
FATR_EchoRuntimeState* GetMutableEchoState(int32 EchoId);
const FATR_EchoRuntimeState* GetEchoState(int32 EchoId) const;
void RegisterActiveEcho(int32 EchoId, AATR_EchoAIController* Controller, APawn* Pawn);
void UnregisterActiveEcho(int32 EchoId);
```

### Dead code / cleanup

- [ ] Do not remove current AIController target selection yet.
- [ ] Mark controller-owned `CurrentTarget` as transitional.

### End state

- Subsystem can store canonical state for every Echo.
- Active Echoes have a stable bridge to subsystem state.
- No major behavior change yet.

---

## Phase 2 — Active Perception Reports Facts to Subsystem

### Goal

Convert active perception from local AI decision-making into fact reporting.

### Tasks

- [ ] Add perception report methods to subsystem.
- [ ] Report current sight confirmation.
- [ ] Report lost sight with last known location and last known velocity.
- [ ] Report hearing as location-only stimulus.
- [ ] Ensure hearing does not create direct actor targeting.
- [ ] Store optional debug/source actor metadata without using it for behavior.
- [ ] Explicitly disable senses and clear perception memory on pool entry.
- [ ] Explicitly re-enable senses on possess.

### Required APIs

```cpp
void ReportEchoSawActor(int32 EchoId, AActor* Actor, const FVector& Location, const FVector& Velocity, float TimeSeconds);
void ReportEchoLostSight(int32 EchoId, AActor* Actor, const FVector& LastKnownLocation, const FVector& LastKnownVelocity, float TimeSeconds);
void ReportEchoHeardLocation(int32 EchoId, const FVector& Location, float Strength, float TimeSeconds);
void ReportEchoStimulus(int32 EchoId, const FATR_StimulusEvent& Event);
```

### AIController changes

- [ ] Keep `UAIPerceptionComponent`.
- [ ] Keep sight/hearing config.
- [ ] Change `HandlePerceptionUpdated()` from “select best local target” to “report perception facts.”
- [ ] Preserve authority-only behavior.
- [ ] Stop using hearing as a target actor source.

### Dead code / cleanup

- [ ] Deprecate `SelectBestTarget()`.
- [ ] Deprecate `CurrentTarget` as canonical target.
- [ ] Remove local-only target score once subsystem intent is active.

### End state

- Active perception feeds subsystem awareness.
- Controller no longer owns the canonical truth of what the Echo knows.
- Behavior can still temporarily use old evaluator until Phase 3.

---

## Phase 3 — Subsystem Intent Selection

### Goal

Subsystem chooses high-level Echo intent from awareness, stimuli, and current state.

### Tasks

- [ ] Implement intent update function for active and relevant simulated Echoes.
- [ ] Implement visible actor intent.
- [ ] Implement lost-sight intent.
- [ ] Implement projected search intent.
- [ ] Implement fan search intent.
- [ ] Implement hearing investigation intent.
- [ ] Implement horde pressure intent placeholder.
- [ ] Implement return-to-idle intent.
- [ ] Add confidence decay.
- [ ] Add urgency decay.
- [ ] Add agitation decay.
- [ ] Add debug tracing for intent transitions.

### Intent rules

```text
Current line of sight:
    Intent = ChaseVisibleActor

Sight lost and memory fresh:
    Intent = ChaseLastSeenLocation

Reached last seen location:
    Intent = SearchProjectedDirection

Projected search reached/exhausted:
    Intent = FanSearchArea

Heard noise but no sight:
    Intent = InvestigateLocation

Agitation pressure only:
    Intent = TurnTowardStimulus or JoinHordePressure

Memory expired:
    Intent = ReturnToIdle or Wander
```

### Dead code / cleanup

- [ ] Remove local target-scoring behavior from controller once subsystem intent is authoritative.
- [ ] Remove direct `GetCurrentTarget()` dependency from final StateTree behavior.

### End state

- Subsystem is the canonical source of Echo intent.
- Active controller can be destroyed/pooled without destroying memory or intent.

---

## Phase 4 — Replace Target Evaluator with Intent Evaluator

### Goal

StateTree consumes subsystem intent and move requests instead of local controller target actor.

### Tasks

- [ ] Add new StateTree evaluator: `FATR_EchoIntentEvaluator`.
- [ ] Evaluator reads `EchoId` from controller/pawn.
- [ ] Evaluator reads runtime state from subsystem.
- [ ] Evaluator exposes intent.
- [ ] Evaluator exposes move request.
- [ ] Evaluator exposes target actor only when `Intent == ChaseVisibleActor` and line of sight is current.
- [ ] Evaluator exposes target location for lost sight, projected search, fan search, hearing investigation, horde pressure, and obstacle fallback.
- [ ] Update StateTree asset bindings.

### Output fields

```cpp
EATR_EchoIntent Intent;
FATR_EchoMoveRequest MoveRequest;
AActor* ConfirmedVisibleActor;
FVector TargetLocation;
float AcceptanceRadius;
bool bHasValidMoveRequest;
```

### Dead code / cleanup

- [ ] Remove `FATR_EchoTargetEvaluator` after StateTree asset is migrated.
- [ ] Remove `GetCurrentTarget()` if no longer used.
- [ ] Remove target actor-only StateTree bindings.

### End state

- StateTree is driven by subsystem intent.
- Active behavior no longer accidentally chases stale actors.
- The system supports actor movement only when actor visibility is confirmed.

---

## Phase 5 — Replace Move Task with Explicit Move Request Task

### Goal

Make movement execution accurate, safe, and reportable.

### Tasks

- [ ] Add `FATR_EchoMoveRequest` input.
- [ ] Reject move request when type is `None`.
- [ ] Reject location movement if no valid location flag/type is set.
- [ ] Remove implicit `TargetActor == null means move to FVector::ZeroVector` behavior.
- [ ] Track `FAIRequestID` for submitted move.
- [ ] Track completion result from `UPathFollowingComponent`.
- [ ] Report movement success to subsystem.
- [ ] Report movement failure to subsystem.
- [ ] Classify failure where possible.
- [ ] Keep fallback classification if exact obstacle is unknown.

### Dead code / cleanup

- [ ] Remove old `FATR_EchoMoveToTask` after StateTree migration.
- [ ] Remove polling-only `Idle == Succeeded` movement logic.
- [ ] Remove actor/location ambiguous move input.

### End state

- Movement cannot accidentally path to world origin.
- Movement success/failure is accurate.
- Subsystem can react to blocked/no-path/aborted movement.
- Obstacle hooks are now possible.

---

## Phase 6 — Last-Seen Projection and Directional Search

### Goal

Implement realistic lost-target behavior directly.

### Tasks

- [ ] Compute `LastSeenLocation` when target is visible.
- [ ] Compute `LastSeenVelocity` when target is visible.
- [ ] Compute clamped `ProjectedSearchLocation` when sight is lost.
- [ ] Add search sequence generation.
- [ ] Add fan search point generation.
- [ ] Add per-Echo search variation.
- [ ] Add search duration and confidence decay.
- [ ] Add transition from search to idle/wander/horde pressure.

### Required behavior

```text
Visible target → actor chase.
Lost sight → last seen location.
Reached last seen → projected direction.
Projection exhausted → fan search.
Search exhausted → idle/wander/horde pressure.
```

### Dead code / cleanup

- [ ] Remove any StateTree transitions that treat missing target actor as immediate idle if memory/search exists.
- [ ] Remove any behavior that reacquires stale actor location without line-of-sight confirmation.

### End state

- Echoes pursue believable memory.
- Echoes search where the target was likely heading.
- Echoes do not magically know the live target position after losing sight.

---

## Phase 7 — Hearing as Location-Only Stimulus

### Goal

Make hearing useful without violating realism.

### Tasks

- [ ] Convert hearing stimuli into `FATR_StimulusEvent` with `Type = Noise`.
- [ ] Store heard location.
- [ ] Store strength/urgency.
- [ ] Apply distance/strength falloff.
- [ ] Convert strong hearing into `InvestigateLocation`.
- [ ] Convert weak hearing into `TurnTowardStimulus` or mild agitation.
- [ ] Do not store heard actor as target.
- [ ] Allow optional debug/source actor metadata only.

### Dead code / cleanup

- [ ] Remove comments claiming hearing is used for alerting unless alerting is implemented through the subsystem.
- [ ] Remove any future code path that uses hearing source actor for direct chase.

### End state

- Sounds pull Echoes toward locations.
- Sounds can help create horde pressure.
- Sounds do not create omniscient actor targeting.

---

## Phase 8 — Horde Agitation and Indirect Awareness Spread

### Goal

Support horde behavior without hive-mind targeting.

### Tasks

- [ ] Add agitation grid/cell state.
- [ ] Add agitation source events.
- [ ] Add agitation decay.
- [ ] Add direction-weighted pressure.
- [ ] Add nearby Echo agitation spread.
- [ ] Add thresholds for curiosity, investigation, and horde joining.
- [ ] Ensure exact visible target data is not shared indirectly.
- [ ] Allow edge Echoes to peel away independently.
- [ ] Add debug visualization for agitation cells and pressure direction.

### Behavior rules

```text
Direct sight gives exact actor knowledge only to the seeing Echo.
Nearby Echoes may receive agitation, not the target actor.
Agitation can cause turning, wandering toward pressure, or horde joining.
Strong repeated stimuli can pull groups.
```

### Dead code / cleanup

- [ ] Remove any shared-target shortcuts between Echoes.
- [ ] Remove any immediate “broadcast player target to nearby Echoes” logic if present.

### End state

- One Echo can be pulled from a crowd.
- Crowds can still escalate into hordes.
- Horde behavior scales through grid/cell pressure, not per-Echo omniscience.

---

## Phase 9 — Obstacle Failure Hooks

### Goal

Prepare for obstacle breaking without implementing the full break system.

### Tasks

- [ ] Add move failure classification.
- [ ] Add obstacle intent state.
- [ ] Add StateTree `HandleObstacle` state.
- [ ] Add placeholder obstacle fallback task.
- [ ] Add hooks for door/window/fence actor classification.
- [ ] Add event reporting for obstacle contact/failure.
- [ ] Add debug logs for obstacle hook activation.

### Placeholder behavior

```text
If blocked/no path:
    Record obstacle location/reason.
    Enter HandleObstacle.
    Try short repath/search/fallback.
    Return to search or idle if unresolved.
```

### Deferred behavior

- [ ] Door damage.
- [ ] Window breaking.
- [ ] Fence breaking/climbing.
- [ ] Multi-Echo obstacle pounding.
- [ ] Noise generated by obstacle attacks.
- [ ] Structural persistence.

### Dead code / cleanup

- [ ] Remove generic move-failed-is-idle behavior.
- [ ] Remove movement failure paths that silently discard failure reason.

### End state

- AI has a clean seam for obstacle behavior.
- Future obstacle breaking will not require reworking movement/intent architecture.

---

## Phase 10 — Promotion/Demotion Memory Continuity

### Goal

Ensure active pooling does not erase intelligence.

### Tasks

- [ ] On promotion, initialize controller/StateTree from subsystem state.
- [ ] On demotion, unregister active controller but keep subsystem state.
- [ ] Stop perception and StateTree on demotion.
- [ ] Clear active-only references safely.
- [ ] Keep awareness/search/agitation/intent in subsystem.
- [ ] Ensure active actor recycling does not leak prior Echo state.
- [ ] Ensure pooled controllers have no stale delegates.
- [ ] Ensure pooled controllers have no stale move requests.

### Dead code / cleanup

- [ ] Remove any code that treats controller spawn/possess as new AI memory initialization.
- [ ] Remove any reliance on controller-owned state surviving pooling.

### End state

- Echo can see player, lose sight, demote, promote, and continue correct search/investigation.
- Active pool is purely an execution layer.

---

## Phase 11 — Lower-Tier Simulation Additions

### Goal

Make non-active Echoes react believably without full AIController/perception.

### Tasks

- [ ] Add low-detail stimulus sampling.
- [ ] Add cell-level noise pressure.
- [ ] Add cell-level agitation pressure.
- [ ] Add abstract migration intent.
- [ ] Add low-detail search/investigation state.
- [ ] Add population movement across cells.
- [ ] Add rules for building-contained Echoes responding to stimuli.
- [ ] Add promotion candidates based on proximity, visibility, stimulus, and relevance.
- [ ] Add demotion rules based on distance, visibility, budget, and current urgency.

### Dead code / cleanup

- [ ] Remove any fallback that requires full active actors for non-near Echo decisions.
- [ ] Remove any simulation code that depends on per-Echo `UAIPerceptionComponent` outside active tier.

### End state

- Far/lower-tier Echoes can react to noise/agitation/search pressure cheaply.
- Full active AI remains reserved for relevant near Echoes.

---

## Phase 12 — Replication and Client Presentation Integration

### Goal

Keep server authority while making clients see responsive Echo behavior efficiently.

### Tasks

- [ ] Replicate only relevant active Echoes as actors.
- [ ] Use replication scheduler budgets for Echo state delivery.
- [ ] Preserve initial client seeding/warm-up so new clients see far Echoes immediately.
- [ ] Replicate summarized lower-tier/proxy state where needed.
- [ ] Replicate movement/animation/intent presentation state, not full AI memory.
- [ ] Ensure clients do not run authoritative Echo decision-making.
- [ ] Ensure standalone uses same authoritative subsystem path locally.
- [ ] Integrate query-driven ISM/relevancy with subsystem state.

### Client data should include

- Transform / compressed movement.
- Animation state.
- Relevant active intent enum if needed for animation/FX.
- Combat/audio events.
- Visual proxy/ISM data.

### Client data should not include

- Full target memory.
- Full perception history.
- Complete horde field data unless needed for debug.
- Irrelevant far Echo internals.

### Dead code / cleanup

- [ ] Remove any client-side authoritative target selection.
- [ ] Remove any replication path that sends full AI brain state unnecessarily.
- [ ] Remove actor-only rendering assumptions for far Echoes.

### End state

- Dedicated server owns decisions.
- Clients receive enough presentation state.
- Standalone uses same code path.
- New clients receive seeded far/proxy Echo visibility immediately.

---

## Phase 13 — Debugging, Visualization, and Metrics

### Goal

Make the final system measurable and tunable.

### Tasks

- [ ] Add debug draw for active perception facts.
- [ ] Add debug draw for last seen location.
- [ ] Add debug draw for projected search location.
- [ ] Add debug draw for fan search points.
- [ ] Add debug draw for agitation cells.
- [ ] Add debug draw for horde pressure vectors.
- [ ] Add debug draw for obstacle hooks.
- [ ] Add per-tier Echo counts.
- [ ] Add active pool occupancy metrics.
- [ ] Add promotion/demotion counts.
- [ ] Add perception event counts.
- [ ] Add movement success/failure counts.
- [ ] Add replication scheduler budget metrics.

### Dead code / cleanup

- [ ] Remove ad hoc `LogTemp` debugging once structured debug tools exist.
- [ ] Remove noisy always-on debug draw.

### End state

- Echo behavior can be inspected at runtime.
- Performance and correctness can be measured.
- Tuning can happen without guessing.

---

## Phase 14 — Final Cleanup and Stabilization

### Goal

Remove transitional code and leave the system in final architecture form.

### Tasks

- [ ] Remove old `CurrentTarget` canonical usage.
- [ ] Remove old `SelectBestTarget()` local scoring.
- [ ] Remove old `FATR_EchoTargetEvaluator`.
- [ ] Remove old ambiguous `FATR_EchoMoveToTask`.
- [ ] Remove any stale comments saying AIController owns all AI.
- [ ] Update comments to reflect subsystem-authoritative architecture.
- [ ] Add documentation comments for ownership boundaries.
- [ ] Add tests or automation where practical.
- [ ] Verify multiplayer server/client behavior.
- [ ] Verify standalone behavior.
- [ ] Verify active pool recycling.
- [ ] Verify lower-tier simulation.
- [ ] Verify initial client seeding.

### End state

- No transitional actor-target-only AI path remains.
- Echo subsystem owns durable intelligence.
- Active controller/StateTree execute intent only.
- System is ready for continuous tuning and future feature hooks.

---

# Dead Code and Transitional Code Removal List

## Remove after Phase 4/5 migration

- `FATR_EchoTargetEvaluator`
- `FATR_EchoTargetEvaluatorInstanceData`
- StateTree bindings that only expose `TargetActor`
- `AATR_EchoAIController::GetCurrentTarget()` if unused
- `AATR_EchoAIController::CurrentTarget` as canonical behavior state
- `AATR_EchoAIController::SelectBestTarget()` once subsystem intent replaces it

## Remove after movement migration

- `FATR_EchoMoveToTask` if replaced by explicit move request task
- `FATR_EchoMoveToTaskInstanceData` actor/location ambiguity
- Polling logic that treats `Idle` as guaranteed success
- Any implicit null-actor fallback to `FVector::ZeroVector`

## Replace immediately or early

- Hot-path `LogTemp` usage
- Warning-level possess/move spam
- Comments stating controller owns all AI
- Team ID mismatch comments

## Keep

- `UAIPerceptionComponent` for active Echoes
- Sight config for active Echoes
- Hearing config for active Echoes, but use as location stimulus input
- `UStateTreeComponent` for active execution
- AIController lifecycle cleanup
- Pooling structure
- Authority-only perception reporting
- Generic team behavior for Echo-friendly filtering

---

# Full Implementation End State

At the end of this implementation, the Echo system should satisfy the following checklist.

## Architecture

- [ ] EchoSubsystem owns canonical runtime state for all Echoes.
- [ ] EchoSubsystem owns awareness, stimuli, intent, search, agitation, obstacle hooks, and tier.
- [ ] Active AIController is not the canonical brain.
- [ ] StateTree executes subsystem intent.
- [ ] Active perception reports facts into subsystem.
- [ ] Lower-tier simulation does not require actor perception.
- [ ] Promotion/demotion preserves memory and intent.
- [ ] PCG remains first-generation only; runtime simulation owns generated Echoes afterward.

## Performance

- [ ] Full AI stack is limited to active promoted Echoes.
- [ ] Lower tiers use cheap grid/subsystem simulation.
- [ ] Logging is not hot/noisy.
- [ ] Replication is scheduled/budgeted.
- [ ] Clients receive only presentation-relevant state.
- [ ] Standalone uses same authority path locally.
- [ ] Active pool recycling is clean and safe.

## Accuracy

- [ ] Sight confirms actors only while visible.
- [ ] Lost sight uses last known/projected/search locations.
- [ ] Hearing is location-only.
- [ ] Movement success/failure is tracked accurately.
- [ ] Movement failures report reasons where possible.
- [ ] No missing target path moves to world origin.
- [ ] Echo team behavior is consistent and documented.

## Realism

- [ ] Echoes can chase visible targets.
- [ ] Echoes can lose targets.
- [ ] Echoes search projected movement direction.
- [ ] Echoes fan-search after losing target.
- [ ] Echoes investigate sounds without omniscience.
- [ ] Single edge Echoes can be pulled from crowds.
- [ ] Horde behavior emerges through indirect agitation.
- [ ] Obstacles have hooks for future break/attack behavior.

## Networking

- [ ] Server remains authoritative.
- [ ] Clients do not run authoritative AI decisions.
- [ ] Active Echoes replicate as relevant actors.
- [ ] Far/proxy Echoes are represented through scheduled/seeding/ISM/proxy data.
- [ ] New clients receive initial far Echo seeding.
- [ ] Replication does not require full AI memory state.

---

# Non-Goals for This Implementation

These are intentionally deferred, but hooks should exist where specified.

- Full obstacle destruction.
- Door/window/fence damage persistence.
- Advanced smell trail simulation.
- Advanced group combat coordination.
- Advanced animation polish.
- Final tuning of all AI constants.
- Final replication budget tuning.
- Final horde balance tuning.

---

# Implementation Priority Summary

1. Establish subsystem-owned runtime state.
2. Convert active perception into subsystem fact reporting.
3. Move target memory and intent out of the AIController.
4. Replace actor-only target evaluator with subsystem intent evaluator.
5. Replace ambiguous move task with explicit move request/result tracking.
6. Implement lost-sight projection and directional/fan search.
7. Implement hearing as location-only stimulus.
8. Implement indirect horde agitation.
9. Add obstacle hooks.
10. Preserve memory across promotion/demotion.
11. Extend lower-tier simulation.
12. Integrate replication/client presentation cleanly.
13. Add debug visualization and metrics.
14. Remove transitional/dead code.

---

# Final Design Principle

The Echo system should not be a collection of smart actors. It should be a scalable simulation where active actors are temporary high-fidelity projections of durable subsystem-owned Echo state.

That principle is what allows the game to support complex behavior, believable zombie realism, horde-scale simulation, efficient multiplayer server authority, cheap clients, and consistent standalone behavior.
