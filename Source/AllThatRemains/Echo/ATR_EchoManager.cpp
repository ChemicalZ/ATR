// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoManager.h"
#include "ATR_EchoSubsystem.h"
#include "ATR_EchoSettings.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

DEFINE_LOG_CATEGORY(LogATR_EchoNet);
DEFINE_LOG_CATEGORY(LogATR_EchoRender);

// ─── Construction ─────────────────────────────────────────────────────────────

AATR_EchoManager::AATR_EchoManager()
{
	// Driven by Subsystem::Tick — disable Actor's own tick to avoid double-work
	PrimaryActorTick.bCanEverTick = false;

	// Replicate to all clients so they receive the ISM actor for local rendering.
	bReplicates     = true;
	bAlwaysRelevant = true; // ISM must be present on all clients regardless of world position

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;

	ISM_Near = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ISM_Near"));
	ISM_Mid  = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ISM_Mid"));
	ISM_Far  = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("ISM_Far"));

	ISM_Near->SetupAttachment(Root);
	ISM_Mid->SetupAttachment(Root);
	ISM_Far->SetupAttachment(Root);

	// Constructor-time defaults must not depend on developer settings.
	// Settings are validated and applied in BeginPlay.
	ApplyISMSafeDefaults(ISM_Near);
	ApplyISMSafeDefaults(ISM_Mid);
	ApplyISMSafeDefaults(ISM_Far);
}

void AATR_EchoManager::BeginPlay()
{
	Super::BeginPlay();

	// Visual band distances come from Echo|Rendering — NOT the network relevancy
	// ranges. Rendering and networking solve different problems (RenderThread cost
	// vs bandwidth) and must be tuned independently. Validate first so hand-edited
	// config cannot violate distance ordering or LocalZoneRadius invariants.
	if (UATR_EchoSettings* Settings = GetMutableDefault<UATR_EchoSettings>())
	{
		Settings->ValidateAndClamp();

		NearBandDistance = Settings->VisualNearDistance;
		MidBandDistance  = Settings->VisualMidDistance;
		FarBandDistance  = Settings->VisualFarDistance;
	}

	// Settings-driven render flags are applied after validation.
	ConfigureISMComponent(ISM_Near);
	ConfigureISMComponent(ISM_Mid);
	ConfigureISMComponent(ISM_Far);

	// Wire ourselves into the Subsystem on all machines (server sets it via SpawnActor
	// return value; client sets it here when the replicated actor arrives).
	if (auto* Sub = GetWorld()->GetSubsystem<UATR_EchoSubsystem>())
		Sub->SetManager(this);
}

// ─── ISM Update ───────────────────────────────────────────────────────────────

// Visualizes only the locally relevant horde echoes returned by the subsystem's
// spatial relevancy query — never scans the full ActiveEntities array. Echoes
// beyond FarBandDistance (the visual cutoff) get no ISM instance, and any echo
// that was visualized last frame but is not relevant this frame is removed.
void AATR_EchoManager::UpdateISM(UATR_EchoSubsystem* Sub)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoISM_UpdateISM);

	if (!Sub) return;
	if (!ISM_Near || !ISM_Mid || !ISM_Far) return;

	EnsureVisualStampCapacity(Sub->InitializeCount);

	// New frame epoch. On wrap (epoch hits 0) the stale-detection comparison
	// breaks, so reset all stamps and restart at 1.
	if (++VisualFrameEpoch == 0)
	{
		EchoVisualStamp.Init(0, Sub->InitializeCount);
		VisualFrameEpoch = 1;
	}

	FVector ViewLocation = FVector::ZeroVector;
	if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		FRotator ViewRotation;
		PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
	}
	else
	{
		ViewLocation = GetActorLocation();
	}

	NearScratch.Reset();
	MidScratch.Reset();
	FarScratch.Reset();

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoISM_QueryRelevancy);
		Sub->QueryEchoesByRelevancyBands(
			ViewLocation,
			NearBandDistance,
			MidBandDistance,
			FarBandDistance,
			NearScratch,
			MidScratch,
			FarScratch);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoISM_ProcessBands);
		ProcessBand(Sub, NearScratch, EEchoRelevancyBand::Near);
		ProcessBand(Sub, MidScratch,  EEchoRelevancyBand::Mid);
		ProcessBand(Sub, FarScratch,  EEchoRelevancyBand::Far);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoISM_RemoveUnstamped);
		RemoveUnstampedEchoes(Sub);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(EchoISM_FlushUpdates);
		FlushTierUpdates();
	}
}

// ─── Visual band processing ───────────────────────────────────────────────────

void AATR_EchoManager::EnsureVisualStampCapacity(int32 RequiredCapacity)
{
	// Grow only — never shrink during play (indices stay stable for the epoch test).
	if (EchoVisualStamp.Num() < RequiredCapacity)
	{
		EchoVisualStamp.SetNumZeroed(RequiredCapacity);
	}
}

void AATR_EchoManager::ProcessBand(UATR_EchoSubsystem* Sub, const TArray<int32>& Echoes,
                                   EEchoRelevancyBand DesiredBand)
{
	if (!Sub) return;

	const FQuat   ISMCorrection = FQuat(ISMMeshRotationOffset);
	const FVector ISMOffset     = ISMMeshLocationOffset;

	for (int32 EchoIndex : Echoes)
	{
		// Bound by InitializeCount (population capacity), not ActiveEntities:
		// clients receive partial relevancy and do NOT expand ActiveEntities,
		// so a high-index relevant echo would otherwise never render here.
		if (EchoIndex < 0 || EchoIndex >= Sub->InitializeCount) continue;
		if (!Sub->Positions.IsValidIndex(EchoIndex))            continue;

		// Promoted echoes are full Actors — never drawn as horde ISM.
		if (Sub->IndexToActor.IsValidIndex(EchoIndex) && Sub->IndexToActor[EchoIndex]) continue;

		if (!EchoVisualStamp.IsValidIndex(EchoIndex)) continue;

		// Mark seen this frame so RemoveUnstampedEchoes keeps this instance.
		EchoVisualStamp[EchoIndex] = VisualFrameEpoch;

		const FVector WorldPosition(Sub->Positions[EchoIndex]);
		const float   Yaw = Sub->Yaws.IsValidIndex(EchoIndex) ? Sub->Yaws[EchoIndex] : 0.f;
		const FQuat   EntityYaw(FRotator(0.f, Yaw, 0.f));
		const FTransform Transform(EntityYaw * ISMCorrection, WorldPosition + ISMOffset, FVector::OneVector);

		const uint32 Version = Sub->DirtyStates.IsValidIndex(EchoIndex)
			? Sub->DirtyStates[EchoIndex].Version : 0;

		const EEchoRelevancyBand* ExistingBand = EchoToBand.Find(EchoIndex);
		if (!ExistingBand)
		{
			AddEchoToISM(EchoIndex, DesiredBand, Transform, Version);
		}
		else if (*ExistingBand != DesiredBand)
		{
			RemoveEchoFromISM(EchoIndex);
			AddEchoToISM(EchoIndex, DesiredBand, Transform, Version);
		}
		else
		{
			QueueTransformUpdate(EchoIndex, DesiredBand, Transform, Version);
		}
	}
}

void AATR_EchoManager::RemoveUnstampedEchoes(UATR_EchoSubsystem* Sub)
{
	// Collect first, then remove — never mutate EchoToBand while iterating it.
	RemoveScratch.Reset();

	for (const TPair<int32, EEchoRelevancyBand>& Pair : EchoToBand)
	{
		const int32 EchoIndex = Pair.Key;

		const bool bInvalidIndex =
			EchoIndex < 0 ||
			!Sub ||
			EchoIndex >= Sub->ActiveEntities ||
			!EchoVisualStamp.IsValidIndex(EchoIndex);

		const bool bNotSeenThisFrame =
			!bInvalidIndex &&
			EchoVisualStamp[EchoIndex] != VisualFrameEpoch;

		const bool bPromoted =
			!bInvalidIndex &&
			Sub->IndexToActor.IsValidIndex(EchoIndex) &&
			Sub->IndexToActor[EchoIndex] != nullptr;

		if (bInvalidIndex || bNotSeenThisFrame || bPromoted)
		{
			RemoveScratch.Add(EchoIndex);
		}
	}

	for (int32 EchoIndex : RemoveScratch)
	{
		RemoveEchoFromISM(EchoIndex);
	}
}

// ─── ISM Helpers ──────────────────────────────────────────────────────────────

void AATR_EchoManager::ApplyISMSafeDefaults(UInstancedStaticMeshComponent* ISM)
{
	if (!ISM) return;

	ISM->SetComponentTickEnabled(false);
	ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ISM->SetGenerateOverlapEvents(false);

	ISM->SetCastShadow(false);
	ISM->bCastDynamicShadow = false;
	ISM->bCastStaticShadow = false;
	ISM->bReceivesDecals = false;
	ISM->bAffectDistanceFieldLighting = false;
}

void AATR_EchoManager::ConfigureISMComponent(UInstancedStaticMeshComponent* ISM)
{
	if (!ISM) return;

	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();

	// Preserve hard safety defaults even if a setting read fails.
	ApplyISMSafeDefaults(ISM);

	const bool bCastShadows                 = Settings->bHordeISMCastShadows;
	const bool bReceivesDecals              = Settings->bHordeISMReceivesDecals;
	const bool bAffectDistanceFieldLighting = Settings->bHordeISMAffectDistanceFieldLighting;

	ISM->SetCastShadow(bCastShadows);
	ISM->bCastDynamicShadow            = bCastShadows;
	ISM->bCastStaticShadow             = bCastShadows;
	ISM->bReceivesDecals               = bReceivesDecals;
	ISM->bAffectDistanceFieldLighting  = bAffectDistanceFieldLighting;
}

UInstancedStaticMeshComponent* AATR_EchoManager::ISMForBand(EEchoRelevancyBand Band) const
{
	switch (Band)
	{
		case EEchoRelevancyBand::Near: return ISM_Near;
		case EEchoRelevancyBand::Mid:  return ISM_Mid;
		default:                       return ISM_Far;
	}
}

void AATR_EchoManager::AddEchoToISM(int32 EchoIndex, EEchoRelevancyBand Band,
                                     const FTransform& T, uint32 Version)
{
	UInstancedStaticMeshComponent* ISM = ISMForBand(Band);
	if (!ISM || !ISM->GetStaticMesh()) return;

	FEchoISMTier& Tier = Tiers[static_cast<int32>(Band)];
	const int32 InstanceIdx = ISM->AddInstance(T, /*bWorldSpace=*/true);

	Tier.EchoToInstance.Add(EchoIndex, InstanceIdx);
	ensure(InstanceIdx == Tier.InstanceToEcho.Num());
	Tier.InstanceToEcho.Add(EchoIndex);
	Tier.EchoISMVersion.Add(EchoIndex, Version);

	EchoToBand.Add(EchoIndex, Band);
}

void AATR_EchoManager::RemoveEchoFromISM(int32 EchoIndex)
{
	const EEchoRelevancyBand* BandPtr = EchoToBand.Find(EchoIndex);
	if (!BandPtr) return;

	FEchoISMTier& Tier = Tiers[static_cast<int32>(*BandPtr)];
	UInstancedStaticMeshComponent* ISM = ISMForBand(*BandPtr);

	const int32* InstIdxPtr = Tier.EchoToInstance.Find(EchoIndex);
	if (!InstIdxPtr) { EchoToBand.Remove(EchoIndex); return; }

	const int32 RemovedIdx = *InstIdxPtr;

	if (ISM && ISM->GetStaticMesh())
		ISM->RemoveInstance(RemovedIdx);

	Tier.EchoToInstance.Remove(EchoIndex);
	Tier.InstanceToEcho.RemoveAt(RemovedIdx);
	Tier.EchoISMVersion.Remove(EchoIndex);

	// UE RemoveInstance compacts the array — all slots above RemovedIdx shift down by 1.
	for (auto& Pair : Tier.EchoToInstance)
		if (Pair.Value > RemovedIdx) --Pair.Value;

	EchoToBand.Remove(EchoIndex);
}

void AATR_EchoManager::QueueTransformUpdate(int32 EchoIndex, EEchoRelevancyBand Band,
                                             const FTransform& T, uint32 Version)
{
	FEchoISMTier& Tier = Tiers[static_cast<int32>(Band)];

	const uint32* LastVer = Tier.EchoISMVersion.Find(EchoIndex);
	if (LastVer && *LastVer == Version) return; // transform unchanged

	const int32* InstIdxPtr = Tier.EchoToInstance.Find(EchoIndex);
	if (!InstIdxPtr) return;

	Tier.EchoISMVersion.Add(EchoIndex, Version);
	Tier.DirtyInstances.Add(*InstIdxPtr);
	Tier.DirtyTransforms.Add(T);
}

void AATR_EchoManager::FlushTierUpdates()
{
	for (int32 BandIdx = 0; BandIdx < 3; ++BandIdx)
	{
		FEchoISMTier& Tier = Tiers[BandIdx];
		UInstancedStaticMeshComponent* ISM = ISMForBand(static_cast<EEchoRelevancyBand>(BandIdx));

		if (!ISM || !ISM->GetStaticMesh() || Tier.DirtyInstances.IsEmpty()) continue;

		const int32 InstanceCount = ISM->GetInstanceCount();
		for (int32 k = 0; k < Tier.DirtyInstances.Num(); ++k)
		{
			const int32 InstIdx = Tier.DirtyInstances[k];
			if (InstIdx >= 0 && InstIdx < InstanceCount)
				ISM->UpdateInstanceTransform(InstIdx, Tier.DirtyTransforms[k],
				                             /*bWorldSpace=*/true, /*bMarkRenderStateDirty=*/false,
				                             /*bTeleport=*/false);
		}

		ISM->MarkRenderStateDirty();
		Tier.DirtyInstances.Reset();
		Tier.DirtyTransforms.Reset();
	}
}

