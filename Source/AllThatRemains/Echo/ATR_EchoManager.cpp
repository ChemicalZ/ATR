// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoManager.h"
#include "ATR_EchoSubsystem.h"
#include "ATR_EchoSettings.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

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

	// ISM components never tick themselves — updated via UpdateISM()
	ISM_Near->SetComponentTickEnabled(false);
	ISM_Mid->SetComponentTickEnabled(false);
	ISM_Far->SetComponentTickEnabled(false);
}

void AATR_EchoManager::BeginPlay()
{
	Super::BeginPlay();

	const UATR_EchoSettings* Settings = GetDefault<UATR_EchoSettings>();
	NearBandDistance          = Settings->NearRelevancyRange;
	MidBandDistance           = Settings->MidRelevancyRange;
	bDebugShowPromotedEchoISM = Settings->bDebugShowPromotedEchoISM;

	// Wire ourselves into the Subsystem on all machines (server sets it via SpawnActor
	// return value; client sets it here when the replicated actor arrives).
	if (auto* Sub = GetWorld()->GetSubsystem<UATR_EchoSubsystem>())
		Sub->SetManager(this);
}

// ─── ISM Update ───────────────────────────────────────────────────────────────

void AATR_EchoManager::UpdateISM(UATR_EchoSubsystem* Sub)
{
	if (!Sub || !ISM_Near || !ISM_Mid || !ISM_Far) return;

	const FQuat   ISMCorrection = FQuat(ISMMeshRotationOffset);
	const FVector ISMOffset     = ISMMeshLocationOffset;

	FVector CameraLoc = FVector::ZeroVector;
	if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		FVector Loc; FRotator Rot;
		PC->GetPlayerViewPoint(Loc, Rot);
		CameraLoc = Loc;
	}

	const float NearSq = NearBandDistance * NearBandDistance;
	const float MidSq  = MidBandDistance  * MidBandDistance;

	// Remove instances whose SoA slots were reclaimed by swap-remove.
	{
		TArray<int32> Stale;
		for (const auto& Pair : EchoToBand)
			if (Pair.Key >= Sub->ActiveEntities)
				Stale.Add(Pair.Key);
		for (int32 Idx : Stale)
			RemoveEchoFromISM(Idx);
	}

	for (int32 i = 0; i < Sub->ActiveEntities; ++i)
	{
		const bool bPromoted = (Sub->IndexToActor[i] != nullptr);
		if (bPromoted && !bDebugShowPromotedEchoISM)
		{
			if (EchoToBand.Contains(i)) RemoveEchoFromISM(i);
			continue;
		}

		const FVector WP(Sub->Positions[i]);
		const FQuat   EntityYaw(FRotator(0.f, Sub->Yaws[i], 0.f));
		const FTransform T(EntityYaw * ISMCorrection, WP + ISMOffset, FVector::OneVector);

		const float DSq = FVector::DistSquaredXY(WP, CameraLoc);
		const EEchoRelevancyBand DesiredBand =
			(DSq <= NearSq) ? EEchoRelevancyBand::Near :
			(DSq <= MidSq)  ? EEchoRelevancyBand::Mid  : EEchoRelevancyBand::Far;

		const uint32 Version = Sub->DirtyStates.IsValidIndex(i) ? Sub->DirtyStates[i].Version : 0;

		const EEchoRelevancyBand* CurrentBand = EchoToBand.Find(i);
		if (!CurrentBand)
		{
			AddEchoToISM(i, DesiredBand, T, Version);
		}
		else if (*CurrentBand != DesiredBand)
		{
			RemoveEchoFromISM(i);
			AddEchoToISM(i, DesiredBand, T, Version);
		}
		else
		{
			QueueTransformUpdate(i, DesiredBand, T, Version);
		}
	}

	FlushTierUpdates();
}

// ─── ISM Helpers ──────────────────────────────────────────────────────────────

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

