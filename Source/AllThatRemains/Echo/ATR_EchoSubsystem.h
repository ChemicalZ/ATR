// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"
#include "Subsystems/WorldSubsystem.h"
#include "ATR_EchoSubsystem.generated.h"

class AATR_EchoManager;
class AATR_ActiveEcho;
class AATR_EchoAIController;

UENUM()
enum class EEchoDirtyFlags : uint8
{
	None      = 0,
	Transform = 1 << 0,
	Anim      = 1 << 1,
	Spawn     = 1 << 3,
	Despawn   = 1 << 4,
};
ENUM_CLASS_FLAGS(EEchoDirtyFlags)

struct FEchoDirtyState
{
	EEchoDirtyFlags Flags   = EEchoDirtyFlags::None;
	uint32          Version = 0; // increments each dirty marking; never wraps to 0
};

// Sparse uniform spatial grid. Only occupied cells allocate memory.
// Rebuilt each frame from a local-entity set (entities near players only).
// Never owns entity data. Member named SpatialGrid on the subsystem for
// backward compat with ATR_EchoReplicationComponent direct access.
struct FATR_SparseGrid
{
public:
	void  Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize);
	void  Reset();
	void  Build(TArrayView<const FVector3f> Positions, const TArray<int32>& LocalIndices);
	int32 QueryRadius(FVector3f QueryPos, float Radius,
	                  TArrayView<const FVector3f> Positions,
	                  TArray<int32>& OutIndices) const;

	template<typename FuncType>
	void ForEachInBounds(FVector2D Min, FVector2D Max, FuncType&& Func) const
	{
		const int32 MinCx = CellX(Min.X), MaxCx = CellX(Max.X);
		const int32 MinCy = CellY(Min.Y), MaxCy = CellY(Max.Y);
		for (int32 Cy = MinCy; Cy <= MaxCy; ++Cy)
			for (int32 Cx = MinCx; Cx <= MaxCx; ++Cx)
			{
				const int32 Key = CellIndex(Cx, Cy);
				if (const TArray<int32>* Bucket = Cells.Find(Key))
					for (int32 i : *Bucket)
						Func(i);
			}
	}

	int32 GetNumCells()   const { return NumCellsX * NumCellsY; }
	bool  IsInitialized() const { return NumCellsX > 0; }

	FORCEINLINE float GetCellSize() const { return CellSize; }

	FORCEINLINE bool IsValidCellId(int32 InCellId) const
	{
		return InCellId >= 0 && InCellId < NumCellsX * NumCellsY;
	}

	FORCEINLINE int32 GetCellId(FVector2f Pos2D) const
	{
		return CellIndex(CellX(Pos2D.X), CellY(Pos2D.Y));
	}

	FORCEINLINE FVector2f GetCellOrigin2D(int32 InCellId) const
	{
		const int32 Cx = InCellId % FMath::Max(1, NumCellsX);
		const int32 Cy = InCellId / FMath::Max(1, NumCellsX);
		return FVector2f(
			static_cast<float>(WorldMin.X) + Cx * CellSize,
			static_cast<float>(WorldMin.Y) + Cy * CellSize
		);
	}

private:
	FORCEINLINE int32 CellX(float X) const
	{
		return FMath::Clamp(static_cast<int32>((X - WorldMin.X) * InvCellSize), 0, NumCellsX - 1);
	}
	FORCEINLINE int32 CellY(float Y) const
	{
		return FMath::Clamp(static_cast<int32>((Y - WorldMin.Y) * InvCellSize), 0, NumCellsY - 1);
	}
	FORCEINLINE int32 CellIndex(int32 Cx, int32 Cy) const { return Cy * NumCellsX + Cx; }

	FVector2D WorldMin    = FVector2D::ZeroVector;
	float     CellSize    = 500.f;
	float     InvCellSize = 1.f / 500.f;
	int32     NumCellsX   = 0;
	int32     NumCellsY   = 0;

	TMap<int32, TArray<int32>> Cells;
	TArray<int32>              PopulatedCells;
};

// Global coarse grid tracking entity counts per large cell.
// Updated incrementally — only when an entity crosses a coarse cell boundary.
struct FATR_CoarseGrid
{
public:
	void Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize);

	FORCEINLINE int32 GetCellId(FVector2f Pos2D) const
	{
		return CellIndex(CellX(Pos2D.X), CellY(Pos2D.Y));
	}

	void OnEntityAdded(int32 CellId)
	{
		CellCounts.FindOrAdd(CellId)++;
	}

	void OnEntityRemoved(int32 CellId)
	{
		int32* Count = CellCounts.Find(CellId);
		if (!ensureAlways(Count)) return;
		if (--(*Count) == 0)
			CellCounts.Remove(CellId);
	}

	const TMap<int32, int32>& GetCellCounts() const { return CellCounts; }

private:
	FORCEINLINE int32 CellX(float X) const
	{
		return FMath::Clamp(static_cast<int32>((X - WorldMin.X) * InvCellSize), 0, NumCellsX - 1);
	}
	FORCEINLINE int32 CellY(float Y) const
	{
		return FMath::Clamp(static_cast<int32>((Y - WorldMin.Y) * InvCellSize), 0, NumCellsY - 1);
	}
	FORCEINLINE int32 CellIndex(int32 Cx, int32 Cy) const { return Cy * NumCellsX + Cx; }

	FVector2D WorldMin    = FVector2D::ZeroVector;
	float     CellSize    = 15000.f;
	float     InvCellSize = 1.f / 15000.f;
	int32     NumCellsX   = 0;
	int32     NumCellsY   = 0;

	TMap<int32, int32> CellCounts;
};

UCLASS()
class ALLTHATREMAINS_API UATR_EchoSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void    Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void    OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void    Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool    IsTickable() const override { return bInitialized; }

	// --- Config (read-only at runtime — edit via Project Settings > AllThatRemains > Echo Horde) ---

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 InitializeCount = 10000;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 SpawnCount = 10;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	TSubclassOf<AATR_EchoManager> ManagerClass;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	TSubclassOf<AATR_ActiveEcho> ActiveEchoClass;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	TSubclassOf<AATR_EchoAIController> ControllerClass;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float SpawnRadius = 5000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 PoolSize = 20;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 SimHz = 20;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float GridCellSize = 500.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float WorldHalfExtent = 512000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float MustPromoteRadius = 800.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float HordeWalkSpeed = 120.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float PromoteRadius = 2500.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float DemoteRadius = 4000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float MinTimeInTierSeconds = 1.5f;

	// --- SoA (no UPROPERTY — worker-thread written, never GC-traced) ---
	// Parallel arrays, one entry per live entity [0, ActiveEntities).
	// Never resized after Initialize(). Capacity = InitializeCount.

	TArray<FVector3f>     Forces;
	TArray<FVector3f>     Accelerations;
	TArray<FVector3f>     Velocities;
	TArray<FVector3f>     Positions;
	TArray<uint8>         AnimState;
	TArray<uint8>         AnimFrame;
	TArray<float>         PromotionTimes; // GetWorld()->GetTimeSeconds() at promotion; 0 = not promoted
	TArray<float>         Yaws;           // degrees [0, 360) — facing direction per entity
	TArray<FEchoDirtyState> DirtyStates;
	int32             ActiveEntities = 0;

	// --- Public API ---

	int32 AddEcho(FVector3f Position);
	void  RemoveEcho(int32 Index);

	// Immediately demote (if promoted) and remove from SoA. Use when an echo dies.
	// Bypasses bBlockDemotion and hysteresis — unconditional.
	void ForceDestroyEcho(int32 SoAIndex);
	void ForceDestroyEcho(AATR_ActiveEcho* Actor); // convenience overload for promoted echoes

	// Promote SoA entity to a pooled Actor for full simulation.
	// Caller must pop Actor from EchoPool first.
	void PromoteToActive(int32 SoAIndex, AATR_ActiveEcho* Actor);

	// Write Actor state back to SoA and return Actor to pool.
	void DemoteToHorde(AATR_ActiveEcho* Actor);

	// --- High-level pool API (call these from gameplay code) ---

	// Pop from pool, wire IndexToActor, seed from SoA, activate.
	// Returns nullptr and logs a warning if pool is empty.
	AATR_ActiveEcho* PromoteEcho(int32 SoAIndex);

	// Flush Actor state to SoA, deactivate, return to pool.
	void DemoteEcho(AATR_ActiveEcho* Actor);

	const FATR_SparseGrid& GetSpatialGrid() const { return SpatialGrid; }
	bool                    IsGridReady()    const { return bGridReady; }

	void MarkEchoDirty(int32 Index, EEchoDirtyFlags Flags);
	bool IsEchoDirty  (int32 Index, EEchoDirtyFlags Flags) const;

	float PositionDirtyThresholdSq = 25.f;  // set from settings in Initialize()
	float YawDirtyThresholdDeg     = 2.f;

	// Single FarRange grid query, results classified into three XY-distance bands.
	// OutNear/Mid/Far are non-overlapping. Appends to caller's arrays (caller owns Reset).
	void QueryEchoesByRelevancyBands(
		const FVector& Origin,
		float NearRange, float MidRange, float FarRange,
		TArray<int32>& OutNear, TArray<int32>& OutMid, TArray<int32>& OutFar) const;

	bool IsInitialized() const
	{
		return bInitialized                       &&
		       Positions.Num()  >= InitializeCount &&
		       Yaws.Num()       >= InitializeCount &&
		       AnimState.Num()  >= InitializeCount;
	}

	bool ValidateActiveArrays() const
	{
		return ActiveEntities >= 0                  &&
		       ActiveEntities <= InitializeCount    &&
		       Positions.Num()   >= ActiveEntities  &&
		       Yaws.Num()        >= ActiveEntities  &&
		       AnimState.Num()   >= ActiveEntities  &&
		       IndexToActor.Num() >= ActiveEntities;
	}

	// Called by AATR_EchoManager::BeginPlay on all machines — wires the Manager pointer
	// on clients (where the Subsystem didn't spawn the Manager itself).
	void SetManager(AATR_EchoManager* InManager) { Manager = InManager; }

	// Reverse map: SoA index → promoted Actor. nullptr = in horde.
	// Raw observer pointers — lifetime guaranteed by EchoPool TObjectPtr.
	UPROPERTY()
	TArray<AATR_ActiveEcho*> IndexToActor;

	// Compact list of SoA indices currently promoted to an Actor.
	// Maintained by PromoteEcho/DemoteEcho/RemoveEcho. Never has nullptr entries.
	TArray<int32> PromotedIndices;

	// SoA parallel array: current coarse cell per entity (INDEX_NONE = not yet registered).
	// Updated incrementally in UpdateCoarseGrid() on cell boundary crossings.
	TArray<int32> CoarseCellIds;

	void SimTick(float DeltaTime);
	void RebuildFineGrid();
	void UpdateCoarseGrid();

	// Fine local grid — rebuilt each tick from entities within LocalZoneRadius of any player.
	// Member name SpatialGrid preserved for ATR_EchoReplicationComponent direct access.
	FATR_SparseGrid SpatialGrid;

	// Global coarse grid — incremental count-only, updated on cell boundary crossings.
	FATR_CoarseGrid CoarseGrid;

	UPROPERTY()
	TObjectPtr<AATR_EchoManager> Manager;

	UPROPERTY()
	TArray<TObjectPtr<AATR_ActiveEcho>> EchoPool;

	UPROPERTY()
	TArray<TObjectPtr<AATR_EchoAIController>> ControllerPool;

	void RunSteeringPass();
	void RunPromotionPass();

	// Frame-scope scratch for local entity indices; allocation persists across ticks.
	TArray<int32> LocalEntityScratch;

	// Bit per SoA slot: was this entity in the local zone last frame?
	// Used to zero stale steering velocities when an entity exits the zone.
	TBitArray<> WasLocalLastFrame;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float LocalZoneRadius = 27500.f;  // must be >= FarRelevancyRange for replication queries

	float CoarseGridCellSize = 15000.f;

	float TickAccumulator = 0.f;
	bool  bGridReady      = false;
	bool  bInitialized    = false;
};
