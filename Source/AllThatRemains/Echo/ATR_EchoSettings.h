// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ATR_EchoSettings.generated.h"

class AATR_EchoManager;
class AATR_ActiveEcho;

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

	// ── Pool ──────────────────────────────────────────────────────────────────

	// ActiveEcho actors pre-warmed in the pool at BeginPlay (server only).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool", meta=(ClampMin=0))
	int32 PoolSize = 20;

	// Blueprint subclass of AATR_EchoManager. Leave empty to use the base C++ class.
	// Assign your BP here to get designer-configured ISM meshes.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool")
	TSoftClassPtr<AATR_EchoManager> ManagerClass;

	// Blueprint subclass of AATR_ActiveEcho. Leave empty to use the base C++ class.
	// Assign your BP here so pooled actors carry the correct mesh, AnimBP, and StateTree.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Pool")
	TSoftClassPtr<AATR_ActiveEcho> ActiveEchoClass;

	// ── Rendering ─────────────────────────────────────────────────────────────

	// Entities closer than this distance use ISM_Near (highest detail).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(ClampMin=0.f, ForceUnits="cm"))
	float NearDistance = 3000.f;

	// Entities between NearDistance and MidDistance use ISM_Mid.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Rendering", meta=(ClampMin=0.f, ForceUnits="cm"))
	float MidDistance = 10000.f;

	// ── Replication ───────────────────────────────────────────────────────────

	// Snapshot multicast rate. Must match on server and all clients.
	UPROPERTY(Config, EditAnywhere, Category="Echo|Replication", meta=(ClampMin=1.f, ClampMax=60.f, DisplayName="Snapshot Hz"))
	float SnapshotHz = 20.f;

	// Z quantization floor for snapshot encoding (must match server and client).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Replication", meta=(ForceUnits="cm"))
	float SnapshotZMin = -10000.f;

	// Z quantization ceiling for snapshot encoding (must match server and client).
	UPROPERTY(Config, EditAnywhere, Category="Echo|Replication", meta=(ForceUnits="cm"))
	float SnapshotZMax = 50000.f;
};
