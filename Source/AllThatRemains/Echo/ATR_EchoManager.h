// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ATR_EchoManager.generated.h"

class UATR_EchoSubsystem;

DECLARE_LOG_CATEGORY_EXTERN(LogATR_EchoNet,    Log, All);
DECLARE_LOG_CATEGORY_EXTERN(LogATR_EchoRender, Log, All);

// Quantized Echo state for net transport — 14 bytes per entity.
// XY encoded cell-relative: LocalX/Y in [0, CellSize].
// Z encoded world-relative over [-CellSize, CellSize*3] (no Z cell origin; grid is 2D).
USTRUCT()
struct FEchoSnapshot
{
	GENERATED_BODY()

	UPROPERTY() int32  EchoIndex = INDEX_NONE;  // authoritative SoA index
	UPROPERTY() int32  CellId    = INDEX_NONE;  // flat 2D spatial grid cell index
	UPROPERTY() uint16 LocalX    = 0;           // X offset from cell origin, quantized over [0, CellSize]
	UPROPERTY() uint16 LocalY    = 0;           // Y offset from cell origin, quantized over [0, CellSize]
	UPROPERTY() uint16 LocalZ    = 0;           // World Z, quantized over [-CellSize, CellSize*3]
	UPROPERTY() uint8  Anim      = 0;
	UPROPERTY() uint8  Yaw       = 0;

	bool NetSerialize(FArchive& Ar, UPackageMap* /*Map*/, bool& bOutSuccess)
	{
		Ar << EchoIndex << CellId << LocalX << LocalY << LocalZ << Anim << Yaw;
		bOutSuccess = true;
		return true;
	}
};

template<>
struct TStructOpsTypeTraits<FEchoSnapshot> : public TStructOpsTypeTraitsBase2<FEchoSnapshot>
{
	enum { WithNetSerializer = true };
};

UENUM()
enum class EEchoSnapshotKind : uint8
{
	Delta,
	Full,
	Correction,
	Despawn,
	CellEnter,
	CellExit
};

UENUM()
enum class EEchoRelevancyBand : uint8
{
	Near,
	Mid,
	Far
};

// Chunked wrapper for FEchoSnapshot arrays.
// Keeps each RPC within bunch-size limits. Sequence + ChunkIndex + ChunkCount allow
// the client to reassemble a multi-chunk send and detect missing chunks.
USTRUCT()
struct FEchoSnapshotChunk
{
	GENERATED_BODY()

	UPROPERTY() uint16 Sequence    = 0;
	UPROPERTY() uint8  ChunkIndex  = 0;
	UPROPERTY() uint8  ChunkCount  = 0;
	UPROPERTY() int32  TotalEchoes = 0;  // authoritative active count, sync'd to Sub->ActiveEntities

	UPROPERTY() EEchoSnapshotKind  SnapshotKind  = EEchoSnapshotKind::Full;
	UPROPERTY() EEchoRelevancyBand RelevancyBand = EEchoRelevancyBand::Near;
	UPROPERTY() int32              ViewId        = 0;

	UPROPERTY() TArray<FEchoSnapshot> Snapshots;

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
	{
		Ar << Sequence << ChunkIndex << ChunkCount << TotalEchoes;

		uint8 Kind = static_cast<uint8>(SnapshotKind);
		uint8 Band = static_cast<uint8>(RelevancyBand);
		Ar << Kind << Band;
		if (Ar.IsLoading())
		{
			SnapshotKind  = static_cast<EEchoSnapshotKind>(Kind);
			RelevancyBand = static_cast<EEchoRelevancyBand>(Band);
		}
		Ar << ViewId;

		int32 Count = Snapshots.Num();
		Ar << Count;
		if (Ar.IsLoading())
			Snapshots.SetNum(Count);

		for (FEchoSnapshot& S : Snapshots)
		{
			bool bOk = true;
			S.NetSerialize(Ar, Map, bOk);
			if (!bOk) { bOutSuccess = false; return false; }
		}

		bOutSuccess = true;
		return true;
	}
};

template<>
struct TStructOpsTypeTraits<FEchoSnapshotChunk> : public TStructOpsTypeTraitsBase2<FEchoSnapshotChunk>
{
	enum { WithNetSerializer = true };
};

// ISM renderer for the Echo horde.
// Updates three LOD-tier ISMComponents from local SoA data each frame.
// Owns no simulation data. Driven by UATR_EchoSubsystem::Tick — own tick disabled.
// Snapshot encoding/delivery is handled by UATR_EchoReplicationComponent (per-player).
UCLASS()
class ALLTHATREMAINS_API AATR_EchoManager : public AActor
{
	GENERATED_BODY()

public:
	AATR_EchoManager();

	virtual void BeginPlay() override;

	// Called by UATR_EchoSubsystem::Tick after RebuildFineGrid.
	// Skipped on NM_DedicatedServer (no GPU).
	void UpdateISM(UATR_EchoSubsystem* Sub);

	static constexpr int32 MaxEchoSnapshotsPerChunk = 256;

	// --- ISM LOD tiers -------------------------------------------------------
	// Assign static meshes per-tier in a Blueprint subclass or the CDO.

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|ISM")
	TObjectPtr<UInstancedStaticMeshComponent> ISM_Near;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|ISM")
	TObjectPtr<UInstancedStaticMeshComponent> ISM_Mid;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|ISM")
	TObjectPtr<UInstancedStaticMeshComponent> ISM_Far;

	// Offset applied to every ISM instance — corrects mesh pivot/facing vs. SoA world position.
	// Set in Blueprint class defaults to match the static mesh's local pivot.
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Echo|ISM")
	FVector ISMMeshLocationOffset = FVector::ZeroVector;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Echo|ISM")
	FRotator ISMMeshRotationOffset = FRotator::ZeroRotator;

	// --- Visual band distances (cached from UATR_EchoSettings in BeginPlay) ---
	// These come from the Echo|Rendering settings (VisualNearDistance, etc.) and
	// are deliberately NOT the Echo|Networking relevancy ranges. They drive what
	// the local client renders as horde ISM.

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Rendering")
	float NearBandDistance = 3000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Rendering")
	float MidBandDistance = 10000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Rendering")
	float FarBandDistance = 20000.f;

private:
	// Per-tier stable ISM slot state.
	struct FEchoISMTier
	{
		TMap<int32, int32>  EchoToInstance;  // SoA index → ISM instance slot
		TArray<int32>       InstanceToEcho;  // ISM instance slot → SoA index
		TMap<int32, uint32> EchoISMVersion;  // SoA index → DirtyState.Version at last update
		TArray<int32>       DirtyInstances;  // pending update indices (parallel with DirtyTransforms)
		TArray<FTransform>  DirtyTransforms;
	};

	FEchoISMTier                    Tiers[3];      // indexed by EEchoRelevancyBand (Near=0,Mid=1,Far=2)
	TMap<int32, EEchoRelevancyBand> EchoToBand;    // current tier for each echo in the ISM

	// Per-echo "seen this frame" stamp. EchoVisualStamp[i] == VisualFrameEpoch means
	// echo i was returned by the relevancy query this frame and should remain in the ISM.
	// Any echo in EchoToBand whose stamp is stale is removed (left the visual cutoff).
	// Sized to Sub->InitializeCount; grown only, never shrunk during play.
	TArray<uint32> EchoVisualStamp;
	uint32         VisualFrameEpoch = 1;

	// Frame-scope scratch for the relevancy query results and the removal pass.
	// Allocation persists across frames (Reset, not Empty) to avoid per-frame churn.
	TArray<int32> NearScratch;
	TArray<int32> MidScratch;
	TArray<int32> FarScratch;
	TArray<int32> RemoveScratch;

	UInstancedStaticMeshComponent* ISMForBand       (EEchoRelevancyBand Band) const;
	void                           AddEchoToISM     (int32 EchoIndex, EEchoRelevancyBand Band,
	                                                  const FTransform& T, uint32 Version);
	void                           RemoveEchoFromISM(int32 EchoIndex);
	void                           QueueTransformUpdate(int32 EchoIndex, EEchoRelevancyBand Band,
	                                                     const FTransform& T, uint32 Version);
	void                           FlushTierUpdates ();

	// Process a single relevancy band: stamp each returned echo, then add / band-swap /
	// queue-update its ISM instance. Skips promoted and invalid indices.
	void ProcessBand(UATR_EchoSubsystem* Sub, const TArray<int32>& Echoes,
	                 EEchoRelevancyBand DesiredBand);

	// Remove every ISM echo not stamped this frame (left the cutoff, invalid, or promoted).
	void RemoveUnstampedEchoes(UATR_EchoSubsystem* Sub);

	// Grow EchoVisualStamp to at least RequiredCapacity. Never shrinks during play.
	void EnsureVisualStampCapacity(int32 RequiredCapacity);

	// Apply hard performance-safe defaults that do not depend on project settings.
	// Called from the constructor so CDO/component defaults are safe even before settings are read.
	void ApplyISMSafeDefaults(UInstancedStaticMeshComponent* ISM);

	// Apply designer-configurable rendering flags from validated UATR_EchoSettings.
	// Called from BeginPlay after ValidateAndClamp().
	void ConfigureISMComponent(UInstancedStaticMeshComponent* ISM);
};
