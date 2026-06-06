// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoReplicationComponent.h"
#include "ATR_EchoSubsystem.h"
#include "ATR_EchoSettings.h"
#include "ATR_ActiveEcho.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Async/ParallelFor.h"

// ─── Construction ─────────────────────────────────────────────────────────────

UATR_EchoReplicationComponent::UATR_EchoReplicationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UATR_EchoReplicationComponent::BeginPlay()
{
	Super::BeginPlay();
	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	NearSnapshotHz = Settings->NearSnapshotHz;
	MidSnapshotHz  = Settings->MidSnapshotHz;
	FarSnapshotHz  = Settings->FarSnapshotHz;
	NearRelevancyRange = Settings->NearRelevancyRange;
	MidRelevancyRange  = Settings->MidRelevancyRange;
	FarRelevancyRange  = Settings->FarRelevancyRange;
	FullResyncCooldownSeconds       = Settings->FullResyncCooldownSeconds;
	MaxMissingSequencesBeforeResync = Settings->MaxMissingSequencesBeforeResync;
}

// ─── Server Tick ──────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::ServerTickReplication(UATR_EchoSubsystem* Sub, float DeltaTime)
{
	if (!Sub || Sub->ActiveEntities == 0) return;

	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC) return;

	FVector ViewOrigin;
	FRotator ViewRotation;
	PC->GetPlayerViewPoint(ViewOrigin, ViewRotation);

	NearAccumulator += DeltaTime;
	MidAccumulator  += DeltaTime;
	FarAccumulator  += DeltaTime;

	const bool bSendNear = (NearAccumulator >= 1.f / FMath::Max(1.f, NearSnapshotHz));
	const bool bSendMid  = (MidAccumulator  >= 1.f / FMath::Max(1.f, MidSnapshotHz));
	const bool bSendFar  = (FarAccumulator  >= 1.f / FMath::Max(1.f, FarSnapshotHz));

	if (!bSendNear && !bSendMid && !bSendFar) return;

	NearEchoes.Reset();
	MidEchoes.Reset();
	FarEchoes.Reset();

	Sub->QueryEchoesByRelevancyBands(
		ViewOrigin,
		NearRelevancyRange, MidRelevancyRange, FarRelevancyRange,
		NearEchoes, MidEchoes, FarEchoes
	);

	// Removal sweep: echoes that fell out of all relevancy bands get a Despawn chunk.
	TSet<int32> CurrentRelevant;
	CurrentRelevant.Reserve(NearEchoes.Num() + MidEchoes.Num() + FarEchoes.Num());
	for (int32 i : NearEchoes) CurrentRelevant.Add(i);
	for (int32 i : MidEchoes)  CurrentRelevant.Add(i);
	for (int32 i : FarEchoes)  CurrentRelevant.Add(i);

	{
		TArray<int32> Removed;
		for (int32 Known : KnownEchoes)
			if (!CurrentRelevant.Contains(Known))
				Removed.Add(Known);

		if (Removed.Num() > 0)
			SendDespawnChunk(Removed);

		for (int32 Idx : Removed)
		{
			KnownEchoes.Remove(Idx);
			EchoLastHandledVersion.Remove(Idx);
		}
	}

	if (bSendNear) { BuildAndSendBand(Sub, EEchoRelevancyBand::Near, NearEchoes); NearAccumulator = 0.f; }
	if (bSendMid)  { BuildAndSendBand(Sub, EEchoRelevancyBand::Mid,  MidEchoes);  MidAccumulator  = 0.f; }
	if (bSendFar)  { BuildAndSendBand(Sub, EEchoRelevancyBand::Far,  FarEchoes);  FarAccumulator  = 0.f; }
}

void UATR_EchoReplicationComponent::BuildAndSendBand(UATR_EchoSubsystem* Sub,
                                                      EEchoRelevancyBand Band,
                                                      const TArray<int32>& EchoIndices)
{
	if (EchoIndices.IsEmpty()) return;

	const float CellSz = Sub->SpatialGrid.GetCellSize();

	// Phase 1 (game thread): dirty check, TSet/TMap bookkeeping, promoted actor reads (CMC).
	// These cannot be parallelized — TSet/TMap are not thread-safe, CMC requires game thread.
	struct FEncodeInput { FVector3f Pos; float Yaw; uint8 Anim; int32 Index; };
	TArray<FEncodeInput> ToEncode;
	ToEncode.Reserve(EchoIndices.Num());
	bool bAnyNew = false;

	for (int32 i : EchoIndices)
	{
		const uint32 CurrentVersion = Sub->DirtyStates.IsValidIndex(i)
		                            ? Sub->DirtyStates[i].Version : 0;
		const bool bKnown = KnownEchoes.Contains(i);

		if (bKnown)
		{
			const uint32 LastVersion = EchoLastHandledVersion.FindRef(i);
			if (CurrentVersion == LastVersion) continue; // clean — skip
			EchoLastHandledVersion[i] = CurrentVersion;
		}
		else
		{
			KnownEchoes.Add(i);
			EchoLastHandledVersion.Add(i, CurrentVersion);
			bAnyNew = true;
		}

		FVector3f EncodePos = Sub->Positions[i];
		float     EncodeYaw = Sub->Yaws[i];
		if (AATR_ActiveEcho* Actor = Sub->IndexToActor[i])
		{
			UCapsuleComponent* Capsule = Actor->GetCapsuleComponent();
			if (Capsule)
			{
				const float HH = Capsule->GetScaledCapsuleHalfHeight();
				EncodePos = FVector3f(Actor->GetActorLocation()) - FVector3f(0.f, 0.f, HH);
				EncodeYaw = Actor->GetActorRotation().Yaw;
			}
		}

		ToEncode.Add({ EncodePos, EncodeYaw, Sub->AnimState[i], i });
	}

	if (ToEncode.IsEmpty()) return; // all echoes clean this band

	// Phase 2 (parallel): encode inputs into snapshots — pure math, no UObject access.
	// Pre-sized array; each lane writes only its unique slot — no data races.
	SnapshotScratch.SetNumUninitialized(ToEncode.Num());
	ParallelFor(ToEncode.Num(), [this, &ToEncode, Sub, CellSz](int32 j)
	{
		const FEncodeInput& In = ToEncode[j];
		const int32     CellId     = Sub->SpatialGrid.GetCellId(FVector2f(In.Pos.X, In.Pos.Y));
		const FVector2f CellOrigin = Sub->SpatialGrid.GetCellOrigin2D(CellId);
		SnapshotScratch[j] = EncodeEchoCell(In.Pos, In.Yaw, In.Anim, In.Index,
		                                    CellId, CellOrigin, CellSz);
	});

	const EEchoSnapshotKind Kind = bAnyNew ? EEchoSnapshotKind::Full : EEchoSnapshotKind::Delta;

	const int32 Total     = SnapshotScratch.Num();
	const int32 NumChunks = FMath::DivideAndRoundUp(Total, AATR_EchoManager::MaxEchoSnapshotsPerChunk);

	if (NumChunks > 255)
	{
		UE_LOG(LogATR_EchoNet, Warning,
			TEXT("Echo snapshot chunk count exceeds 255 for band %d (Count=%d). Clamping."),
			static_cast<int32>(Band), Total);
	}

	const uint8 ChunkCount = static_cast<uint8>(FMath::Clamp(NumChunks, 1, 255));
	++SnapshotSequence;

	for (int32 ChunkIdx = 0; ChunkIdx < ChunkCount; ++ChunkIdx)
	{
		const int32 StartIdx = ChunkIdx * AATR_EchoManager::MaxEchoSnapshotsPerChunk;
		const int32 EndIdx   = FMath::Min(StartIdx + AATR_EchoManager::MaxEchoSnapshotsPerChunk, Total);

		FEchoSnapshotChunk Chunk;
		Chunk.Sequence      = SnapshotSequence;
		Chunk.ChunkIndex    = static_cast<uint8>(ChunkIdx);
		Chunk.ChunkCount    = ChunkCount;
		Chunk.TotalEchoes   = Sub->ActiveEntities;
		Chunk.SnapshotKind  = Kind;
		Chunk.RelevancyBand = Band;
		Chunk.ViewId        = 0;
		Chunk.Snapshots.Reserve(EndIdx - StartIdx);

		for (int32 j = StartIdx; j < EndIdx; ++j)
			Chunk.Snapshots.Add(SnapshotScratch[j]);

		Client_EchoSnapshotChunk(Chunk);
	}
}

// ─── Despawn ──────────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::SendDespawnChunk(const TArray<int32>& EchoIndices)
{
	if (EchoIndices.IsEmpty()) return;

	FEchoSnapshotChunk Chunk;
	Chunk.Sequence      = ++SnapshotSequence;
	Chunk.ChunkIndex    = 0;
	Chunk.ChunkCount    = 1;
	Chunk.TotalEchoes   = 0;
	Chunk.SnapshotKind  = EEchoSnapshotKind::Despawn;
	Chunk.RelevancyBand = EEchoRelevancyBand::Near; // unused for Despawn
	Chunk.ViewId        = 0;
	Chunk.Snapshots.Reserve(EchoIndices.Num());

	for (int32 Idx : EchoIndices)
	{
		FEchoSnapshot S;
		S.EchoIndex = Idx;
		Chunk.Snapshots.Add(S);
	}

	Client_EchoSnapshotChunk(Chunk);
}

// ─── Client RPC ───────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::Client_EchoSnapshotChunk_Implementation(const FEchoSnapshotChunk& Chunk)
{
	// Prune stale assemblies — sequences more than 4 behind are considered lost.
	// Incomplete assemblies at prune time represent dropped chunk(s); count them for resync.
	{
		TArray<uint16> ToRemove;
		for (auto& Pair : PendingChunks)
		{
			if (static_cast<uint16>(Chunk.Sequence - Pair.Key) > 4)
			{
				if (Pair.Value.ExpectedChunkCount > 0 &&
				    Pair.Value.ReceivedChunkIndices.Num() < Pair.Value.ExpectedChunkCount)
				{
					++MissingChunkCount;
				}
				ToRemove.Add(Pair.Key);
			}
		}
		for (uint16 Key : ToRemove)
			PendingChunks.Remove(Key);
	}

	// Trigger a full resync if too many sequences have been lost.
	{
		const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		if (MissingChunkCount >= MaxMissingSequencesBeforeResync &&
		    (Now - LastResyncRequestTime) >= FullResyncCooldownSeconds)
		{
			UE_LOG(LogATR_EchoNet, Log,
				TEXT("Requesting full echo resync. MissingCount=%d LastSeq=%d"),
				MissingChunkCount, static_cast<int32>(LastReceivedSequence));
			Server_RequestFullResync(0, LastReceivedSequence);
			LastResyncRequestTime = Now;
			MissingChunkCount     = 0;
		}
	}

	FPendingChunkAssembly& Assembly = PendingChunks.FindOrAdd(Chunk.Sequence);

	if (Assembly.ExpectedChunkCount == 0)
	{
		Assembly.ExpectedChunkCount = Chunk.ChunkCount;
		Assembly.SnapshotKind       = Chunk.SnapshotKind;
	}

	Assembly.TotalEchoes = FMath::Max(Assembly.TotalEchoes, Chunk.TotalEchoes);

	if (Assembly.ReceivedChunkIndices.Contains(Chunk.ChunkIndex))
		return;

	Assembly.ReceivedChunkIndices.Add(Chunk.ChunkIndex);
	Assembly.Snapshots.Append(Chunk.Snapshots);

	if (Assembly.ReceivedChunkIndices.Num() == Assembly.ExpectedChunkCount)
	{
		LastReceivedSequence = Chunk.Sequence;

		if (Assembly.SnapshotKind == EEchoSnapshotKind::Despawn)
		{
			// Removal acknowledged — client stops rendering these echoes (Phase 8 ISM will act on this).
			UE_LOG(LogATR_EchoNet, Verbose,
				TEXT("Client despawn: %d echoes removed from local relevancy set."),
				Assembly.Snapshots.Num());
		}
		else
		{
			ApplyChunkToSubsystem(Assembly.Snapshots, Assembly.TotalEchoes);
		}
		PendingChunks.Remove(Chunk.Sequence);
	}
}

// ─── Server RPC ───────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::Server_RequestFullResync_Implementation(int32 ViewId, uint16 LastSeq)
{
	UE_LOG(LogATR_EchoNet, Log,
		TEXT("Client requested full resync. ViewId=%d LastSeq=%d — clearing known echo set (%d echoes)."),
		ViewId, static_cast<int32>(LastSeq), KnownEchoes.Num());

	// Drop all per-client tracking. On the next ServerTickReplication every echo in
	// relevancy range will be treated as new and receive a Full snapshot.
	KnownEchoes.Empty();
	EchoLastHandledVersion.Empty();
}

// ─── Apply ────────────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::ApplyChunkToSubsystem(const TArray<FEchoSnapshot>& Snapshots,
                                                            int32 TotalEchoes)
{
	UWorld* World = GetWorld();
	if (!World) return;

	auto* Sub = World->GetSubsystem<UATR_EchoSubsystem>();
	if (!Sub) return;

	if (!Sub->IsInitialized())
	{
		UE_LOG(LogATR_EchoNet, Warning,
			TEXT("Received echo snapshot before EchoSubsystem was initialized. Snapshot ignored."));
		return;
	}

	if (TotalEchoes > 0)
		Sub->ActiveEntities = FMath::Clamp(TotalEchoes, 0, Sub->InitializeCount);

	const float CellSz = Sub->SpatialGrid.GetCellSize();

	for (const FEchoSnapshot& S : Snapshots)
	{
		const int32 EchoIndex = S.EchoIndex;

		if (EchoIndex < 0 || EchoIndex >= Sub->ActiveEntities)
			continue;

		if (!Sub->Positions.IsValidIndex(EchoIndex) ||
			!Sub->Yaws.IsValidIndex(EchoIndex)      ||
			!Sub->AnimState.IsValidIndex(EchoIndex))
		{
			UE_LOG(LogATR_EchoNet, Warning,
				TEXT("Invalid snapshot index. EchoIndex=%d ActiveEntities=%d Positions=%d Yaws=%d Anim=%d"),
				EchoIndex,
				Sub->ActiveEntities,
				Sub->Positions.Num(),
				Sub->Yaws.Num(),
				Sub->AnimState.Num());
			continue;
		}

		if (!Sub->SpatialGrid.IsInitialized() || !Sub->SpatialGrid.IsValidCellId(S.CellId))
		{
			UE_LOG(LogATR_EchoNet, Warning,
				TEXT("Invalid cell ID in snapshot. EchoIndex=%d CellId=%d"),
				EchoIndex, S.CellId);
			continue;
		}

		const FVector2f CellOrigin    = Sub->SpatialGrid.GetCellOrigin2D(S.CellId);
		Sub->Positions[EchoIndex] = DecodeEchoCell(S, CellOrigin, CellSz);
		Sub->Yaws[EchoIndex]      = (S.Yaw / 255.f) * 360.f;
		Sub->AnimState[EchoIndex] = S.Anim;
		// Bump the dirty version so the client's ISM QueueTransformUpdate detects the change.
		Sub->MarkEchoDirty(EchoIndex, EEchoDirtyFlags::Transform);
	}
}

// ─── Quantization + Codec ─────────────────────────────────────────────────────

uint16 UATR_EchoReplicationComponent::QuantizeToUInt16(float Value, float MinValue, float MaxValue)
{
	const float Range = MaxValue - MinValue;
	if (Range <= UE_KINDA_SMALL_NUMBER) return 0;
	const float Alpha = FMath::Clamp((Value - MinValue) / Range, 0.f, 1.f);
	return static_cast<uint16>(FMath::RoundToInt(Alpha * 65535.f));
}

float UATR_EchoReplicationComponent::DequantizeUInt16(uint16 Value, float MinValue, float MaxValue)
{
	const float Range = MaxValue - MinValue;
	if (Range <= UE_KINDA_SMALL_NUMBER) return MinValue;
	return MinValue + (static_cast<float>(Value) / 65535.f) * Range;
}

FEchoSnapshot UATR_EchoReplicationComponent::EncodeEchoCell(
	FVector3f Pos, float Yaw, uint8 AnimState, int32 EchoIndex,
	int32 CellId, FVector2f CellOrigin, float CellSize)
{
	FEchoSnapshot S;
	S.EchoIndex = EchoIndex;
	S.CellId    = CellId;
	S.LocalX    = QuantizeToUInt16(Pos.X - CellOrigin.X, 0.f, CellSize);
	S.LocalY    = QuantizeToUInt16(Pos.Y - CellOrigin.Y, 0.f, CellSize);
	// Z has no cell origin (2D grid); quantize over [-CellSize, CellSize*3]
	S.LocalZ    = QuantizeToUInt16(Pos.Z, -CellSize, CellSize * 3.f);
	S.Yaw       = static_cast<uint8>(FMath::RoundToInt(FRotator::ClampAxis(Yaw) / 360.f * 255.f));
	S.Anim      = AnimState;
	return S;
}

FVector3f UATR_EchoReplicationComponent::DecodeEchoCell(
	const FEchoSnapshot& S, FVector2f CellOrigin, float CellSize)
{
	return FVector3f(
		CellOrigin.X + DequantizeUInt16(S.LocalX, 0.f, CellSize),
		CellOrigin.Y + DequantizeUInt16(S.LocalY, 0.f, CellSize),
		DequantizeUInt16(S.LocalZ, -CellSize, CellSize * 3.f)
	);
}
