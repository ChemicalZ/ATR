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
	void ServerTickReplication(UATR_EchoSubsystem* Sub, float DeltaTime);

	UFUNCTION(Client, Unreliable)
	void Client_EchoSnapshotChunk(const FEchoSnapshotChunk& Chunk);

	UFUNCTION(Server, Reliable)
	void Server_RequestFullResync(int32 ViewId, uint16 LastSeq);

private:
	void BuildAndSendBand(UATR_EchoSubsystem* Sub, EEchoRelevancyBand Band,
	                      const TArray<int32>& EchoIndices);
	void SendDespawnChunk(const TArray<int32>& EchoIndices);
	void ApplyChunkToSubsystem(const TArray<FEchoSnapshot>& Snapshots, int32 TotalEchoes);

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

	// Server-side state — per-band accumulators drive independent send rates.
	float  NearAccumulator = 0.f;
	float  MidAccumulator  = 0.f;
	float  FarAccumulator  = 0.f;
	uint16 SnapshotSequence = 0;

	float NearSnapshotHz = 10.f;
	float MidSnapshotHz  = 3.f;
	float FarSnapshotHz  = 1.f;

	float NearRelevancyRange = 3000.f;
	float MidRelevancyRange  = 10000.f;
	float FarRelevancyRange  = 25000.f;

	float FullResyncCooldownSeconds       = 2.f;
	int32 MaxMissingSequencesBeforeResync = 3;

	TArray<FEchoSnapshot> SnapshotScratch;
	TArray<int32> NearEchoes;
	TArray<int32> MidEchoes;
	TArray<int32> FarEchoes;

	// Server-side per-client known echo set
	TSet<int32>         KnownEchoes;
	TMap<int32, uint32> EchoLastHandledVersion; // SoA index → DirtyState.Version when last sent

	// Client-side state
	TMap<uint16, FPendingChunkAssembly> PendingChunks;
	uint16 LastReceivedSequence  = 0;
	int32  MissingChunkCount     = 0;
	float  LastResyncRequestTime = 0.f;
};
