// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "ATR_EchoSubsystem.generated.h"

class AATR_EchoManager;
class AATR_ActiveEcho;

// CSR-style uniform spatial grid. Stores SoA indices — never source of truth.
// Rebuilt every frame from Positions SoA. Never owns entity data.
struct FATR_SpatialGrid
{
public:
	void  Initialize(FVector2D InWorldMin, FVector2D InWorldMax, float InCellSize);
	void  Rebuild(TArrayView<const FVector3f> Positions, int32 Count);
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
				const int32 C = CellIndex(Cx, Cy);
				for (int32 i = CellStart[C]; i < CellStart[C + 1]; ++i)
					Func(SortedEntities[i]);
			}
	}

	int32 GetNumCells()    const { return NumCellsX * NumCellsY; }
	bool  IsInitialized()  const { return NumCellsX > 0; }

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

	TArray<int32> CellStart;       // size = NumCells + 1
	TArray<int32> SortedEntities;  // size = live entity count
	TArray<int32> CellCounts;      // rebuild scratch
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
	float SpawnRadius = 5000.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 PoolSize = 20;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	int32 SimHz = 20;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float GridCellSize = 500.f;

	UPROPERTY(BlueprintReadOnly, Category = "Echo|Config")
	float WorldHalfExtent = 512000.f;

	// --- SoA (no UPROPERTY — worker-thread written, never GC-traced) ---
	// Parallel arrays, one entry per live entity [0, ActiveEntities).
	// Never resized after Initialize(). Capacity = InitializeCount.

	TArray<FVector3f> Forces;
	TArray<FVector3f> Accelerations;
	TArray<FVector3f> Velocities;
	TArray<FVector3f> Positions;
	TArray<uint8>     AnimState;
	TArray<uint8>     AnimFrame;
	int32             ActiveEntities = 0;

	// --- Public API ---

	int32 AddEcho(FVector3f Position);
	void  RemoveEcho(int32 Index);

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

	const FATR_SpatialGrid& GetSpatialGrid() const { return SpatialGrid; }
	bool                    IsGridReady()    const { return bGridReady; }

	// Called by AATR_EchoManager::BeginPlay on all machines — wires the Manager pointer
	// on clients (where the Subsystem didn't spawn the Manager itself).
	void SetManager(AATR_EchoManager* InManager) { Manager = InManager; }

private:
	void SimTick(float DeltaTime);
	void RebuildGrid();

	// Grid rebuilt every frame from Positions. Never source of truth.
	FATR_SpatialGrid SpatialGrid;

	// Reverse map: SoA index → promoted Actor. nullptr = in horde.
	// Raw observer pointers — lifetime guaranteed by EchoPool TObjectPtr.
	TArray<AATR_ActiveEcho*> IndexToActor;

	UPROPERTY()
	TObjectPtr<AATR_EchoManager> Manager;

	UPROPERTY()
	TArray<TObjectPtr<AATR_ActiveEcho>> EchoPool;

	float TickAccumulator = 0.f;
	bool  bGridReady      = false;
	bool  bInitialized    = false;
};
