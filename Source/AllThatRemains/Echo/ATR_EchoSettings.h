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
	// PostEditChangeProperty in-editor and may be called from runtime init as a
	// safety net so config files edited by hand can't violate invariants.
	void ValidateAndClamp();

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
	float HordeWalkSpeed = 120.f;

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
		ToolTip="Reserved for future debug visualization. Promoted Actor echoes are currently excluded from horde ISM to prevent duplicate visuals and stale transform rendering. Use Actor debug drawing for promoted echo debugging."
	))
	bool bDebugShowPromotedEchoISM = false;

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
	float ConfidenceDecayPerSecond = 0.15f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0",
		ToolTip="How fast pursuit urgency fades per second. Higher = calms down faster after a stimulus."))
	float UrgencyDecayPerSecond = 0.20f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Confidence below this forgets the target and falls through to idle/wander/horde. Higher = gives up sooner."))
	float LostSightMemoryThreshold = 0.05f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="How long a heard location stays actionable for investigation. Higher = investigates older noises."))
	float HeardMemorySeconds = 8.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Urgency at/above which a heard noise causes InvestigateLocation; below this the Echo only turns toward it."))
	float HeardInvestigateUrgency = 0.40f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="How long a smell/blood location stays actionable. Higher = follows older trails."))
	float SmellMemorySeconds = 12.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Urgency at/above which a smell stimulus causes investigation rather than mild orientation."))
	float SmellInvestigateUrgency = 0.35f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Confidence restored when sight is reacquired on a previously lost target."))
	float ReacquireSightConfidence = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Awareness", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Grace period after sight is first lost before the Echo commits to memory/search. Smooths brief occlusions."))
	float SightLossGraceSeconds = 0.5f;

	// ── Echo|Sight ─────────────────────────────────────────────────────────────
	// Active perception sight config + last-seen projection. Applied to the active
	// controller's sight sense; projection clamps keep prediction non-supernatural.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Active sight radius. Targets are confirmed visible within this range with line of sight."))
	float ActiveSightRadius = 2000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Active lose-sight radius. Must be >= ActiveSightRadius; the gap provides sight hysteresis."))
	float ActiveLoseSightRadius = 2500.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ClampMax="180.0", ForceUnits="deg",
		ToolTip="Half-angle of peripheral vision. 90 = 180-degree forward cone."))
	float ActivePeripheralVisionAngleDegrees = 90.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Max age of a sight stimulus before perception forgets it."))
	float ActiveSightMaxAgeSeconds = 5.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Lead time used to project the last observed travel into a search anchor."))
	float LastSeenProjectionSeconds = 2.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Clamp on projected-direction distance so prediction can never be supernatural."))
	float MaxLastSeenProjectionDistance = 800.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Sight", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Smoothing for stored observed velocity. 1 = snap to newest observation; lower = smoother. Only updates while the target is actually visible."))
	float LastSeenVelocitySmoothingAlpha = 0.5f;

	// ── Echo|Hearing ───────────────────────────────────────────────────────────
	// Hearing is location-only. These scale noise strength into urgency/agitation and
	// gate whether a noise causes a turn or a full investigation.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="Active hearing range. Single source of truth for the hearing sense and the distance falloff applied to heard noises."))
	float ActiveHearingRange = 3000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Max age of a hearing stimulus before perception forgets it."))
	float ActiveHearingMaxAgeSeconds = 5.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0",
		ToolTip="Scale from noise strength (post-falloff) to urgency."))
	float NoiseStrengthToUrgencyScale = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0",
		ToolTip="Scale from noise strength to local agitation contribution."))
	float NoiseStrengthToAgitationScale = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Below this post-falloff strength a noise only turns the Echo toward it."))
	float WeakNoiseTurnOnlyThreshold = 0.20f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Hearing", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="At/above this post-falloff strength a noise triggers a move-to investigation."))
	float StrongNoiseInvestigateThreshold = 0.40f;

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

	UPROPERTY(Config, EditAnywhere, Category="Echo|Search", meta=(
		ToolTip="Default search pattern asset. Leave empty to use the built-in standard directional fan."))
	TSoftObjectPtr<UATR_EchoSearchPatternDataAsset> DefaultSearchPattern;

	// ── Echo|Agitation ─────────────────────────────────────────────────────────
	// Indirect horde field. Sources deposit a scalar + direction; never a target actor.

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="1.0", ForceUnits="cm",
		ToolTip="World size of one agitation cell. Larger = coarser, cheaper horde field."))
	float AgitationCellSize = 2000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0",
		ToolTip="How fast deposited field pressure fades per second."))
	float AgitationFieldDecayPerSecond = 0.25f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0",
		ToolTip="How fast an individual Echo's personal agitation fades per second."))
	float EchoPersonalAgitationDecayPerSecond = 0.10f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Agitation at/above which an Echo becomes curious and orients toward the hotspot."))
	float HordeCuriosityThreshold = 0.20f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Agitation at/above which an Echo migrates with horde pressure."))
	float AgitationJoinThreshold = 0.50f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Agitation deposited into the field by an Echo that currently sees a target."))
	float SightAgitationAmount = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0",
		ToolTip="Scale from noise strength to agitation deposited into the field."))
	float NoiseAgitationAmountScale = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Agitation deposited by combat events."))
	float CombatAgitationAmount = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Radius over which an Echo-sourced agitation spreads into the field."))
	float EchoAgitationSpreadRadius = 2000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Distance an Echo moves per step when migrating under horde pressure."))
	float HordePressureMoveDistance = 800.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Agitation", meta=(ClampMin="0.0", ClampMax="1.0",
		ToolTip="Smoothing for an Echo's horde pressure direction. 1 = snap; lower = smoother turns."))
	float HordePressureDirectionSmoothingAlpha = 0.5f;

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

	// ── Echo|ObstacleHooks ─────────────────────────────────────────────────────
	// Obstacle classification + sidestep/repath tuning. Break/climb behavior lives in the
	// obstacle behavior DataAsset (future seam); only sidestep/repath is active today.

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Forward trace length used to identify what physically blocked a move."))
	float ObstacleForwardTraceLength = 200.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Sphere-trace radius used when classifying a blocking obstacle. 0 = line trace."))
	float ObstacleTraceRadius = 34.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="How long a fresh block forces HandleObstacle before falling through to other intents."))
	float ObstacleHandleTimeoutSeconds = 2.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Lateral sidestep distance used by the obstacle fallback."))
	float ObstacleSidestepDistance = 300.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="cm",
		ToolTip="Forward nudge applied alongside the sidestep so the Echo presses the obstacle."))
	float ObstacleForwardNudgeDistance = 100.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(ClampMin="0.0", ForceUnits="s",
		ToolTip="Cooldown before retrying a failed obstacle handling attempt."))
	float ObstacleRetryCooldownSeconds = 1.0f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|ObstacleHooks", meta=(
		ToolTip="Default obstacle behavior asset. Leave empty for sidestep/repath only."))
	TSoftObjectPtr<UATR_EchoObstacleBehaviorDataAsset> DefaultObstacleBehavior;

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
