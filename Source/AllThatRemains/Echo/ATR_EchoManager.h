// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ATR_EchoManager.generated.h"

class UATR_EchoSubsystem;

// Quantized Echo state for net transport — 10 bytes per entity.
// XY encoded over [-WorldHalfExtent, +WorldHalfExtent].
// Z encoded over [SnapshotZMin, SnapshotZMax].
USTRUCT()
struct FEchoSnapshot
{
	GENERATED_BODY()

	UPROPERTY() uint16 PosX  = 0;
	UPROPERTY() uint16 PosY  = 0;
	UPROPERTY() uint16 PosZ  = 0;
	UPROPERTY() uint16 Index = 0;  // mirrors SoA index on server and client
	UPROPERTY() uint8  Anim  = 0;
	UPROPERTY() uint8  Yaw   = 0;  // reserved for Phase 4 (facing direction)

	bool NetSerialize(FArchive& Ar, UPackageMap* /*Map*/, bool& bOutSuccess)
	{
		Ar << PosX << PosY << PosZ << Index << Anim << Yaw;
		bOutSuccess = true;
		return true;
	}
};

template<>
struct TStructOpsTypeTraits<FEchoSnapshot> : public TStructOpsTypeTraitsBase2<FEchoSnapshot>
{
	enum { WithNetSerializer = true };
};

// Network surface and ISM renderer for the Echo horde.
//
// Responsibilities:
//   Server/listen-server — packs FEchoSnapshot arrays and multicasts at SnapshotHz.
//   Client/listen-server — updates three LOD-tier ISMComponents from live or decoded SoA data.
//
// Owns no simulation data. Queries UATR_EchoSubsystem for positions each frame.
// Driven by UATR_EchoSubsystem::Tick — its own tick is disabled.
UCLASS()
class ALLTHATREMAINS_API AATR_EchoManager : public AActor
{
	GENERATED_BODY()

public:
	AATR_EchoManager();

	virtual void BeginPlay() override;

	// Called by UATR_EchoSubsystem::Tick after RebuildGrid.
	// Skipped on NM_DedicatedServer (no GPU).
	void UpdateISM(UATR_EchoSubsystem* Sub);

	// Called by UATR_EchoSubsystem::Tick — packs snapshots and multicasts.
	// NM_DedicatedServer and NM_ListenServer only.
	void ServerTick(UATR_EchoSubsystem* Sub, float DeltaTime);

	// --- ISM LOD tiers -------------------------------------------------------
	// Assign static meshes per-tier in a Blueprint subclass or the CDO.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|ISM")
	TObjectPtr<UInstancedStaticMeshComponent> ISM_Near;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|ISM")
	TObjectPtr<UInstancedStaticMeshComponent> ISM_Mid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|ISM")
	TObjectPtr<UInstancedStaticMeshComponent> ISM_Far;

	// Distance thresholds from the local player camera (in Unreal Units).
	// Edit via Project Settings > AllThatRemains > Echo Horde.
	UPROPERTY(BlueprintReadOnly, Category = "Echo|ISM")
	float NearDistance = 3000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|ISM")
	float MidDistance = 10000.f;
	
	UPROPERTY(BlueprintReadOnly, Category = "Echo|ISM")	
	bool bDebugShowIsm = false;
	// --- Replication config --------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Replication")
	float SnapshotHz = 20.f;

	// Z quantization range — must match on server and client.
	UPROPERTY(BlueprintReadOnly, Category = "Echo|Replication")
	float SnapshotZMin = -10000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Replication")
	float SnapshotZMax = 50000.f;
	



private:
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_EchoSnapshot(const TArray<FEchoSnapshot>& Snapshots, int32 TotalActiveCount);

	void ApplySnapshotsToSubsystem(const TArray<FEchoSnapshot>& Snapshots, int32 TotalActiveCount);

	static FEchoSnapshot EncodeEcho(FVector3f Pos, uint8 AnimState, uint16 Index,
	                                 float HalfExtent, float ZMin, float ZMax);
	static FVector3f     DecodeEcho(const FEchoSnapshot& S,
	                                 float HalfExtent, float ZMin, float ZMax);

	// Pre-allocated scratch — zero heap allocations per frame on the hot path.
	TArray<FTransform>    AllTransforms;    // parallel transform build
	TArray<uint8>         BucketIds;        // parallel LOD classification
	TArray<FTransform>    NearTransforms;
	TArray<FTransform>    MidTransforms;
	TArray<FTransform>    FarTransforms;
	TArray<FEchoSnapshot> SnapshotScratch;

	float SnapshotAccumulator = 0.f;
};
