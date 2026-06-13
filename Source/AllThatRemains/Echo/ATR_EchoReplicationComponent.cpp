// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoReplicationComponent.h"
#include "ATR_EchoSubsystem.h"
#include "ATR_EchoSettings.h"
#include "ATR_ActiveEcho.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

// ─── Construction ─────────────────────────────────────────────────────────────

UATR_EchoReplicationComponent::UATR_EchoReplicationComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UATR_EchoReplicationComponent::BeginPlay()
{
	Super::BeginPlay();

	UATR_EchoSettings* Settings = GetMutableDefault<UATR_EchoSettings>();
	if (!Settings) return;

	Settings->ValidateAndClamp();

	NearSnapshotHz = Settings->NearSnapshotHz;
	MidSnapshotHz  = Settings->MidSnapshotHz;
	FarSnapshotHz  = Settings->FarSnapshotHz;
	NearRelevancyRange = Settings->NearRelevancyRange;
	MidRelevancyRange  = Settings->MidRelevancyRange;
	FarRelevancyRange  = Settings->FarRelevancyRange;
	MaxSnapshotsPerChunk            = Settings->MaxSnapshotsPerChunk;
	FullResyncCooldownSeconds       = Settings->FullResyncCooldownSeconds;
	MaxMissingSequencesBeforeResync = Settings->MaxMissingSequencesBeforeResync;

	ServerReplicationBudgetMs      = Settings->ServerReplicationBudgetMs;
	MaxReplicationJobsPerFrame     = Settings->MaxReplicationJobsPerFrame;
	MaxSnapshotsPerClientPerFrame  = Settings->MaxSnapshotsPerClientPerFrame;
	MaxNearReplicationJobsPerFrame = Settings->MaxNearReplicationJobsPerFrame;
	MaxMidReplicationJobsPerFrame  = Settings->MaxMidReplicationJobsPerFrame;
	MaxFarReplicationJobsPerFrame  = Settings->MaxFarReplicationJobsPerFrame;

	// Force all bands due immediately so a newly connected client receives Near/Mid/Far
	// within the first scheduler pass instead of waiting for the first interval to elapse.
	const double NowSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	NextNearReplicationTime = NowSeconds;
	NextMidReplicationTime  = NowSeconds;
	NextFarReplicationTime  = NowSeconds;
}

// ─── Server Tick ──────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::ServerTickReplication(UATR_EchoSubsystem* Sub, float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_ServerTickReplication_LegacyWrapper);

	if (!Sub || Sub->ActiveEntities == 0) return;

	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC) return;

	FVector ViewOrigin;
	FRotator ViewRotation;
	PC->GetPlayerViewPoint(ViewOrigin, ViewRotation);

	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;

	ResetFrameReplicationBudget();

	if (IsBandDue(EEchoRelevancyBand::Near, Now))
	{
		ServerReplicateBandBudgeted(Sub, EEchoRelevancyBand::Near, ViewOrigin, Now,
		                            GetRemainingSnapshotBudget());
	}

	if (IsBandDue(EEchoRelevancyBand::Mid, Now) && GetRemainingSnapshotBudget() > 0)
	{
		ServerReplicateBandBudgeted(Sub, EEchoRelevancyBand::Mid, ViewOrigin, Now,
		                            GetRemainingSnapshotBudget());
	}

	if (IsBandDue(EEchoRelevancyBand::Far, Now) && GetRemainingSnapshotBudget() > 0)
	{
		ServerReplicateBandBudgeted(Sub, EEchoRelevancyBand::Far, ViewOrigin, Now,
		                            GetRemainingSnapshotBudget());
	}
}

int32 UATR_EchoReplicationComponent::BuildAndSendBand(UATR_EchoSubsystem* Sub,
                                                       EEchoRelevancyBand Band,
                                                       const TArray<int32>& EchoIndices,
                                                       int32 MaxSnapshotsToSend)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_BuildAndSendBand);

	if (EchoIndices.IsEmpty() || MaxSnapshotsToSend <= 0) return 0;

	const float CellSz = Sub->SpatialGrid.GetCellSize();
	const int32 SafeMaxSnapshotsPerChunk = FMath::Clamp(MaxSnapshotsPerChunk, 1, 512);
	const int32 MaxSnapshotsThisSequence = SafeMaxSnapshotsPerChunk * 255;
	const int32 EffectiveMax = FMath::Min(MaxSnapshotsThisSequence, MaxSnapshotsToSend);

	// Phase 1 (game thread): dirty check, TSet/TMap bookkeeping, promoted actor reads (CMC).
	// These cannot be parallelized — TSet/TMap are not thread-safe, CMC requires game thread.
	struct FEncodeInput { FVector3f Pos; float Yaw; uint8 Anim; int32 Index; };
	TArray<FEncodeInput> ToEncode;
	TArray<int32> NewlyKnown; // first-time-relevant this band — may need a health refresh
	bool bAnyNew = false;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_BuildEncodeInputs);
		ToEncode.Reserve(FMath::Min(EchoIndices.Num(), EffectiveMax));

		for (int32 i : EchoIndices)
		{
			const uint32 CurrentVersion = Sub->DirtyStates.IsValidIndex(i)
			                            ? Sub->DirtyStates[i].Version : 0;
			const bool bKnown = KnownEchoes.Contains(i);

			if (bKnown)
			{
				const uint32 LastVersion = EchoLastHandledVersion.FindRef(i);
				if (CurrentVersion == LastVersion) continue; // clean — skip
			}

			if (ToEncode.Num() >= EffectiveMax)
			{
				break; // scheduler budget reached — defer to next frame
			}

			if (bKnown)
			{
				EchoLastHandledVersion[i] = CurrentVersion;
			}
			else
			{
				KnownEchoes.Add(i);
				EchoLastHandledVersion.Add(i, CurrentVersion);
				NewlyKnown.Add(i);
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
	}

	if (ToEncode.IsEmpty()) return 0; // all echoes clean this band

	// Phase 2 (parallel): encode inputs into snapshots — pure math, no UObject access.
	// Pre-sized array; each lane writes only its unique slot — no data races.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_EncodeSnapshots);
		SnapshotScratch.SetNumUninitialized(ToEncode.Num());
		ParallelFor(ToEncode.Num(), [this, &ToEncode, Sub, CellSz](int32 j)
		{
			const FEncodeInput& In = ToEncode[j];
			const int32     CellId     = Sub->SpatialGrid.GetCellId(FVector2f(In.Pos.X, In.Pos.Y));
			const FVector2f CellOrigin = Sub->SpatialGrid.GetCellOrigin2D(CellId);
			SnapshotScratch[j] = EncodeEchoCell(In.Pos, In.Yaw, In.Anim, In.Index,
			                                    CellId, CellOrigin, CellSz);
		});
	}

	const EEchoSnapshotKind Kind = bAnyNew ? EEchoSnapshotKind::Full : EEchoSnapshotKind::Delta;

	const int32 Total      = SnapshotScratch.Num();
	const int32 NumChunks  = FMath::DivideAndRoundUp(Total, SafeMaxSnapshotsPerChunk);
	const uint8 ChunkCount = static_cast<uint8>(FMath::Clamp(NumChunks, 1, 255));
	++SnapshotSequence;

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_SendSnapshotChunks);
		for (int32 ChunkIdx = 0; ChunkIdx < ChunkCount; ++ChunkIdx)
		{
			const int32 StartIdx = ChunkIdx * SafeMaxSnapshotsPerChunk;
			const int32 EndIdx   = FMath::Min(StartIdx + SafeMaxSnapshotsPerChunk, Total);

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

	// Newly-relevant echoes that already carry structural damage get a full
	// health refresh so this client doesn't render them pristine (it missed
	// the original deltas). Clean echoes need nothing — clean is the default.
	if (NewlyKnown.Num() > 0)
	{
		const FATR_EchoHealthModel& Model = Sub->GetHealthModel();
		TArray<FATR_EchoHealthDelta> Refresh;
		for (const int32 i : NewlyKnown)
		{
			if (Model.IsRowDamaged(i))
			{
				Refresh.Add(Model.MakeFullRefreshDelta(i));
			}
		}
		if (Refresh.Num() > 0)
		{
			Client_EchoHealthDeltas(Refresh);
		}
	}

	return Total;
}

// ─── Despawn ──────────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::SendDespawnChunk(const TArray<int32>& EchoIndices)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_SendDespawnChunks);

	if (EchoIndices.IsEmpty()) return;

	const int32 SafeMaxSnapshotsPerChunk = FMath::Clamp(MaxSnapshotsPerChunk, 1, 512);
	const int32 MaxSnapshotsPerSequence = SafeMaxSnapshotsPerChunk * 255;
	const int32 Total = EchoIndices.Num();

	for (int32 SequenceStart = 0; SequenceStart < Total; SequenceStart += MaxSnapshotsPerSequence)
	{
		const int32 SequenceEnd = FMath::Min(SequenceStart + MaxSnapshotsPerSequence, Total);
		const int32 SequenceTotal = SequenceEnd - SequenceStart;
		const int32 NumChunks = FMath::DivideAndRoundUp(SequenceTotal, SafeMaxSnapshotsPerChunk);
		const uint8 ChunkCount = static_cast<uint8>(FMath::Clamp(NumChunks, 1, 255));
		const uint16 Sequence = ++SnapshotSequence;

		for (int32 ChunkIdx = 0; ChunkIdx < ChunkCount; ++ChunkIdx)
		{
			const int32 StartIdx = SequenceStart + ChunkIdx * SafeMaxSnapshotsPerChunk;
			const int32 EndIdx = FMath::Min(StartIdx + SafeMaxSnapshotsPerChunk, SequenceEnd);
			if (StartIdx >= EndIdx) break;

			FEchoSnapshotChunk Chunk;
			Chunk.Sequence      = Sequence;
			Chunk.ChunkIndex    = static_cast<uint8>(ChunkIdx);
			Chunk.ChunkCount    = ChunkCount;
			Chunk.TotalEchoes   = 0;
			Chunk.SnapshotKind  = EEchoSnapshotKind::Despawn;
			Chunk.RelevancyBand = EEchoRelevancyBand::Near; // unused for Despawn
			Chunk.ViewId        = 0;
			Chunk.Snapshots.Reserve(EndIdx - StartIdx);

			for (int32 j = StartIdx; j < EndIdx; ++j)
			{
				FEchoSnapshot S;
				S.EchoIndex = EchoIndices[j];
				Chunk.Snapshots.Add(S);
			}

			Client_EchoSnapshotChunk(Chunk);
		}
	}
}

// ─── Client RPC ───────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::Client_EchoSnapshotChunk_Implementation(const FEchoSnapshotChunk& Chunk)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_ClientReceiveChunk);

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

			if (UWorld* World = GetWorld())
			{
				if (UATR_EchoSubsystem* Sub = World->GetSubsystem<UATR_EchoSubsystem>())
					Sub->ClearClientEchoRelevancy();
			}
			PendingChunks.Empty();

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
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_ClientAssembly);
		LastReceivedSequence = Chunk.Sequence;

		if (Assembly.SnapshotKind == EEchoSnapshotKind::Despawn)
		{
			ApplyDespawnToSubsystem(Assembly.Snapshots);
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

	// Drop all per-client tracking. On the next scheduler pass every echo in
	// relevancy range will be treated as new and receive a Full snapshot.
	KnownEchoes.Empty();
	EchoLastHandledVersion.Empty();

	// Reset band due times so all three bands fire on the next scheduler pass.
	// Without this, a resync during a low-Hz Far interval could leave the client
	// waiting up to 10s (at FarSnapshotHz=0.1) before receiving far echo indices.
	const double NowSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	NextNearReplicationTime = NowSeconds;
	NextMidReplicationTime  = NowSeconds;
	NextFarReplicationTime  = NowSeconds;
}

// ─── Structural health deltas (client apply) ──────────────────────────────────

void UATR_EchoReplicationComponent::Client_EchoHealthDeltas_Implementation(const TArray<FATR_EchoHealthDelta>& Deltas)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// Listen-server host: the local model IS the authoritative one — applying
	// our own deltas back would be redundant. Skip.
	if (World->GetNetMode() != NM_Client) return;

	UATR_EchoSubsystem* Sub = World->GetSubsystem<UATR_EchoSubsystem>();
	if (!Sub || !Sub->IsInitialized()) return;

	FATR_EchoHealthModel& Model = Sub->GetHealthModel();
	for (const FATR_EchoHealthDelta& Delta : Deltas)
	{
		Model.ApplyDeltaFromServer(Delta);
	}
}

// ─── Apply ────────────────────────────────────────────────────────────────────

void UATR_EchoReplicationComponent::ApplyChunkToSubsystem(const TArray<FEchoSnapshot>& Snapshots,
                                                            int32 TotalEchoes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_ClientApplySnapshots);

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

	// TotalEchoes is server metadata only. This client receives a partial
	// relevancy stream, so do not expand ActiveEntities to the server total.
	// Each received EchoIndex is marked client-relevant individually below.
	(void)TotalEchoes;

	const float CellSz = Sub->SpatialGrid.GetCellSize();

	for (const FEchoSnapshot& S : Snapshots)
	{
		const int32 EchoIndex = S.EchoIndex;

		if (EchoIndex < 0 || EchoIndex >= Sub->InitializeCount)
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
		Sub->MarkEchoClientRelevant(EchoIndex);
		// Bump the dirty version so the client's ISM QueueTransformUpdate detects the change.
		Sub->MarkEchoDirty(EchoIndex, EEchoDirtyFlags::Transform);
	}
}


void UATR_EchoReplicationComponent::ApplyDespawnToSubsystem(const TArray<FEchoSnapshot>& Snapshots)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_ClientApplyDespawns);

	UWorld* World = GetWorld();
	if (!World) return;

	UATR_EchoSubsystem* Sub = World->GetSubsystem<UATR_EchoSubsystem>();
	if (!Sub) return;

	DespawnIndexScratch.Reset();
	DespawnIndexScratch.Reserve(Snapshots.Num());

	for (const FEchoSnapshot& S : Snapshots)
	{
		if (S.EchoIndex < 0 || S.EchoIndex >= Sub->InitializeCount)
			continue;

		DespawnIndexScratch.Add(S.EchoIndex);
	}

	Sub->MarkEchoesClientIrrelevant(DespawnIndexScratch);

	UE_LOG(LogATR_EchoNet, Verbose,
		TEXT("Client despawn: %d echoes removed from local relevancy set."),
		DespawnIndexScratch.Num());
}

// ─── Scheduler API ───────────────────────────────────────────────────────────

bool UATR_EchoReplicationComponent::IsBandDue(EEchoRelevancyBand Band, double NowSeconds) const
{
	switch (Band)
	{
	case EEchoRelevancyBand::Near: return NowSeconds >= NextNearReplicationTime;
	case EEchoRelevancyBand::Mid:  return NowSeconds >= NextMidReplicationTime;
	case EEchoRelevancyBand::Far:  return NowSeconds >= NextFarReplicationTime;
	default:                       return false;
	}
}

void UATR_EchoReplicationComponent::ResetFrameReplicationBudget()
{
	SnapshotsSentThisFrame = 0;
}

int32 UATR_EchoReplicationComponent::GetRemainingSnapshotBudget() const
{
	return FMath::Max(0, MaxSnapshotsPerClientPerFrame - SnapshotsSentThisFrame);
}

void UATR_EchoReplicationComponent::MarkBandProcessed(EEchoRelevancyBand Band, double NowSeconds)
{
	switch (Band)
	{
	case EEchoRelevancyBand::Near:
		NextNearReplicationTime = NowSeconds + (1.0 / FMath::Max(0.1f, NearSnapshotHz));
		break;
	case EEchoRelevancyBand::Mid:
		NextMidReplicationTime  = NowSeconds + (1.0 / FMath::Max(0.1f, MidSnapshotHz));
		break;
	case EEchoRelevancyBand::Far:
		NextFarReplicationTime  = NowSeconds + (1.0 / FMath::Max(0.1f, FarSnapshotHz));
		break;
	}
}

int32 UATR_EchoReplicationComponent::ServerReplicateBandBudgeted(
	UATR_EchoSubsystem* Sub,
	EEchoRelevancyBand  Band,
	const FVector&      ViewOrigin,
	double              NowSeconds,
	int32               MaxSnapshotsForThisJob)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_ServerReplicateBandBudgeted);

	if (!Sub || MaxSnapshotsForThisJob <= 0) return 0;

	NearScratch.Reset();
	MidScratch.Reset();
	FarScratch.Reset();

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_QueryRelevancy);
		Sub->QueryEchoesByRelevancyBands(
			ViewOrigin,
			NearRelevancyRange, MidRelevancyRange, FarRelevancyRange,
			NearScratch, MidScratch, FarScratch);
	}

	// Removal sweep — only during Near band job to avoid redundant scans per client.
	if (Band == EEchoRelevancyBand::Near)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_NearRemovalMaintenance);

		CurrentRelevantScratch.Reset();
		CurrentRelevantScratch.Reserve(NearScratch.Num() + MidScratch.Num() + FarScratch.Num());
		for (int32 i : NearScratch) CurrentRelevantScratch.Add(i);
		for (int32 i : MidScratch)  CurrentRelevantScratch.Add(i);
		for (int32 i : FarScratch)  CurrentRelevantScratch.Add(i);

		RemovedScratch.Reset();
		for (int32 Known : KnownEchoes)
		{
			if (!CurrentRelevantScratch.Contains(Known))
				RemovedScratch.Add(Known);
		}

		if (RemovedScratch.Num() > 0)
		{
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_SendDespawns);
				SendDespawnChunk(RemovedScratch);
			}

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(EchoRep_RemoveKnownEchoes);
				for (int32 Removed : RemovedScratch)
				{
					KnownEchoes.Remove(Removed);
					EchoLastHandledVersion.Remove(Removed);
				}
			}
		}
	}

	const TArray<int32>* Selected = nullptr;
	switch (Band)
	{
	case EEchoRelevancyBand::Near: Selected = &NearScratch; break;
	case EEchoRelevancyBand::Mid:  Selected = &MidScratch;  break;
	case EEchoRelevancyBand::Far:  Selected = &FarScratch;  break;
	}

	int32 Sent = 0;
	if (Selected)
	{
		Sent = BuildAndSendBand(Sub, Band, *Selected, MaxSnapshotsForThisJob);
	}

	SnapshotsSentThisFrame += Sent;
	MarkBandProcessed(Band, NowSeconds);

	return Sent;
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
