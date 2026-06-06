// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ATR_EchoManager.h"
#include "ATR_EchoReplicationComponent.generated.h"

class UATR_EchoSubsystem;

// Per-player echo snapshot streaming component. Attach to PlayerController on the server.
// Server queries relevant echoes each tick, encodes them into bounded chunks, and sends
// unreliable client RPCs only to the owning connection. Client reassembles chunks and
// applies the decoded positions/state to its local EchoSubsystem.
UCLASS()
class ALLTHATREMAINS_API UATR_EchoReplicationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UATR_EchoReplicationComponent();

	virtual void BeginPlay() override;

	// Called server-side by UATR_EchoSubsystem::Tick for each active PlayerController.
	// Kept as legacy compatibility wrapper — subsystem scheduler calls ServerReplicateBandBudgeted directly.
	void ServerTickReplication(UATR_EchoSubsystem* Sub, float DeltaTime);

	// Scheduler API — called by UATR_EchoSubsystem::TickReplicationScheduler.
	bool  IsBandDue(EEchoRelevancyBand Band, double NowSeconds) const;
	void  ResetFrameReplicationBudget();
	int32 GetRemainingSnapshotBudget() const;
	int32 ServerReplicateBandBudgeted(UATR_EchoSubsystem* Sub, EEchoRelevancyBand Band,
	                                   const FVector& ViewOrigin, double NowSeconds,
	                                   int32 MaxSnapshotsForThisJob);

	UFUNCTION(Client, Unreliable)
	void Client_EchoSnapshotChunk(const FEchoSnapshotChunk& Chunk);

	UFUNCTION(Server, Reliable)
	void Server_RequestFullResync(int32 ViewId, uint16 LastSeq);

private:
	int32 BuildAndSendBand(UATR_EchoSubsystem* Sub, EEchoRelevancyBand Band,
	                       const TArray<int32>& EchoIndices, int32 MaxSnapshotsToSend);
	void SendDespawnChunk(const TArray<int32>& EchoIndices);
	void MarkBandProcessed(EEchoRelevancyBand Band, double NowSeconds);
	void ApplyChunkToSubsystem(const TArray<FEchoSnapshot>& Snapshots, int32 TotalEchoes);
	void ApplyDespawnToSubsystem(const TArray<FEchoSnapshot>& Snapshots);

	// Cell-relative quantization helpers.
	static uint16        QuantizeToUInt16 (float Value, float MinValue, float MaxValue);
	static float         DequantizeUInt16 (uint16 Value, float MinValue, float MaxValue);
	static FEchoSnapshot EncodeEchoCell   (FVector3f Pos, float Yaw, uint8 AnimState, int32 EchoIndex,
	                                        int32 CellId, FVector2f CellOrigin, float CellSize);
	static FVector3f     DecodeEchoCell   (const FEchoSnapshot& S, FVector2f CellOrigin, float CellSize);

	struct FPendingChunkAssembly
	{
		uint8                 ExpectedChunkCount = 0;
		int32                 TotalEchoes        = 0;
		EEchoSnapshotKind     SnapshotKind       = EEchoSnapshotKind::Full;
		TSet<uint8>           ReceivedChunkIndices;
		TArray<FEchoSnapshot> Snapshots;
	};

	// Server-side state — per-band next-due timestamps drive independent send rates.
	uint16 SnapshotSequence = 0;

	// Scheduler config (read from settings at BeginPlay)
	float  ServerReplicationBudgetMs      = 1.5f;
	int32  MaxReplicationJobsPerFrame     = 8;
	int32  MaxSnapshotsPerClientPerFrame  = 256;
	int32  MaxNearReplicationJobsPerFrame = 8;
	int32  MaxMidReplicationJobsPerFrame  = 4;
	int32  MaxFarReplicationJobsPerFrame  = 2;

	// Per-frame snapshot accounting. Reset by scheduler before servicing this client.
	int32  SnapshotsSentThisFrame = 0;

	// Next-due timestamps (wall-clock seconds) per band.
	double NextNearReplicationTime = 0.0;
	double NextMidReplicationTime  = 0.0;
	double NextFarReplicationTime  = 0.0;

	float NearSnapshotHz = 10.f;
	float MidSnapshotHz  = 3.f;
	float FarSnapshotHz  = 1.f;

	float NearRelevancyRange = 3000.f;
	float MidRelevancyRange  = 10000.f;
	float FarRelevancyRange  = 25000.f;

	float FullResyncCooldownSeconds       = 2.f;
	int32 MaxMissingSequencesBeforeResync = 3;
	int32 MaxSnapshotsPerChunk            = 256;

	TArray<FEchoSnapshot> SnapshotScratch;
	TSet<int32>   CurrentRelevantScratch;
	TArray<int32> RemovedScratch;
	TArray<int32> DespawnIndexScratch;
	TArray<int32> NearScratch;
	TArray<int32> MidScratch;
	TArray<int32> FarScratch;

	// Server-side per-client known echo set
	TSet<int32>         KnownEchoes;
	TMap<int32, uint32> EchoLastHandledVersion; // SoA index → DirtyState.Version when last sent

	// Client-side state
	TMap<uint16, FPendingChunkAssembly> PendingChunks;
	uint16 LastReceivedSequence  = 0;
	int32  MissingChunkCount     = 0;
	float  LastResyncRequestTime = 0.f;
};
