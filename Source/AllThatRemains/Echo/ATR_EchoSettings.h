// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ATR_EchoSettings.generated.h"

class AATR_EchoManager;
class AATR_ActiveEcho;
class AATR_EchoAIController;

// Project Settings > AllThatRemains > Echo Horde
// All Echo system config lives here. C++ reads it once at init; never modify
// subsystem/manager properties directly.
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Echo Horde"))
class ALLTHATREMAINS_API UATR_EchoSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return FName("AllThatRemains"); }

	// ── Simulation ────────────────────────────────────────────────────────────

	// Max live entities. SoA pre-allocated to this at startup — never resized.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Simulation", meta=(ClampMin=1))
	int32 InitializeCount = 10000;

	// Entities seeded into SoA at BeginPlay (server/standalone only).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Simulation", meta=(ClampMin=0))
	int32 SpawnCount = 10;

	// Radius around the world origin within which SpawnCount echoes are scattered.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Simulation", meta=(ClampMin=0.f, ForceUnits="cm"))
	float SpawnRadius = 5000.f;

	// Authoritative sim steps per second (server/standalone).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Simulation", meta=(ClampMin=1, ClampMax=120, DisplayName="Sim Hz"))
	int32 SimHz = 20;

	// ── World ─────────────────────────────────────────────────────────────────

	// Spatial grid cell side length (Unreal Units).
	UPROPERTY(Config, EditAnywhere, Category="Echo|World", meta=(ClampMin=100.f, ForceUnits="cm"))
	float GridCellSize = 500.f;

	// Half-width of the simulated world square. Used for grid bounds and XY snapshot encoding.
	UPROPERTY(Config, EditAnywhere, Category="Echo|World", meta=(ClampMin=1000.f, ForceUnits="cm"))
	float WorldHalfExtent = 512000.f;

	// Radius around each player within which entities enter the fine spatial grid.
	// Must be >= FarRelevancyRange (default 25000) for QueryEchoesByRelevancyBands to
	// return all replication candidates. Must also be >= DemoteRadius.
	UPROPERTY(Config, EditAnywhere, Category="Echo|World", meta=(ClampMin=100.f, ForceUnits="cm"))
	float LocalZoneRadius = 27500.f;

	// Coarse global grid cell side length. Used for migration/density tracking.
	// Larger = less frequent incremental updates. Recommended: 30x+ GridCellSize.
	UPROPERTY(Config, EditAnywhere, Category="Echo|World", meta=(ClampMin=1000.f, ForceUnits="cm"))
	float CoarseGridCellSize = 15000.f;

	// ── Pool ──────────────────────────────────────────────────────────────────

	// ActiveEcho actors pre-warmed in the pool at BeginPlay (server only).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool", meta=(ClampMin=0))
	int32 PoolSize = 20;

	// Inner radius: entities here get pool slots before any outer-ring entities.
	// Overflow (unpromoted) entities within this radius also receive a steering force toward the player.
	// Must be strictly less than PromoteRadius.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool", meta=(ClampMin=100.f, ForceUnits="cm"))
	float MustPromoteRadius = 800.f;

	// Walk speed applied to unpromoted (ISM) entities within MustPromoteRadius as a steering force.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool", meta=(ClampMin=0.f, ForceUnits="cm/s"))
	float HordeWalkSpeed = 120.f;

	// Distance at which a horde entity is promoted to a full actor (server only).
	// Must be strictly less than DemoteRadius — the gap between them is the hysteresis band.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool", meta=(ClampMin=100.f, ForceUnits="cm"))
	float PromoteRadius = 2500.f;

	// Distance at which a promoted actor is demoted back to the horde.
	// Must be strictly greater than PromoteRadius.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool", meta=(ClampMin=100.f, ForceUnits="cm"))
	float DemoteRadius = 4000.f;

	// Minimum seconds an entity must spend in its current tier before a tier transition is allowed.
	// Prevents flip-flop when a player lingers near the boundary.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool", meta=(ClampMin=0.f))
	float MinTimeInTierSeconds = 1.5f;

	// Blueprint subclass of AATR_EchoManager. Leave empty to use the base C++ class.
	// Assign your BP here to get designer-configured ISM meshes.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool")
	TSoftClassPtr<AATR_EchoManager> ManagerClass;

	// Blueprint subclass of AATR_ActiveEcho. Leave empty to use the base C++ class.
	// Assign your BP here so pooled actors carry the correct mesh and AnimBP.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool")
	TSoftClassPtr<AATR_ActiveEcho> ActiveEchoClass;

	// Blueprint subclass of AATR_EchoAIController. Leave empty to use the base C++ class.
	// Assign your BP here so pooled controllers carry the correct StateTree asset.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool")
	TSoftClassPtr<AATR_EchoAIController> ControllerClass;

	// ── Rendering ─────────────────────────────────────────────────────────────
	// ISM LOD tier distances are driven by NearRelevancyRange/MidRelevancyRange (Echo|Networking),
	// keeping network relevancy and render LOD bands consistent.

	// Show ISM instance for echoes that are currently promoted to a full actor (debug only).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering")
	bool bDebugShowPromotedEchoISM = false;

	// ── Networking ────────────────────────────────────────────────────────────

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=1.f, ClampMax=60.f))
	float NearSnapshotHz = 10.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=0.1f, ClampMax=60.f))
	float MidSnapshotHz = 3.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=0.1f, ClampMax=60.f))
	float FarSnapshotHz = 1.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=100.f, ForceUnits="cm"))
	float NearRelevancyRange = 3000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=100.f, ForceUnits="cm"))
	float MidRelevancyRange = 10000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=100.f, ForceUnits="cm"))
	float FarRelevancyRange = 25000.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=1, ClampMax=512))
	int32 MaxSnapshotsPerChunk = 256;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=0.f))
	float FullResyncCooldownSeconds = 2.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=1))
	int32 MaxMissingSequencesBeforeResync = 3;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=0.f, ForceUnits="cm"))
	float PositionDirtyThreshold = 5.f;

	UPROPERTY(Config, EditAnywhere, Category="Echo|Networking", meta=(ClampMin=0.f))
	float YawDirtyThresholdDegrees = 2.f;
};
