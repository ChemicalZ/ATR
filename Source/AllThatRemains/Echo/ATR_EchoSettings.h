// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ATR_EchoSettings.generated.h"

class AATR_EchoManager;
class AATR_ActiveEcho;
class AATR_EchoAIController;

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
};
