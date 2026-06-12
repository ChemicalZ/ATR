# Echo StateTree Setup

This replaces the old `statetree.txt`, which described the deprecated target-actor / last-seen-location
model and the removed `FATR_EchoTargetEvaluator` / `FATR_EchoMoveToTask`. The StateTree is now a thin
executor of subsystem-owned intent: it never decides durable memory, it only runs the move/obstacle the
subsystem already chose.

## Ownership model

The `UATR_EchoSubsystem` owns every Echo's durable awareness, intent, and search state
(`FATR_EchoRuntimeState`). `RunIntentPass` writes each active Echo's `EATR_EchoIntent` and a typed
`FATR_EchoMoveRequest` every tick. The StateTree, running on the pooled `AATR_EchoAIController`, reads
that intent through the Echo Intent evaluator and executes it through the Echo Move Request / Echo Handle
Obstacle tasks. Clients never run this — it is server/standalone only.

## Required asset wiring (`BP_ATR_EchoStateTree`)

Schema / Context

- **Context Actor = AIController** (`AATR_EchoAIController`). The tasks and evaluator cast the StateTree
  owner to `AATR_EchoAIController`; if the context actor is the pawn instead, they fail fast.

Evaluator

- **Echo Intent** (`FATR_EchoIntentEvaluator`). Reads the subsystem intent for the possessed Echo and
  exposes bindable outputs each tick: the current `EATR_EchoIntent`, the typed `FATR_EchoMoveRequest`,
  and a `bHasValidMoveRequest` gate. Bind these outputs to the task inputs below.

Tasks

- **Echo Move Request** (`FATR_EchoMoveRequestTask`). Bind `MoveRequest` and `bHasValidMoveRequest` from
  the evaluator. Used by every motion intent (chase visible, chase last-seen, search projected, fan
  search, investigate, join horde pressure). It issues the typed move through the controller and resolves
  Succeeded/Failed from the subsystem's classified result keyed by move serial — never from path going
  Idle.
- **Echo Handle Obstacle** (`FATR_EchoHandleObstacleTask`). Bind `MoveRequest` and `bHasValidMoveRequest`.
  Used only by the `HandleObstacle` intent. Executes the nav-projected sidestep/repath fallback; the
  door/window/fence break seam attaches here later via `UATR_EchoObstacleBehaviorDataAsset` without
  changing the tree.

## Transition logic

Drive state selection off the evaluator's `EATR_EchoIntent` output, and drive state completion off task
status:

- `HandleObstacle` → Echo Handle Obstacle task.
- `ChaseVisibleActor`, `ChaseLastSeenLocation`, `SearchProjectedDirection`, `FanSearchArea`,
  `InvestigateLocation`, `JoinHordePressure` → Echo Move Request task.
- `TurnTowardStimulus` → orient-only state (no move task; `bHasValidMoveRequest` is false for these).
- `Idle`, `Wander`, `ReturnToIdle`, `Dormant` → idle/ambient state.

Because the subsystem re-evaluates intent every tick, transitions should be allowed to re-select as the
intent output changes; the move task aborts cleanly (its serial is superseded) when a new intent issues a
new request.

## What must NOT be in the tree

- No `Echo Target` evaluator and no `Echo Move To` task — both are removed. If the asset still references
  them they will appear as missing nodes and must be replaced with the Echo Intent evaluator and the Echo
  Move Request task.
- No actor-target knowledge derived inside the tree. The only actor a move ever targets is the currently
  confirmed-visible actor, and that comes from the subsystem intent, not from tree-side perception.

## Verification checklist

1. Open `Content/Echo/BP_ATR_EchoStateTree`. Confirm Context Actor = AIController.
2. Confirm the single evaluator is **Echo Intent**; remove any **Echo Target** evaluator.
3. Confirm motion states use **Echo Move Request** and the obstacle state uses **Echo Handle Obstacle**;
   remove any **Echo Move To** task.
4. Confirm task inputs are bound to the Echo Intent evaluator outputs (`MoveRequest`,
   `bHasValidMoveRequest`).
5. PIE as a listen server with a few echoes; confirm chase → lost-sight search → fan → idle progresses,
   and that a blocked move routes through Handle Obstacle rather than reporting success.
