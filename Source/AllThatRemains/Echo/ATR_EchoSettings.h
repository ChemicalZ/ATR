// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ATR_EchoSettings.generated.h"

class AATR_EchoManager;
class AATR_ActiveEcho;
class AATR_EchoAIController;
class UATR_EchoSearchPatternDataAsset;
class UATR_EchoObstacleBehaviorDataAsset;
class UATR_WeaponDamageProfile;

// Project Settings > AllThatRemains > Echo Horde
//
// All Echo system config lives here. C++ reads it once at init; never modify
// subsystem/manager properties directly at runtime.
//
// Categories are ordered by concern:
//   Echo|Classes      — which C++/Blueprint classes the system spawns.
//   Echo|Population    — initial capacity and seeding.
//   Echo|Simulation    — tick cadence and horde movement.
//   Echo|Spatial       — grid sizing and the local interest zone.
//   Echo|Promotion     — when a horde entity becomes a full Actor.
//   Echo|Rendering     — what the *local client* draws as ISM (RenderThread cost).
//   Echo|Networking    — what the *server* sends to clients (bandwidth cost).
//   Echo|Dirty State   — thresholds that gate replication/visual updates.
//
// Rendering and Networking ranges are intentionally separate: rendering drives
// RenderThread/occlusion cost, networking drives bandwidth/serialization cost.
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Echo Horde"))
class ALLTHATREMAINS_API UATR_EchoSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName("AllThatRemains"); }

	// Defensive clamp of all settings to their required invariants. Called from
	// PostInitProperties at CDO load time (once per process) and from
	// PostEditChangeProperty on in-editor edits. Runtime callers do not need to
	// re-clamp — the CDO is already valid by the time any world ticks.
	void ValidateAndClamp();

	virtual void PostInitProperties() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ── Echo|Classes ───────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Classes", meta=(
		ToolTip="Blueprint subclass of AATR_EchoManager. Leave empty to use the native C++ manager. Assign a Blueprint here when designers need to configure ISM meshes, materials, or visual offsets."
	))
	TSoftClassPtr<AATR_EchoManager> ManagerClass;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Classes", meta=(
		ToolTip="Blueprint subclass of AATR_ActiveEcho. Leave empty to use the native C++ actor. This class is used for promoted echoes that require full Actor behavior, AI, collision, animation, perception, or StateTree logic."
	))
	TSoftClassPtr<AATR_ActiveEcho> ActiveEchoClass;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Classes", meta=(
		ToolTip="Blueprint subclass of AATR_EchoAIController. Leave empty to use the native C++ controller. This controller is assigned to promoted echo actors from the pool."
	))
	TSoftClassPtr<AATR_EchoAIController> ControllerClass;

	// ── Echo|Population ────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Population", meta=(
		ClampMin="1",
		ToolTip="Maximum number of live Echo entities. The subsystem pre-allocates SoA arrays to this size at startup and does not resize them during play. Higher values allow larger hordes but increase memory and any remaining full-capacity operations."
	))
	int32 InitializeCount = 10000;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Population", meta=(
		ClampMin="0",
		ToolTip="Number of Echo entities seeded at BeginPlay on server/standalone. Higher values increase initial horde density and startup work."
	))
	int32 SpawnCount = 10;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Population", meta=(
		ClampMin="0.0",
		ForceUnits="cm",
		ToolTip="Radius around world origin where SpawnCount echoes are randomly scattered at BeginPlay. Larger values spread initial echoes over a wider area."
	))
	float SpawnRadius = 5000.f;

	// ── Echo|Simulation ────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Simulation", meta=(
		ClampMin="1",
		ClampMax="120",
		DisplayName="Simulation Hz",
		ToolTip="Authoritative horde simulation steps per second on server/standalone. Higher values make horde movement more responsive but increase CPU cost. Lower values improve performance but make movement less smooth."
	))
	int32 SimHz = 20;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Simulation", meta=(
		ClampMin="0.0",
		ForceUnits="cm/s",
		ToolTip="Speed applied to unpromoted horde entities that are close enough to steer toward a player. Higher values make local horde pressure more aggressive but increase movement churn and dirty transform updates."
	))
	float HordeWalkSpeed = 90.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Simulation", meta=(
		ClampMin="0.0",
		ClampMax="0.9",
		ToolTip="Per-echo walk-speed spread, as a fraction of HordeWalkSpeed (and the promoted actor's base walk speed). Each echo gets a STABLE multiplier in [1-this, 1+this] from its EchoId, so a herd shows mixed paces — some shamblers lag, some press ahead — instead of marching in lockstep. 0 = uniform speed; 0.25 = ±25%."
	))
	float HordeWalkSpeedVariation = 0.25f;

	// ── Echo|Spatial ───────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Spatial", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		DisplayName="Fine Grid Cell Size",
		ToolTip="Cell size for the local fine spatial grid used near players. Smaller cells make radius queries more precise but increase grid overhead. Larger cells reduce overhead but can increase candidates per query."
	))
	float GridCellSize = 500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Spatial", meta=(
		ClampMin="1000.0",
		ForceUnits="cm",
		ToolTip="Half-width of the simulated world square. Used for spatial grid bounds and XY encoding. Larger worlds require more coordinate range but may reduce precision if network packing depends on this value."
	))
	float WorldHalfExtent = 512000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Spatial", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Radius around each player where horde entities enter the local fine grid. Must be greater than or equal to VisualFarDistance and FarRelevancyRange, or visual/network queries may miss echoes. Larger values increase local query and rendering candidate cost."
	))
	float LocalZoneRadius = 27500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Spatial", meta=(
		ClampMin="1000.0",
		ForceUnits="cm",
		ToolTip="Cell size for the global coarse entity-bucket grid. Larger cells reduce coarse cell boundary updates but increase candidate counts during local queries. Smaller cells improve local query precision but increase coarse-grid bookkeeping. Must be greater than or equal to the fine grid cell size."
	))
	float CoarseGridCellSize = 15000.f;

	// ── Echo|Promotion ─────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(
		ClampMin="0",
		ToolTip="Number of ActiveEcho actors and AI controllers pre-warmed in the server pool. Higher values allow more fully simulated nearby echoes but increase actor, AI, animation, and replication cost."
	))
	int32 PoolSize = 20;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Inner priority radius. Echoes inside this radius are promoted before outer-ring echoes when pool slots are limited. Must be less than PromoteRadius. Smaller values reserve full Actor simulation for very close threats."
	))
	float MustPromoteRadius = 800.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Distance from a player where a horde entity may be promoted to a full Actor. Must be less than DemoteRadius. Larger values create more actor-level zombies near players and cost more CPU."
	))
	float PromoteRadius = 2500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Distance from all players where a promoted Actor can be demoted back to horde SoA simulation. Must be greater than PromoteRadius. The gap between PromoteRadius and DemoteRadius prevents rapid promote/demote flip-flopping."
	))
	float DemoteRadius = 4000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(
		ClampMin="0.0",
		ForceUnits="s",
		ToolTip="Minimum seconds an echo should remain in its current tier before another tier transition is allowed. Higher values reduce promote/demote churn but may keep actors alive longer than strictly necessary."
	))
	float MinTimeInTierSeconds = 1.5f;

	// ── Echo|Rendering ─────────────────────────────────────────────────────────
	// These control ONLY what the local client draws as horde ISM. They are
	// independent of the Echo|Networking relevancy ranges. Defaults are more
	// aggressive (shorter) than networking so the client/server can track echoes
	// that are not yet visually represented.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Distance where horde ISM visuals use the near visual tier. Near should be the highest visual fidelity tier. Larger values keep higher-detail instances visible farther away and increase render cost."
	))
	float VisualNearDistance = 3000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Distance where horde ISM visuals switch from mid to far visual tier. Must be greater than or equal to VisualNearDistance. Larger values keep mid-detail visuals farther away and increase instance/render cost."
	))
	float VisualMidDistance = 10000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Maximum distance where horde ISM visuals are rendered. Echoes beyond this range have no ISM instance. Must be less than or equal to LocalZoneRadius. Larger values increase visible horde count, ISM bounds size, occlusion cost, and RenderThread work."
	))
	float VisualFarDistance = 20000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(
		ToolTip="Whether horde ISM components cast shadows. Default false for performance. Enabling this can greatly increase RenderThread and shadow cost when many horde instances are visible."
	))
	bool bHordeISMCastShadows = false;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(
		ToolTip="Whether horde ISM components receive decals. Default false for performance. Enabling this can increase material/decal cost."
	))
	bool bHordeISMReceivesDecals = false;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(
		ToolTip="Whether horde ISM components affect distance-field lighting. Default false for performance. Enabling this can increase lighting and distance-field cost."
	))
	bool bHordeISMAffectDistanceFieldLighting = false;

	// ── Echo|Networking ────────────────────────────────────────────────────────
	// These control ONLY what the server sends to clients. They are independent
	// of the Echo|Rendering visual ranges and do not affect local draw distance.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="1.0",
		ClampMax="60.0",
		ForceUnits="Hz",
		ToolTip="Snapshot send rate for near echoes. Higher values improve client smoothness and accuracy but increase bandwidth and serialization cost."
	))
	float NearSnapshotHz = 10.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="0.1",
		ClampMax="60.0",
		ForceUnits="Hz",
		ToolTip="Snapshot send rate for mid-range echoes. Lower than near to reduce bandwidth."
	))
	float MidSnapshotHz = 3.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="0.1",
		ClampMax="60.0",
		ForceUnits="Hz",
		ToolTip="Snapshot send rate for far-range echoes. Keep low to reduce bandwidth and bunch size."
	))
	float FarSnapshotHz = 1.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Network near relevancy range. Echoes inside this range receive the highest snapshot rate. This does not control visual rendering distance."
	))
	float NearRelevancyRange = 3000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Network mid relevancy range. Echoes between near and mid receive medium-rate snapshots. This does not control visual rendering distance."
	))
	float MidRelevancyRange = 10000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="100.0",
		ForceUnits="cm",
		ToolTip="Network far relevancy range. Echoes between mid and far receive low-rate snapshots. Must be less than or equal to LocalZoneRadius or subsystem queries may not return all candidates."
	))
	float FarRelevancyRange = 25000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="1",
		ClampMax="512",
		ToolTip="Maximum echo snapshots serialized into a single network chunk. Larger chunks reduce overhead but risk larger bunches. Smaller chunks reduce max bunch size but increase chunk count."
	))
	int32 MaxSnapshotsPerChunk = 256;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="0.0",
		ForceUnits="s",
		ToolTip="Cooldown before another full resync can be requested. Higher values reduce resync spam but may keep clients incorrect longer after packet loss or prediction drift."
	))
	float FullResyncCooldownSeconds = 2.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="1",
		ToolTip="Number of missing snapshot sequences allowed before a full resync is requested. Lower values recover faster but can trigger more resyncs."
	))
	int32 MaxMissingSequencesBeforeResync = 3;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="0.1",
		ClampMax="10.0",
		ForceUnits="ms",
		ToolTip="Maximum CPU time the Echo replication scheduler should spend per server frame. Lower values reduce frame spikes but increase replication latency. Higher values reduce latency but can create server hitches with many clients."
	))
	float ServerReplicationBudgetMs = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="1",
		ClampMax="64",
		ToolTip="Maximum number of client replication jobs processed per server frame. A job is one client and one band. Higher values reduce replication latency but increase per-frame server cost."
	))
	int32 MaxReplicationJobsPerFrame = 8;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="1",
		ClampMax="2048",
		ToolTip="Maximum snapshots a single client may receive from Echo replication in one server frame across all bands. Higher values improve catch-up speed but increase bandwidth and CPU spikes."
	))
	int32 MaxSnapshotsPerClientPerFrame = 256;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="1",
		ClampMax="64",
		ToolTip="Maximum Near-band replication jobs processed per frame. Near updates are highest priority because they affect close horde motion and client accuracy."
	))
	int32 MaxNearReplicationJobsPerFrame = 8;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="0",
		ClampMax="64",
		ToolTip="Maximum Mid-band replication jobs processed per frame. Mid updates may be deferred before Near updates when the replication budget is tight."
	))
	int32 MaxMidReplicationJobsPerFrame = 4;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(
		ClampMin="0",
		ClampMax="64",
		ToolTip="Maximum Far-band replication jobs processed per frame. Far updates are lowest priority and should be aggressively staggered to prevent server frame spikes."
	))
	int32 MaxFarReplicationJobsPerFrame = 2;

	// ── Echo|Dirty State ───────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Dirty State", meta=(
		ClampMin="0.0",
		ForceUnits="cm",
		ToolTip="Minimum position change before an echo is marked transform-dirty for replication/visual update. Higher values reduce update frequency and bandwidth but can make movement appear less accurate."
	))
	float PositionDirtyThreshold = 5.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Dirty State", meta=(
		ClampMin="0.0",
		ForceUnits="deg",
		ToolTip="Minimum yaw change before an echo is marked transform-dirty. Higher values reduce update frequency but can make turning less accurate."
	))
	float YawDirtyThresholdDegrees = 2.f;

	// ── Echo|Awareness ─────────────────────────────────────────────────────────
	// Memory decay and stimulus→behavior thresholds. These drive how long an Echo
	// holds knowledge after a stimulus and how aggressively it acts on it.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0",
		ToolTip="How fast sight confidence fades per second when the target is not currently visible. Higher = forgets a lost target faster."))
	float ConfidenceDecayPerSecond = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0",
		ToolTip="How fast pursuit urgency fades per second. Higher = calms down faster after a stimulus."))
	float UrgencyDecayPerSecond = 0.12f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Confidence below this forgets the target and falls through to idle/wander/horde. Higher = gives up sooner."))
	float LostSightMemoryThreshold = 0.10f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="How long a heard location stays actionable for investigation. Higher = investigates older noises."))
	float HeardMemorySeconds = 12.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Urgency at/above which a heard noise causes InvestigateLocation; below this the Echo only turns toward it."))
	float HeardInvestigateUrgency = 0.40f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Confidence restored when sight is reacquired on a previously lost target."))
	float ReacquireSightConfidence = 1.0f;

	// ── Echo|Sight ─────────────────────────────────────────────────────────────
	// Active perception sight config + last-seen projection. Applied to the active
	// controller's sight sense; projection clamps keep prediction non-supernatural.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Active sight radius. Targets are confirmed visible within this range with line of sight."))
	float ActiveSightRadius = 1200.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Active lose-sight radius. Must be >= ActiveSightRadius; the gap provides sight hysteresis."))
	float ActiveLoseSightRadius = 1500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ClampMax="180.0", ForceUnits="deg",
		ToolTip="Half-angle of peripheral vision. 90 = 180-degree forward cone. Wide+dim: walkers catch movement across a broad arc but resolve nothing at distance."))
	float ActivePeripheralVisionAngleDegrees = 100.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Max age of a sight stimulus before perception forgets it."))
	float ActiveSightMaxAgeSeconds = 3.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Lead time used to project the last observed travel into a search anchor."))
	float LastSeenProjectionSeconds = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Clamp on projected-direction distance so prediction can never be supernatural."))
	float MaxLastSeenProjectionDistance = 400.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Smoothing for stored observed velocity. 1 = snap to newest observation; lower = smoother. Only updates while the target is actually visible."))
	float LastSeenVelocitySmoothingAlpha = 0.5f;

	// ── Echo|Acoustics ─────────────────────────────────────────────────────────
	// Real-unit sound propagation (see ATR_EchoAcoustics.h). Sounds are authored as dB SPL
	// at the reference distance; received level falls off with inverse-square spreading
	// (−6 dB per doubling of distance) plus linear atmospheric absorption. Audible radius
	// is DERIVED — never authored. This free-field model is the seam where sound ray
	// tracing (occlusion/diffraction path loss) plugs in later.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Acoustics", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Reference distance at which a source's LoudnessDb is specified. 100 cm = 1 m, the standard SPL spec distance."))
	float AcousticReferenceDistanceCm = 100.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Acoustics", meta=(ClampMin="0.0", ClampMax="194.0", ForceUnits="dB",
		ToolTip="Hearing threshold: received level an Echo needs to notice a sound at all. Roughly the ambient noise floor of the world — sounds arriving below this are masked."))
	float EchoHearingThresholdDb = 30.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Acoustics", meta=(ClampMin="0.0", ClampMax="194.0", ForceUnits="dB",
		ToolTip="Received level at/above which a sound registers at full perceived intensity (normalized strength 1.0). Between threshold and saturation, intensity scales linearly in dB."))
	float EchoHearingSaturationDb = 95.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Acoustics", meta=(ClampMin="0.0", ClampMax="10.0",
		ToolTip="Atmospheric absorption in dB per 100 m, on top of inverse-square spreading. ~0.5 approximates mid-frequency air absorption; raise for muffled/foggy atmospheres."))
	float AirAbsorptionDbPer100m = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Acoustics", meta=(ClampMin="100.0", ForceUnits="cm",
		ToolTip="Hard cap on any sound's audible radius, bounding stimulus fan-out queries regardless of source loudness."))
	float MaxAudibleRangeCm = 50000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Acoustics", meta=(ClampMin="0.0", ClampMax="194.0", ForceUnits="dB",
		ToolTip="Source level assumed for an AIPerception noise event reported with Loudness 1.0 (e.g. MakeNoise). Loudness multipliers shift this by 20·log10(loudness)."))
	float DefaultPerceivedNoiseLoudnessDb = 70.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Acoustics", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Imperfect hearing: maximum positional error in an Echo's estimate of a sound's origin, as a fraction of its distance to the source. Applies fully at barely-audible levels and shrinks to zero at saturation — so a distant gunshot draws Echoes to scattered nearby estimates (forming several converging hordes) instead of one exact point."))
	float HearingMaxLocationErrorFraction = 0.30f;

	// ── Echo|Hearing ───────────────────────────────────────────────────────────
	// Hearing is location-only. These scale noise strength into urgency/agitation and
	// gate whether a noise causes a turn or a full investigation.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Active hearing range. Single source of truth for the hearing sense and the distance falloff applied to heard noises."))
	float ActiveHearingRange = 5000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Max age of a hearing stimulus before perception forgets it."))
	float ActiveHearingMaxAgeSeconds = 8.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0",
		ToolTip="Scale from noise strength (post-falloff) to urgency."))
	float NoiseStrengthToUrgencyScale = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0",
		ToolTip="Scale from noise strength to local agitation contribution."))
	float NoiseStrengthToAgitationScale = 0.25f;

	// ── Echo|Search ────────────────────────────────────────────────────────────
	// Lost-sight search tuning. Search points are nav-projected before becoming moves.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Arrival tolerance for memory/search points."))
	float ReachLocationRadius = 120.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Default search radius used when no velocity projection is available."))
	float SearchDefaultRadius = 600.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Maximum time a search persists before giving up."))
	float SearchMaxDurationSeconds = 12.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Acceptance radius for advancing to the next fan step."))
	float SearchStepAcceptanceRadius = 120.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Navmesh projection radius for generated search points. A point that cannot project within this radius is skipped."))
	float SearchPointNavProjectionRadius = 500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ForceUnits="deg",
		ToolTip="Per-Echo angular jitter applied to the search direction so a group fans out."))
	float SearchRandomAngleDegrees = 20.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="1",
		ToolTip="Maximum number of fan steps before a search is considered exhausted."))
	int32 MaxSearchSteps = 5;

	// Per-echo search variation: radius = base × (RadiusBaseScale + RadiusPerEchoVariation × hash
	// + RadiusAggressionBonus × aggression); duration likewise. Spread keeps a searching group
	// from sweeping identical robotic arcs.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Base fraction of the computed search radius every echo gets."))
	float SearchRadiusBaseScale = 0.8f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Per-echo random variation added to the search radius scale (0..this, stable per echo)."))
	float SearchRadiusPerEchoVariation = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Extra search radius scale at maximum aggression — aggressive echoes sweep wider."))
	float SearchRadiusAggressionBonus = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Base fraction of the configured search duration every echo gets."))
	float SearchDurationBaseScale = 0.7f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Per-echo random variation added to the search duration scale (0..this, stable per echo)."))
	float SearchDurationPerEchoVariation = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Extra search duration scale at maximum aggression — aggressive echoes persist longer."))
	float SearchDurationAggressionBonus = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(
		ToolTip="Default search pattern asset. Leave empty to use the built-in standard directional fan."))
	TSoftObjectPtr<UATR_EchoSearchPatternDataAsset> DefaultSearchPattern;

	// ── Echo|Agitation ─────────────────────────────────────────────────────────
	// Indirect horde field. Sources deposit a scalar + direction; never a target actor.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="World size of one agitation cell. Larger = coarser, cheaper horde field."))
	float MomentumCellSize = 2000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0",
		ToolTip="How fast deposited field pressure fades per second."))
	float AgitationFieldDecayPerSecond = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0",
		ToolTip="How fast an individual Echo's personal agitation fades per second."))
	float EchoPersonalAgitationDecayPerSecond = 0.10f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Agitation at/above which an Echo becomes curious and orients toward the hotspot."))
	float HordeCuriosityThreshold = 0.20f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Agitation at/above which an Echo migrates with horde pressure."))
	float AgitationJoinThreshold = 0.50f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0",
		ToolTip="Scale from noise strength to agitation deposited into the field."))
	float NoiseAgitationAmountScale = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Agitation deposited by combat events."))
	float CombatAgitationAmount = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Distance an Echo moves per step when migrating under horde pressure."))
	float HordePressureMoveDistance = 800.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Smoothing for an Echo's horde pressure direction. 1 = snap; lower = smoother turns."))
	float HordePressureDirectionSmoothingAlpha = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Distance of the pseudo-target an agitated/curious echo turns toward along the horde pressure direction (orient-only intents)."))
	float HordeOrientTargetDistanceCm = 500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of HordeWalkSpeed a horde-tier echo moves at toward its own heard estimate when its urgency is ZERO; scales linearly to full speed at urgency 1."))
	float HeardSteerMinSpeedFraction = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of HordeWalkSpeed used when an echo is only de-clumping (separation with no horde flow or stimulus)."))
	float HordeSeparationOnlySpeedFraction = 0.35f;

	// Field diffusion + gradient shaping. Spread pressure across cells and steer by the smooth
	// pressure gradient (toward hotspots) instead of the raw deposited direction, so hordes react
	// from a distance and movement isn't snapped to cell/grid lines.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(
		ToolTip="Spread agitation into neighbouring cells every tick so pressure forms a smooth gradient instead of staying in the deposit cell. Turn OFF to see the old grid-locked behaviour."))
	bool bEnableMomentumDiffusion = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="20.0",
		ToolTip="How fast agitation propagates outward, in cells-worth of exchange per second. Higher = pressure reaches far echoes sooner and the 'doesn't leave the grid' look disappears. 0 disables spread."))
	float MomentumDiffusionRate = 3.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="180.0", ForceUnits="deg",
		ToolTip="Per-Echo random angular jitter added to field-driven movement so a group fans out instead of marching in lockstep along identical directions. 0 = no jitter (sharper patterns)."))
	float HordeDirectionJitterDegrees = 25.f;

	// ── Echo|HordeShaping ──────────────────────────────────────────────────────
	// Boids-style separation + approach jitter so echoes form an organic mass rather than a
	// perfect circle packed onto the player's exact location.

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(
		ToolTip="Enable short-range separation so nearby echoes push apart instead of converging on one point. Turn OFF to see the old perfect-ring packing."))
	bool bEnableHordeSeparation = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Echoes within this distance of each other push apart. Roughly one to two body widths works well."))
	float HordeSeparationRadius = 160.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ClampMax="4.0",
		ToolTip="Strength of separation relative to the seek/flow direction. Higher = looser, more spread-out crowd; too high becomes jittery and stops them closing in."))
	float HordeSeparationStrength = 0.85f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ClampMax="180.0", ForceUnits="deg",
		ToolTip="Per-Echo random angular jitter on the direct player-seek direction so close echoes approach from spread angles instead of forming a perfect circle."))
	float HordeApproachJitterDegrees = 20.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="1", ClampMax="64",
		ToolTip="Maximum neighbours considered when computing an echo's separation (performance cap)."))
	int32 HordeSeparationMaxNeighbors = 12;

	// Detachment — echoes peel off a horde from the edges, the tail, and as a constant trickle, so
	// hordes erode and eventually break up rather than persisting forever.

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(
		ToolTip="Enable echoes detaching/peeling off a moving horde (edge, tail, and random trickle). Off = once aligned, echoes follow momentum until it decays."))
	bool bEnableHordeDetachment = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0", ClampMax="32",
		ToolTip="An echo with fewer nearby neighbours than this counts as being on the EDGE of the horde and detaches more easily."))
	int32 DetachEdgeNeighborCount = 3;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="-1.0", ClampMax="1.0",
		ToolTip="An echo is at the BACK of the horde when the local crowd centre lies ahead of it along the momentum direction by at least this dot value. Lower = more echoes count as 'back'."))
	float DetachBackDot = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ClampMax="5.0",
		ToolTip="Per-second probability that an EDGE echo detaches and wanders off."))
	float DetachChanceEdgePerSec = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ClampMax="5.0",
		ToolTip="Per-second probability that a BACK (tail) echo detaches and wanders off."))
	float DetachChanceBackPerSec = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ClampMax="5.0",
		ToolTip="Constant per-second detach probability for ANY horde echo, so hordes always shed a few stragglers."))
	float DetachChanceRandomPerSec = 0.03f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeShaping", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Speed a detached echo drifts outward (away from the crowd) as it peels off."))
	float DetachDriftSpeed = 70.f;

	// ── Echo|HordeMomentum ─────────────────────────────────────────────────────
	// Movement-momentum model. Echoes don't share targets or deposit 'I saw the player' pressure;
	// instead, MOVING echoes build a momentum field that nearby echoes align to. Aligned movement
	// self-reinforces (more movers = stronger, longer-lived momentum) and a loud sound kicks a group
	// into motion to seed it. Sight only makes that one echo chase — its movement can still drag a
	// horde along via momentum, but it never reveals the player's location to others.

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(
		ToolTip="Master switch for the movement-momentum horde model. Off = no horde alignment (echoes act individually)."))
	bool bEnableHordeMomentum = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ClampMax="10.0",
		ToolTip="How fast aligned movement builds a cell's momentum per second. Higher = hordes coalesce faster."))
	float MomentumBuildRate = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ClampMax="10.0",
		ToolTip="Base momentum decay per second. Higher = hordes peter out sooner."))
	float MomentumDecayPerSecond = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="How much strong momentum resists its own decay (0..1). Higher = big, coherent hordes take much longer to dissipate than weak ones."))
	float MomentumPersistence = 0.85f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Minimum speed for an echo's movement to count toward building momentum. Filters out idle jitter."))
	float MomentumMoverSpeedThreshold = 30.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="1.0", ClampMax="200.0",
		ToolTip="Number of aligned movers in a cell for full-strength momentum build. This is why a loud sound that starts 30 moving builds far stronger momentum than 3 wandering echoes."))
	float MomentumRefMoverCount = 12.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Minimum local momentum strength before an echo will align/join the horde. Below this it acts on its own."))
	float MomentumAlignThreshold = 0.12f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ClampMax="10.0",
		ToolTip="How strongly CALM echoes resist being dragged along by passing horde momentum. The align threshold scales by (1 + this × (1 − agitation)) — an agitated echo (heard the gunshot) joins readily, while a calm echo far from the stimulus ignores the passing flow instead of being carried off with it."))
	float MomentumCalmResistance = 2.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.1", ClampMax="4.0",
		ToolTip="Maximum momentum magnitude a cell can hold."))
	float MomentumMaxStrength = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Earshot radius of a loud sound: echoes within this start moving toward it (seeding a horde)."))
	float SoundImpulseRadius = 6000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Initial speed an echo moves toward a heard sound (scaled by loudness & distance falloff)."))
	float SoundImpulseSpeed = 150.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|HordeMomentum", meta=(ClampMin="0.0", ClampMax="4.0",
		ToolTip="Scales the sound movement impulse by event strength."))
	float SoundImpulseStrengthScale = 1.0f;

	// ── Echo|LowerTierSimulation ───────────────────────────────────────────────
	// Budgets and speeds for LowDetail individual simulation and Abstract cell simulation.

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.1", ClampMax="60.0", ForceUnits="Hz",
		ToolTip="Update frequency for budgeted LowDetail individual Echoes."))
	float LowDetailUpdateHz = 8.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.1", ClampMax="60.0", ForceUnits="Hz",
		ToolTip="Update frequency for Abstract cell-level simulation."))
	float AbstractUpdateHz = 1.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="1",
		ToolTip="Max LowDetail Echoes updated per tick (budget cap)."))
	int32 LowDetailMaxUpdatesPerTick = 256;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="1",
		ToolTip="Max Abstract cells updated per tick (budget cap)."))
	int32 AbstractMaxCellsPerTick = 64;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Radius a LowDetail Echo queries for nearby stimuli/field each update."))
	float LowDetailStimulusQueryRadius = 3000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Movement speed for a LowDetail Echo investigating a location."))
	float LowDetailInvestigateSpeed = 150.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Movement speed for a LowDetail Echo wandering/drifting."))
	float LowDetailWanderSpeed = 60.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Movement speed for a LowDetail Echo running a simplified search."))
	float LowDetailSearchSpeed = 120.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Search radius for the simplified LowDetail search."))
	float LowDetailSearchRadius = 600.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Duration of the simplified LowDetail search before giving up."))
	float LowDetailSearchDurationSeconds = 12.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0",
		ToolTip="Promotion-score weight applied per unit of urgency for LowDetail Echoes."))
	float LowDetailPromotionUrgencyBoost = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0",
		ToolTip="Promotion-score weight applied per unit of agitation for LowDetail Echoes."))
	float LowDetailPromotionAgitationBoost = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of an Abstract cell's population that migrates toward high-pressure neighbors per update."))
	float AbstractCellMigrationRate = 0.05f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0",
		ToolTip="Scale from cell noise memory to migration attraction."))
	float AbstractCellNoiseAttractionScale = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|LowerTierSimulation", meta=(ClampMin="0.0",
		ToolTip="Scale from cell agitation to migration attraction."))
	float AbstractCellAgitationAttractionScale = 1.0f;

	// ── Echo|ActivePursuit ─────────────────────────────────────────────────────
	// Line-of-desire pursuit (design doc: Active Prey-Driven Pursuit). During active
	// pursuit the Echo moves directly along the stimulus vector with short validated
	// steps; the navmesh is a movement VALIDATOR (ground projection), never a route
	// planner. Blockers on the line of desire transition to barrier engagement.

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(
		ToolTip="Master switch for line-of-desire pursuit. ON (default, design-correct): chase/last-seen/search moves go straight toward the stimulus and engage blockers. OFF: legacy full-pathfinding moves (Echoes intelligently route around fences/doors — for debugging only)."))
	bool bUseLineOfDesirePursuit = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Forward sweep distance along the desired direction used to detect meaningful blockers during direct pursuit."))
	float ActivePursuitForwardSweepDistanceCm = 150.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Sphere radius of the forward blocker sweep. 0 = line trace."))
	float ActivePursuitSweepRadiusCm = 30.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Navmesh projection radius used to validate that direct-pursuit movement stays on walkable ground. Validation only — never route planning."))
	float ActivePursuitGroundProjectionRadiusCm = 300.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(ClampMin="0.05", ForceUnits="s",
		ToolTip="If a direct-pursuit Echo makes less than ActivePursuitStuckProgressCm of progress for this long, whatever is ahead is classified as a blocker and engaged."))
	float ActivePursuitStuckTimeSeconds = 0.75f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Minimum progress over ActivePursuitStuckTimeSeconds for a direct-pursuit move to count as moving."))
	float ActivePursuitStuckProgressCm = 15.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Heard/smelled investigations within this distance use line-of-desire movement (strong NEARBY stimulus = active pursuit). Beyond it, broad navigation is allowed (distant investigation = ambient movement)."))
	float ActivePursuitDirectInvestigateDistanceCm = 2500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ActivePursuit", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="How head-on a swept TAGGED barrier hit must be (dot of desired direction vs inverse hit normal) to immediately trigger engagement. Glancing contacts wall-slide instead; the stuck timer still catches everything."))
	float ActivePursuitBlockerHeadOnDot = 0.35f;

	// ── Echo|Barrier ───────────────────────────────────────────────────────────
	// Barrier engagement (design doc: Engage Barrier / Reach Through Barrier /
	// Frustrated Search). Doors buy time but make noise; fences stop movement but not
	// attention; hordes overwhelm through pressure, not intelligence.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Contact range — the Echo closes to within this distance of the barrier before pressing/attacking."))
	float BarrierEngageDistanceCm = 150.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Default reach-through interaction distance for permeable barriers (chain-link fence, broken window). Barrier data assets can override per type."))
	float BarrierReachThroughDistanceCm = 180.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.05", ForceUnits="s",
		ToolTip="Seconds between barrier attacks while engaged."))
	float BarrierAttackIntervalSeconds = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Wind-up before the FIRST hit after reaching barrier contact, when no obstacle-behavior data asset overrides it."))
	float BarrierFirstAttackDelaySeconds = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0",
		ToolTip="Fallback damage per barrier hit when no obstacle-behavior or barrier data asset provides a value."))
	float BarrierDamagePerHit = 10.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0",
		ToolTip="Group pressure each engaged Echo contributes to a barrier per hit. Pressure from multiple Echoes stacks and scales damage — the horde overwhelms, it does not solve."))
	float BarrierPressurePerEcho = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0",
		ToolTip="How fast accumulated barrier pressure decays per second once Echoes stop hitting it."))
	float BarrierPressureDecayPerSecond = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0",
		ToolTip="Damage multiplier per unit of accumulated group pressure (EffectiveDamage = DamagePerHit * (1 + Pressure * this))."))
	float BarrierPressureDamageMultiplier = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ClampMax="194.0", ForceUnits="dB",
		ToolTip="Source loudness (dB SPL @ acoustic reference distance) of each barrier impact. Pounding on a door is ~85 dB. The audible radius derives from the Echo|Acoustics propagation model — banging attracts nearby Echoes, the sound feedback loop."))
	float BarrierImpactLoudnessDb = 85.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Maximum time an Echo keeps engaging a barrier after its stimulus confidence has gone stale, before flipping to frustrated search."))
	float MaxBarrierEngageSecondsWithoutStimulus = 10.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="How long the frustrated phase lingers near the barrier (shuffling, occasional hits) before decaying to idle/wander."))
	float FrustratedSearchDurationSeconds = 6.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Radius of the small random shuffle movements during frustrated search."))
	float FrustratedSearchRadiusCm = 300.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.1", ForceUnits="s",
		ToolTip="Seconds between the occasional barrier hits during frustrated search."))
	float FrustratedSearchHitIntervalSeconds = 3.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="1.0", ClampMax="5.0",
		ToolTip="A frustrated echo only lands its occasional tap when within BarrierEngageDistanceCm × this of the barrier."))
	float FrustratedSearchHitRangeMultiplier = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Minimum frustrated-shuffle distance as a fraction of FrustratedSearchRadiusCm (shuffle targets land between this fraction and the full radius)."))
	float FrustratedShuffleMinRadiusFraction = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Barrier", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Arrival tolerance for a frustrated-shuffle point before the echo picks a new one."))
	float FrustratedShuffleAcceptRadiusCm = 40.f;

	// ── Echo|ObstacleHooks ─────────────────────────────────────────────────────
	// Obstacle classification tuning. Sidestep/repath values remain ONLY for the
	// bAllowActivePursuitTacticalReroute debug override — never normal game behavior.

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(
		ToolTip="DEBUG/TESTING ONLY. When true, a blocked pursuit falls back to the legacy sidestep/repath instead of barrier engagement. The design doc requires this to be false in normal play: a blocked Echo engages the blocker, it never solves the layout."))
	bool bAllowActivePursuitTacticalReroute = false;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Forward trace length used to identify what physically blocked a move."))
	float ObstacleForwardTraceLength = 200.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="How long a fresh block keeps EngageBarrier active without a new impact before falling through to other intents."))
	float ObstacleHandleTimeoutSeconds = 2.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Lateral sidestep distance — used ONLY by the bAllowActivePursuitTacticalReroute debug override."))
	float ObstacleSidestepDistance = 300.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Forward nudge applied alongside the sidestep — used ONLY by the bAllowActivePursuitTacticalReroute debug override."))
	float ObstacleForwardNudgeDistance = 100.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(
		ToolTip="Default obstacle behavior asset (per-Echo barrier interaction capabilities). Leave empty for built-in defaults."))
	TSoftObjectPtr<UATR_EchoObstacleBehaviorDataAsset> DefaultObstacleBehavior;

	// ── Echo|Combat ────────────────────────────────────────────────────────────
	// Melee grab → bite → pull. Promoted echoes keep moving forward (momentum preserved) and, when
	// within reach, attempt to grab; once grabbed they bite on a cadence and pull the target in.
	// Resolution (success/failure, grip strength, wound severity, failed-grab scratches) is fully
	// C++-owned on AATR_ActiveEcho — these are the tuning knobs for that math.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(
		ToolTip="Master switch for melee grab/bite. Off = promoted echoes only chase, never attack."))
	bool bEnableMeleeAttack = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="A seen target within this range flips the echo's intent from Chase to Attack (engages the melee task). Keep >= GrabRange so the attack state is active a little before arm's length."))
	float MeleeAttackRange = 220.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Arm's length — within this the echo attempts a grab (while still moving forward)."))
	float GrabRange = 160.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Minimum seconds between grab attempts."))
	float GrabCooldownSeconds = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="1.0",
		ToolTip="The grab releases if the target gets farther than GrabRange * this multiplier."))
	float GrabReleaseMultiplier = 1.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Once grabbed, the echo bites when the target is within this range. Clamped to <= GrabRange."))
	float BiteRange = 110.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Minimum seconds between bites."))
	float BiteCooldownSeconds = 1.2f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0",
		ToolTip="Strength passed to the pawn's PullTarget hook while grabbed (your pull/root-motion logic scales off this)."))
	float MeleePullStrength = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.5", ClampMax="120.0", ForceUnits="s",
		ToolTip="Maximum seconds a single grab can persist before forced release. Prevents bBlockDemotion from stranding when target stays in a BadAngle/cone-fail loop close to the echo."))
	float MaxGrabHoldSeconds = 15.f;

	// ── Echo|Combat — resolution (grab/bite outcomes, server-side C++) ─────────
	// Resolved entirely on the server through the Echo health model: missing
	// arms can't grab, missing fingers grab weak, no jaw = no bite.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="360.0", ForceUnits="deg",
		ToolTip="Full facing cone for a grab attempt — target outside it = BadAngle (no grab, no scratch)."))
	float GrabFacingConeDegrees = 140.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Chance a physically possible, well-angled grab still whiffs (clumsy dead hands)."))
	float GrabMissChance = 0.30f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Chance a whiffed grab still rakes fingers across the target (scratch wound)."))
	float ScratchOnMissChance = 0.35f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Sustained pull acceleration toward the echo (velocity gained per second of pulling) at MeleePullStrength 1 with a strong grip. Weak grips scale by WeakGripPullScale."))
	float MeleePullSpeed = 220.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Pull-speed scale when the grip is weak (missing fingers / one hand)."))
	float WeakGripPullScale = 0.5f;

	// --- Grab hold + struggle (player break-free) ---
	// While a player is grabbed, each grip DRAGS them (a movement-speed penalty that stacks per
	// grab) and PULLS them toward the echo. Breaking free is active effort: mash the struggle
	// input to fill a meter that decays over time; cross a grip-strength-scaled threshold to rip
	// one grip loose. Heavy drag, escapable solo — lethal when swarmed. Resolved on AATR_Player.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.05", ClampMax="1.0",
		ToolTip="MaxWalkSpeed multiplier applied to a grabbed player PER grip held. Stacks multiplicatively: one grab at 0.45 = 45% speed, two ≈ 20%, three ≈ 9%. Lower = heavier drag."))
	float GrabHoldSpeedScale = 0.45f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Movement-input pull toward the grabbing echo(es), applied to a grabbed player each frame as a fraction of full input. 0 = drag only, no pull-in."))
	float GrabPullInputScale = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.5",
		ToolTip="Struggle points needed to rip loose a STRONG grip. Higher = more mashing to escape."))
	float StruggleBreakThresholdStrong = 6.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.5",
		ToolTip="Struggle points needed to rip loose a WEAK grip. Clamped to <= the strong threshold; weak grips pop first when swarmed."))
	float StruggleBreakThresholdWeak = 3.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.01",
		ToolTip="Struggle points added per struggle-input press (one mash)."))
	float StrugglePressGain = 1.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0",
		ToolTip="Struggle points lost per second — forces sustained mashing instead of one tap. 0 = progress never decays."))
	float StruggleDecayPerSecond = 2.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="After a player rips a grip loose, that echo is staggered (cannot re-grab) for this long — the window to actually get away."))
	float GrabBreakStaggerSeconds = 1.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(
		ToolTip="Weapon profile for Echo bites (DamageType Bite + contamination). REQUIRED: no profile = no bite damage applied. Severity/penetration/contamination live on the profile; tier roll only scales severity via EventScale."))
	TSoftObjectPtr<UATR_WeaponDamageProfile> BiteDamageProfile;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Bite tier roll with a STRONG grip: chance the bite is a full laceration."))
	float BiteLacerationChanceStrongGrip = 0.45f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Bite tier roll with a STRONG grip: chance of a deep scratch (rolled after laceration)."))
	float BiteDeepScratchChanceStrongGrip = 0.35f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Bite tier roll with a WEAK grip: chance the bite is a full laceration."))
	float BiteLacerationChanceWeakGrip = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Bite tier roll with a WEAK grip: chance of a deep scratch (rolled after laceration)."))
	float BiteDeepScratchChanceWeakGrip = 0.35f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="EventScale applied to BiteDamageProfile for a Scratch tier (failed-grab rake or weakest bite). Damage values come from the profile; this only scales."))
	float ScratchTierScale = 0.3f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="EventScale applied to BiteDamageProfile for a Deep Scratch tier."))
	float DeepScratchTierScale = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="EventScale applied to BiteDamageProfile for a Laceration tier (full bite)."))
	float LacerationTierScale = 1.0f;

	// ── Echo|Combat — structural movement effects ─────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="MaxWalkSpeed scale for a promoted echo that can walk but not run (one bad leg — limp)."))
	float LimpSpeedScale = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="MaxWalkSpeed for a promoted echo that can only crawl."))
	float CrawlSpeed = 60.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="deg",
		ToolTip="Yaw turn rate (deg/sec) for the TurnTowardStimulus orient task."))
	float OrientTurnRateDegPerSec = 90.f;

	// --- Shoulder barge (player pushes through an echo) ---
	// A fast-moving player colliding with an echo can knock it aside and break its grip.
	// Power = (speed / reference) × (player mass / echo mass) × glancing factor / echo strength.
	// Dead-center hits are hardest; clipping a shoulder is easiest. Resolved server-side in
	// AATR_ActiveEcho::NotifyHit.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Minimum player speed for a collision to count as a barge attempt at all. Below this the echo just blocks."))
	float BargeMinSpeed = 300.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="1.0", ForceUnits="cm/s",
		ToolTip="Player speed at which the speed term of barge power equals 1.0 (≈ sprint speed)."))
	float BargeReferenceSpeed = 450.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0",
		ToolTip="Barge power required to knock the echo aside. Power below this = the echo holds its ground (and its grip)."))
	float BargeSuccessPowerThreshold = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Glancing factor for a DEAD-CENTER hit (angle of attack). 0.35 = running straight through an echo's torso is ~3x harder than clipping its shoulder (factor 1.0 at the capsule edge)."))
	float BargeCenterEffectiveness = 0.35f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Base knockback speed applied to a successfully barged echo (scaled by barge power, mostly sideways out of the player's path)."))
	float BargeKnockbackSpeed = 450.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="How long a barged echo is staggered: it cannot grab, and any held grip is released. Scaled up by barge power."))
	float BargeStaggerSeconds = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of player speed lost barging dead-center through an echo. Glancing hits lose proportionally less (down to ~none at the shoulder). Lowering the shoulder costs momentum."))
	float BargePlayerSpeedLossAtCenter = 0.45f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Per-echo cooldown between barge resolutions, so one sustained contact doesn't re-resolve every frame."))
	float BargeCooldownSeconds = 0.4f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="-1.0", ClampMax="1.0",
		ToolTip="Minimum dot between the player's movement direction and the direction to the echo for a collision to count as running INTO it (vs brushing past)."))
	float BargeMinApproachDot = 0.2f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Sideways component of barge knockback (out of the player's path). Mixed with the forward component, then normalized."))
	float BargeKnockbackSideMix = 0.8f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Forward (carry-through) component of barge knockback. Mixed with the sideways component, then normalized."))
	float BargeKnockbackForwardMix = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ForceUnits="cm/s",
		ToolTip="Small upward velocity added to barge knockback so the echo visibly staggers off its feet."))
	float BargeKnockbackUpSpeed = 60.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.01", ClampMax="1.0",
		ToolTip="Lower clamp on the player/echo mass ratio in barge power — a feather-light player still has SOME shove."))
	float BargeMassRatioMin = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="1.0", ClampMax="20.0",
		ToolTip="Upper clamp on the player/echo mass ratio in barge power — a heavy player can't trivialize every echo."))
	float BargeMassRatioMax = 4.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="1.0", ClampMax="10.0",
		ToolTip="Cap on resolved barge power when scaling knockback speed and stagger duration."))
	float BargePowerCap = 2.f;

	// --- Body condition distribution (rolled deterministically per echo on promotion) ---

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of echoes with both arms healthy (full grip range). Healthy + MissingFingers + MissingHand should be <= 1; the remainder spawn with no arms."))
	float BodyHealthyChance = 0.55f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of echoes missing fingers (weak grip only, scratches less often)."))
	float BodyMissingFingersChance = 0.20f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of echoes missing a hand (grabs less reliably, weak grip only)."))
	float BodyMissingHandChance = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Minimum per-echo muscle power. Scales strong-grip chance, pull force, and barge resistance."))
	float BodyStrengthScalarMin = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="2.0",
		ToolTip="Maximum per-echo muscle power."))
	float BodyStrengthScalarMax = 1.4f;

	// --- Grip / grab condition modifiers ---

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Fraction of pull force transmitted through a WEAK grip (a strong grip transmits 100%)."))
	float WeakGripPullTransmission = 0.45f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Hard cap on strong-grip chance after the StrengthScalar multiplier."))
	float StrongGripChanceCap = 0.95f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Grab-chance multiplier for an echo with missing fingers."))
	float GrabMissingFingersMultiplier = 0.90f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Grab-chance multiplier for an echo with a missing hand."))
	float GrabMissingHandMultiplier = 0.75f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Multiplier on the failed-grab scratch chance for fingerless echoes."))
	float ScratchFingerlessMultiplier = 0.5f;

	// --- Bite severity tables (cumulative roll thresholds; remainder = Laceration) ---

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="STRONG grip: rolls below this are a Scratch."))
	float BiteStrongScratchUpTo = 0.20f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="STRONG grip: rolls below this (and above the scratch threshold) are a DeepScratch; the rest are Lacerations."))
	float BiteStrongDeepScratchUpTo = 0.60f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="WEAK grip: rolls below this are a Scratch."))
	float BiteWeakScratchUpTo = 0.50f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Combat", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="WEAK grip: rolls below this (and above the scratch threshold) are a DeepScratch; the rest are Lacerations."))
	float BiteWeakDeepScratchUpTo = 0.85f;

	// ── Echo|Demotion ──────────────────────────────────────────────────────────
	// Guards that keep an actively engaged Echo from being demoted out of the active pool.
	// MinTimeInTierSeconds (hysteresis) lives in Echo|Promotion and is shared with promotion.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Demotion", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Block demotion while confidence is at/above this."))
	float DemotionConfidenceBlockThreshold = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Demotion", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Block demotion while urgency is at/above this."))
	float DemotionUrgencyBlockThreshold = 0.5f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Demotion", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="A search younger than this counts as fresh and blocks demotion."))
	float DemotionSearchBlockSeconds = 5.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Demotion", meta=(
		ToolTip="Block demotion while the Echo currently sees its target."))
	bool bBlockDemotionDuringVisibleChase = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Demotion", meta=(
		ToolTip="Block demotion while the Echo is in a fresh lost-sight search."))
	bool bBlockDemotionDuringFreshSearch = true;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Demotion", meta=(
		ToolTip="Block demotion while the Echo is handling a fresh obstacle."))
	bool bBlockDemotionDuringObstacleHandling = true;

	// ── Echo|Promotion (scoring weights) ───────────────────────────────────────
	// Promotion is score-based, not purely distance-based, so an agitated/searching Echo just
	// outside the nearest ring can still earn a slot. All weights additive into the score.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(ClampMin="0.0",
		ToolTip="Bonus added to promotion score for Echoes inside MustPromoteRadius."))
	float MustPromoteScoreBonus = 1000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(ClampMin="0.0",
		ToolTip="Promotion-score weight per unit of urgency."))
	float PromotionUrgencyBoost = 200.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(ClampMin="0.0",
		ToolTip="Promotion-score weight per unit of confidence."))
	float PromotionConfidenceBoost = 150.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(ClampMin="0.0",
		ToolTip="Promotion-score weight per unit of agitation."))
	float PromotionAgitationBoost = 150.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(ClampMin="0.0",
		ToolTip="Promotion-score weight for an Echo in the player's forward hemisphere (visibility relevance)."))
	float PromotionPlayerFacingBoost = 100.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(ClampMin="0.0",
		ToolTip="Promotion-score penalty subtracted from a recently demoted Echo, decaying over RecentlyDemotedSeconds."))
	float RecentlyDemotedPenalty = 300.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Promotion", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Window over which the recently-demoted penalty applies and decays."))
	float RecentlyDemotedSeconds = 3.f;
};
