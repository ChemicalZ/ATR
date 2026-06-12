# Echo Horde — Developer Settings Guide

**Location:** `Project Settings → AllThatRemains → Echo Horde` (`UATR_EchoSettings`, config: `DefaultGame.ini`)

Every behavior-relevant number in the Echo system lives here or in a data asset — there are no magic numbers in code. This guide explains what each category controls, how the values interact, and which knobs to reach for to achieve specific design goals.

**Mental model.** An Echo is simulated at one of four tiers: **Abstract** (far away — cell populations, no individuals), **LowDetail** (near-ish — cheap individual simulation), **VisualProxy** (rendered, no AI), and **Active** (full Character + AIController + StateTree). Stimuli (sound in real dB, smell, agitation) flow into per-Echo awareness; awareness drives intent (chase, investigate, search, engage barrier…); intent drives movement. During **active prey pursuit** an Echo moves on the *line of desire* — straight toward the stimulus, engaging whatever blocks it. It never solves a route around a fence or building.

---

## 1. Quick recipes — "I want to…"

| Goal | Touch these |
|---|---|
| Make a gunshot draw Echoes from farther away | Raise the emitted `LoudnessDb` at the call site, lower `EchoHearingThresholdDb`, or lower `AirAbsorptionDbPer100m`. Check the cap `MaxAudibleRangeCm`. |
| Make Echoes converge in more/fewer separate hordes after a loud sound | `HearingMaxLocationErrorFraction` (more error → more scattered estimates → more distinct converging streams). |
| Stop distant Echoes being dragged along by a passing horde | Raise `MomentumCalmResistance`. |
| Make hordes form faster / stickier | Raise `MomentumBuildRate`, `MomentumPersistence`; lower `MomentumAlignThreshold`. |
| Make hordes break up sooner | Raise `MomentumDecayPerSecond` and the `Detach*` chances. |
| Make doors hold longer under attack | Lower `BarrierDamagePerHit` / `BarrierPressureDamageMultiplier`, raise the door's health (barrier actor), or gate damage off via the obstacle-behavior asset. |
| Make door-banging attract more Echoes | Raise `BarrierImpactLoudnessDb` (or the per-barrier `ImpactLoudnessDbOverride`). |
| Make Echoes give up on a barrier sooner | Lower `MaxBarrierEngageSecondsWithoutStimulus` and `FrustratedSearchDurationSeconds`, or raise `ConfidenceDecayPerSecond`. |
| Make grabbed players escape more easily | Lower `PullBaseAcceleration` / `PullMaxSpeed`, raise `GrabReleaseMultiplier`, or lower `BargeSuccessPowerThreshold`. |
| Make shoulder-barging through Echoes harder | Raise `BargeSuccessPowerThreshold` / `BargeMinSpeed`, lower `BargeCenterEffectiveness`. |
| Make lost-target searches longer/wider | `SearchMaxDurationSeconds`, `SearchDefaultRadius`, `MaxSearchSteps`, plus the `SearchRadius*`/`SearchDuration*` variation scales. |
| Get more fully-simulated Echoes near the player | Raise `PoolSize` and `PromoteRadius` (watch CPU). |
| Reduce server CPU | Lower `SimHz`, `PoolSize`, `LowDetailMaxUpdatesPerTick`, `AbstractMaxCellsPerTick`; shrink `LocalZoneRadius`. |
| Reduce bandwidth | Lower the `*SnapshotHz` rates and relevancy ranges; raise `PositionDirtyThreshold`. |

---

## 2. Acoustics (`Echo|Acoustics`) — sound in real units

Sound is authored as **dB SPL at the reference distance** (default 1 m), like a spec sheet. Reference points: whisper ≈ 30, footstep ≈ 45, conversation ≈ 60, door pounding ≈ 85, breaking glass ≈ 100, gunshot ≈ 140.

Propagation (in `ATR_EchoAcoustics.h` — the single seam where sound ray tracing will plug in later):

```
ReceivedDb(d) = LoudnessDb − 20·log10(d / RefDistance) − AirAbsorption · (d / 100 m)
```

That is inverse-square spreading (−6 dB per doubling of distance) plus linear atmospheric absorption. An Echo hears a sound when the received level exceeds its threshold; perceived intensity normalizes between threshold and saturation. **Audible radius is always derived — never authored.**

| Setting | Default | Meaning |
|---|---|---|
| `AcousticReferenceDistanceCm` | 100 | Distance at which `LoudnessDb` is specified (1 m standard). |
| `EchoHearingThresholdDb` | 35 | Received level below this is masked (≈ the world's ambient noise floor). Lower = Echoes hear quieter/farther sounds. |
| `EchoHearingSaturationDb` | 95 | Received level at/above this registers at full intensity (urgency/agitation scale linearly in dB between threshold and saturation). |
| `AirAbsorptionDbPer100m` | 0.5 | Extra dB lost per 100 m. Raise for muffled, foggy atmosphere; 0 = pure inverse-square. |
| `MaxAudibleRangeCm` | 30000 | Hard cap on any sound's audible radius (bounds query cost). |
| `DefaultPerceivedNoiseLoudnessDb` | 70 | Source dB assumed for an AIPerception noise with Loudness 1.0 (each ×2 loudness = +6 dB). |
| `HearingMaxLocationErrorFraction` | 0.30 | **Imperfect hearing.** Max error in an Echo's estimate of the source position, as a fraction of distance, at barely-audible levels (shrinks to 0 at saturation). This is what splits one gunshot into several distinct hordes converging on *roughly* the right spot. |

**Worked example:** a 140 dB gunshot vs a 35 dB threshold with 0.5 dB/100 m absorption is audible to roughly 1.5 km (capped by `MaxAudibleRangeCm`). An Echo 800 m away receives ≈ 78 dB → strong but not saturated → solid urgency, and its position estimate can be off by up to ~7% of 800 m.

---

## 3. Awareness, Sight, Hearing (`Echo|Awareness`, `Echo|Sight`, `Echo|Hearing`)

What an Echo *knows* and how fast it forgets. The no-cheat rule: sight knowledge updates only while the target is actually visible; hearing only ever yields a (jittered) location.

| Setting | Default | Meaning |
|---|---|---|
| `ConfidenceDecayPerSecond` | 0.15 | How fast sight memory fades once the target is hidden. Drives how long chases of last-seen positions and barrier engagements persist. |
| `UrgencyDecayPerSecond` | 0.20 | How fast pursuit aggression cools off. |
| `LostSightMemoryThreshold` | 0.05 | Below this confidence the Echo forgets entirely and drops to idle/wander/horde behaviors. |
| `HeardMemorySeconds` | 8 | How long a heard location stays actionable. |
| `HeardInvestigateUrgency` | 0.40 | Urgency needed for a noise to cause a walk-to investigation (below it: orient only). |
| `ReacquireSightConfidence` | 1.0 | Confidence restored when sight is regained. |
| `ActiveSightRadius` / `ActiveLoseSightRadius` | 2000 / 2500 | See / lose-sight ranges (the gap is hysteresis). |
| `ActivePeripheralVisionAngleDegrees` | 90 | Half-angle: 90 = 180° forward cone. |
| `ActiveSightMaxAgeSeconds`, `ActiveHearingMaxAgeSeconds` | 5 / 5 | Perception stimulus max age. |
| `ActiveHearingRange` | 3000 | AIPerception hearing sense radius for **Active** Echoes (the dB model then decides what is actually heard within it). |
| `LastSeenProjectionSeconds` / `MaxLastSeenProjectionDistance` | 2 / 800 | How far ahead a lost target's observed velocity is projected into a search anchor (clamped so prediction can't be supernatural). |
| `LastSeenVelocitySmoothingAlpha` | 0.5 | Smoothing on observed target velocity (1 = snap to newest sample). |
| `NoiseStrengthToUrgencyScale` / `NoiseStrengthToAgitationScale` | 1.0 / 0.25 | Conversion from perceived sound intensity to urgency / agitation. |

---

## 4. Active pursuit (`Echo|ActivePursuit`) — line of desire

During active prey pursuit (visible chase, last-seen chase, local search steps, strong *nearby* noise) the Echo moves **directly along the stimulus vector**. The navmesh only validates that the next step has walkable ground; it never plans a route. Blockers met head-on are classified and engaged.

| Setting | Default | Meaning |
|---|---|---|
| `bUseLineOfDesirePursuit` | **true** | Master switch. OFF reverts to legacy full pathfinding (Echoes intelligently route around fences — debug only, violates the design). |
| `ActivePursuitForwardSweepDistanceCm` | 150 | Blocker-detection sweep length along the desired direction. |
| `ActivePursuitSweepRadiusCm` | 30 | Sweep sphere radius (0 = line trace). |
| `ActivePursuitGroundProjectionRadiusCm` | 300 | Navmesh validation radius for the next step. Step with no walkable ground = don't take it. |
| `ActivePursuitStuckTimeSeconds` / `ActivePursuitStuckProgressCm` | 0.75 / 15 | If less than this progress is made for this long, whatever is ahead is classified and engaged (this is how untagged walls become press → frustrate). |
| `ActivePursuitDirectInvestigateDistanceCm` | 2500 | Heard sounds within this distance are pursued on the line of desire; farther ones count as ambient movement and may use broad navigation. |
| `ActivePursuitBlockerHeadOnDot` | 0.35 | How head-on a swept tagged barrier must be to trigger immediate engagement (glancing contacts wall-slide; the stuck timer still catches everything). |

---

## 5. Barrier engagement (`Echo|Barrier`, `Echo|ObstacleHooks`)

When the line of desire is blocked, the Echo **engages the blocker** — face, press, attack, reach through — and never reroutes. Pressure from multiple Echoes stacks; impacts emit real noise that recruits more Echoes (the door-banging feedback loop). When the stimulus goes stale it degrades into frustrated lingering, then idle.

| Setting | Default | Meaning |
|---|---|---|
| `BarrierEngageDistanceCm` | 150 | Contact range the Echo closes to before pressing/attacking. |
| `BarrierReachThroughDistanceCm` | 180 | Default reach-through interaction distance for permeable barriers (fence/broken window). Barrier data assets can override. |
| `BarrierAttackIntervalSeconds` | 1.2 | Seconds between hits while engaged. |
| `BarrierFirstAttackDelaySeconds` | 0.5 | Wind-up before the first hit (obstacle-behavior asset can override). |
| `BarrierDamagePerHit` | 10 | Fallback per-hit damage when no asset provides one. |
| `BarrierPressurePerEcho` | 1.0 | Group pressure each engaged Echo contributes per hit. |
| `BarrierPressureDecayPerSecond` | 0.5 | Pressure decay once Echoes stop hitting. |
| `BarrierPressureDamageMultiplier` | 0.25 | `Damage = PerHit × (1 + Pressure × this)` — the horde overwhelms, it doesn't think. |
| `BarrierImpactLoudnessDb` | 85 | Source loudness of impacts (audible radius derives from the acoustics model). |
| `MaxBarrierEngageSecondsWithoutStimulus` | 6 | Engagement persists this long after the stimulus goes stale before flipping to frustrated search. |
| `FrustratedSearchDurationSeconds` / `FrustratedSearchRadiusCm` | 6 / 300 | How long, and in what radius, a frustrated Echo lingers/shuffles near the barrier. |
| `FrustratedSearchHitIntervalSeconds` / `FrustratedSearchHitRangeMultiplier` | 3 / 1.5 | Cadence and range gate of the occasional frustrated taps. |
| `FrustratedShuffleMinRadiusFraction` / `FrustratedShuffleAcceptRadiusCm` | 0.3 / 40 | Shape of the random shuffle around the barrier. |
| `ObstacleForwardTraceLength` | 200 | Legacy blocked-path classification trace (pathfinding moves only). |
| `ObstacleHandleTimeoutSeconds` | 2 | Freshness window for a block to start/keep engagement without a new impact. |
| `bAllowActivePursuitTacticalReroute` | **false** | DEBUG ONLY. True = blocked pursuit sidesteps/repaths (`ObstacleSidestepDistance`, `ObstacleForwardNudgeDistance`). Forbidden in normal play. |

**Barrier authoring.** Tag barrier actors (`Echo.Obstacle.Door`, `.Window`, `.Fence`, `.Gate`, `.Barricade`, `.Vehicle`, `.DestructibleWall`) or implement `IATR_EchoBarrier` (returns a `UATR_EchoBarrierDataAsset` with per-type rules, reports passability, receives impacts). Untagged static geometry = non-interactable wall: silent press → frustrate → decay, no damage, no noise. Per-Echo capabilities (can it damage doors/windows, damage per hit, pounding agitation) live in `UATR_EchoObstacleBehaviorDataAsset`, referenced by `DefaultObstacleBehavior`.

---

## 6. Search (`Echo|Search`)

Lost-sight behavior: walk to the last-seen point, then a projected-direction step, then a fan of search points. Every point is navmesh-projected before becoming a move.

| Setting | Default | Meaning |
|---|---|---|
| `ReachLocationRadius` | 120 | "Arrived" tolerance for memory/search points. |
| `SearchDefaultRadius` | 600 | Search radius when no velocity projection is available. |
| `SearchMaxDurationSeconds` | 12 | Hard cap on one search. |
| `SearchStepAcceptanceRadius` | 120 | Tolerance for advancing to the next fan step. |
| `SearchPointNavProjectionRadius` | 500 | Points that can't project to navmesh within this are skipped. |
| `SearchRandomAngleDegrees` | 20 | Per-Echo angular jitter so groups fan out. |
| `MaxSearchSteps` | 5 | Fan steps before the search is exhausted. |
| `SearchRadius{BaseScale, PerEchoVariation, AggressionBonus}` | 0.8 / 0.6 / 0.3 | Radius scale = Base + Variation×hash + Bonus×aggression. |
| `SearchDuration{BaseScale, PerEchoVariation, AggressionBonus}` | 0.7 / 0.6 / 0.3 | Same composition for duration. |
| `DefaultSearchPattern` | — | Optional `UATR_EchoSearchPatternDataAsset` overriding the built-in fan offsets. |

---

## 7. Horde model (`Echo|Agitation`, `Echo|HordeShaping`, `Echo|HordeMomentum`)

There is no hive mind. Moving Echoes build a **momentum field**; nearby Echoes align with it; aligned movement self-reinforces. Sounds kick groups into motion, which seeds momentum. No Echo ever learns another's target.

Key interactions added by the imperfect-hearing work: an Echo with a **fresh personal heard location always steers to its own estimate** and ignores the field; calm Echoes (low agitation) need `MomentumAlignThreshold × (1 + MomentumCalmResistance × (1 − agitation))` field strength to be recruited.

| Setting | Default | Meaning |
|---|---|---|
| `MomentumCellSize` | 2000 | Field cell size (coarser = cheaper, blurrier hordes). |
| `AgitationFieldDecayPerSecond` / `EchoPersonalAgitationDecayPerSecond` | 0.25 / 0.10 | Field / personal agitation fade. |
| `HordeCuriosityThreshold` / `AgitationJoinThreshold` | 0.20 / 0.50 | Agitation to orient toward / migrate with the horde. |
| `NoiseAgitationAmountScale`, `CombatAgitationAmount` | 1.0 / 1.0 | Agitation deposited by noise / combat events. |
| `HordePressureMoveDistance` | 800 | Step distance when migrating under pressure. |
| `HordeOrientTargetDistanceCm` | 500 | Pseudo-target distance for orient-only curiosity. |
| `HeardSteerMinSpeedFraction` | 0.5 | Horde-tier speed fraction toward an Echo's own heard estimate at zero urgency (scales to 1.0 at full urgency). |
| `bEnableMomentumDiffusion` / `MomentumDiffusionRate` | true / 3.0 | Field spreading into neighboring cells. |
| `HordeDirectionJitterDegrees` | 25 | Per-Echo jitter on field-driven movement. |
| `bEnableHordeSeparation`, `HordeSeparationRadius/Strength/MaxNeighbors` | true / 160 / 0.85 / 12 | Short-range de-clumping. |
| `HordeSeparationOnlySpeedFraction` | 0.35 | Speed when only separating (no flow/stimulus). |
| `HordeApproachJitterDegrees` | 20 | Spread on direct player-seek so crowds don't form perfect rings. |
| `bEnableHordeDetachment` + `Detach*` | true | Edge/tail/random peel-off so hordes erode (`DetachEdgeNeighborCount` 3, `DetachBackDot` 0.25, chances 0.5/0.6/0.05 per s, `DetachDriftSpeed` 70). |
| `bEnableHordeMomentum` | true | Master switch for the whole momentum model. |
| `MomentumBuildRate` / `MomentumDecayPerSecond` / `MomentumPersistence` | 1.5 / 0.5 / 0.7 | How fast aligned movement builds momentum, how fast it decays, and how much strong momentum resists decay. |
| `MomentumMoverSpeedThreshold` / `MomentumRefMoverCount` | 30 / 12 | Minimum mover speed counted; movers needed for full-strength build. |
| `MomentumAlignThreshold` / `MomentumCalmResistance` | 0.12 / 3.0 | Field strength to join; calm-Echo resistance multiplier (see above). |
| `MomentumMaxStrength` | 1.0 | Field cap. |
| `SoundImpulse{Radius, Speed, StrengthScale}` | 4000 / 150 / 1.0 | A loud sound starts horde-tier Echoes within the radius moving toward *their own estimate* of it — this seeds hordes. |

---

## 8. Combat (`Echo|Combat`)

Grab → bite → pull, layered on top of the chase, plus the player's shoulder barge. All resolution is C++-owned and logged to `LogATR_EchoCombat` (structured — mine it for tuning).

**Grab:** `MeleeAttackRange` 220 (flips Chase→Attack), `GrabRange` 160, `GrabCooldownSeconds` 1.0, facing cone `GrabFacingConeDegrees` 60. Chance = (`GrabBaseChance` 0.90 − `GrabEchoSpeedPenalty` 0.25×speed − `GrabTargetSpeedPenalty` 0.35×speed/`GrabTargetSpeedReference` 450 − `GrabRandomMissChance` 0.05) × arm-condition multiplier (`GrabMissingFingersMultiplier` 0.90, `GrabMissingHandMultiplier` 0.75). Whiffs scratch with `ScratchOnFailedGrabChance` 0.60 (× `ScratchFingerlessMultiplier` 0.5 if fingerless). Strong grips need healthy arms: `StrongGripBaseChance` 0.50 × strength, capped at `StrongGripChanceCap` 0.95. Release at `GrabRange × GrabReleaseMultiplier` (1.6).

**Bite:** `BiteRange` 110, `BiteCooldownSeconds` 1.2, `BiteBaseMissChance` 0.10 (+`BiteWeakGripMissPenalty` 0.20 on weak grips). Wound severity = cumulative roll thresholds `BiteStrongScratchUpTo` 0.20 / `BiteStrongDeepScratchUpTo` 0.60 (rest Laceration); weak grip 0.50 / 0.85.

**Pull:** `PullBaseAcceleration` 1200 × grip transmission (`WeakGripPullTransmission` 0.45) × strength × `MeleePullStrength`, saturating at `PullMaxSpeed` 300 toward the Echo. Standing still doesn't protect you — the grip compensates the body's braking friction — but deliberate input away still out-accelerates the pull.

**Body condition** (rolled deterministically per Echo): `BodyHealthyChance` 0.70, `BodyMissingFingersChance` 0.15, `BodyMissingHandChance` 0.10, remainder = no arms; muscle `BodyStrengthScalar{Min,Max}` 0.6–1.4.

**Shoulder barge** (player runs through an Echo):

```
Power = (speed / BargeReferenceSpeed) × clamp(playerMass/echoMass, MassRatioMin..Max)
        × lerp(BargeCenterEffectiveness, 1, lateralOffset) ÷ echo strength
```

Requires `BargeMinSpeed` 300 and approach dot ≥ `BargeMinApproachDot` 0.2. Success (≥ `BargeSuccessPowerThreshold` 0.5) launches the Echo aside (`BargeKnockbackSpeed` 450 scaled by power capped at `BargePowerCap` 2, mix `BargeKnockbackSideMix` 0.8 / `BargeKnockbackForwardMix` 0.5, hop `BargeKnockbackUpSpeed` 60), staggers it for `BargeStaggerSeconds` 1.0 × power (grip drops, no re-grab), and costs the player up to `BargePlayerSpeedLossAtCenter` 0.45 of their speed on a dead-center hit (shoulder clips cost almost nothing). `BargeCooldownSeconds` 0.4 per Echo. Dead-center torso ≈ 3× harder than clipping a shoulder (`BargeCenterEffectiveness` 0.35).

`OrientTurnRateDegPerSec` 240 controls the turn-toward-stimulus rate. `bEnableMeleeAttack` is the combat master switch.

---

## 9. Tiers, performance, networking

**Population/Simulation:** `InitializeCount` 10000 (SoA capacity), `SpawnCount`/`SpawnRadius` (initial seed), `SimHz` 20 (horde integration rate), `HordeWalkSpeed` 120.

**Promotion** (`Echo|Promotion`/`Echo|Demotion`): pool of `PoolSize` 20 full actors. Score-based: distance + `MustPromoteScoreBonus` 1000 inside `MustPromoteRadius` 800, plus `PromotionUrgencyBoost` 200, `PromotionConfidenceBoost` 150, `PromotionAgitationBoost` 150, `PromotionPlayerFacingBoost` 100 weights and a `RecentlyDemotedPenalty` 300 decaying over `RecentlyDemotedSeconds` 3. Promote within `PromoteRadius` 2500, demote beyond `DemoteRadius` 4000, hysteresis `MinTimeInTierSeconds` 1.5. Demotion is blocked during visible chases, fresh searches (`DemotionSearchBlockSeconds` 5), barrier engagement, or above the confidence/urgency block thresholds (0.5/0.5).

**Lower tiers** (`Echo|LowerTierSimulation`): `LowDetailUpdateHz` 8 with `LowDetailMaxUpdatesPerTick` 256 budget; speeds `LowDetailInvestigateSpeed` 150 / `LowDetailSearchSpeed` 120 / `LowDetailWanderSpeed` 60; `AbstractUpdateHz` 1 with `AbstractMaxCellsPerTick` 64 and `AbstractCellMigrationRate` 0.05.

**Spatial:** fine grid `GridCellSize` 500 within `LocalZoneRadius` 27500 of players; coarse grid `CoarseGridCellSize` 15000; world `WorldHalfExtent` 512000.

**Rendering** (`Echo|Rendering`): client-side ISM tiers at `VisualNearDistance` 3000 / `VisualMidDistance` 10000 / `VisualFarDistance` 20000; shadow/decal/distance-field toggles default off.

**Networking** (`Echo|Networking`): snapshot rates `NearSnapshotHz` 10 / `MidSnapshotHz` 3 / `FarSnapshotHz` 1 over relevancy ranges 3000/10000/25000; scheduler budget `ServerReplicationBudgetMs` 1.5 with per-frame job/snapshot caps; `PositionDirtyThreshold` 5 cm and `YawDirtyThresholdDegrees` 2 gate what counts as changed.

**Classes** (`Echo|Classes`): Blueprint subclass overrides for the manager, active Echo, and AI controller.

---

## 10. Data assets and the tag contract

| Asset | Role |
|---|---|
| `UATR_EchoBarrierDataAsset` | Per-barrier-TYPE interaction rules (door vs chain-link fence vs glass window): reach-through capability and distance, damage gates, impact loudness override. Returned by the barrier actor's `IATR_EchoBarrier::GetEchoBarrierData`. Several fields (`DamageThreshold`, `PressureThreshold`, climb-through, sight/sound/smell blocking) are authored contracts marked TODO pending their consuming systems. |
| `UATR_EchoObstacleBehaviorDataAsset` | Per-ECHO capabilities: can it damage doors/windows, per-hit damage, first-attack delay, group-pounding agitation. Set as `DefaultObstacleBehavior` in settings. |
| `UATR_EchoSearchPatternDataAsset` | Custom search fan offsets (else the built-in fan is used). |

**Actor tags** classify barriers without code: `Echo.Obstacle.Door`, `Echo.Obstacle.Window`, `Echo.Obstacle.Fence`, `Echo.Obstacle.Gate`, `Echo.Obstacle.Barricade`, `Echo.Obstacle.Vehicle`, `Echo.Obstacle.DestructibleWall`. The `IATR_EchoBarrier` interface wins over tags when both are present.

---

## 11. Debugging

The Echo Debug Map (toolbar in PIE) draws the horde, momentum field/flow, abstract cells, intent colors, target lines, **yellow filled triangles for players**, and **white rings around live (promoted) actors**. Its click-to-emit tool takes a loudness in dB and shows the derived audible radius, so what you see is exactly what the horde hears. Combat resolutions log structured data to `LogATR_EchoCombat`; AI flow to `LogATR_EchoAI` (Verbose/VeryVerbose).
