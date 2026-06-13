// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoHealthModel.h"

#include "../ATR_HealthSettings.h"
#include "../ATR_HealthTypes.h"

namespace ATR_EchoParts
{
	uint32 WithDistalParts(const uint32 PartBits)
	{
		uint32 Out = PartBits;

		// Head takes the jaw with it.
		if (Out & HeadPresent) { Out |= JawPresent; }

		// Arms: upper -> forearm -> hand.
		if (Out & LeftUpperArm) { Out |= LeftForearm; }
		if (Out & LeftForearm) { Out |= LeftHand; }
		if (Out & RightUpperArm) { Out |= RightForearm; }
		if (Out & RightForearm) { Out |= RightHand; }

		// Legs: thigh -> shin -> foot.
		if (Out & LeftThigh) { Out |= LeftShin; }
		if (Out & LeftShin) { Out |= LeftFoot; }
		if (Out & RightThigh) { Out |= RightShin; }
		if (Out & RightShin) { Out |= RightFoot; }

		return Out;
	}

	uint32 PartBitsForBodyRegion(const EATR_BodyRegion Region)
	{
		switch (Region)
		{
		case EATR_BodyRegion::Head:          return HeadPresent;
		case EATR_BodyRegion::Neck:          return NeckFunctional;
		case EATR_BodyRegion::Chest:         return TorsoFunctional;
		case EATR_BodyRegion::Abdomen:       return TorsoFunctional;
		case EATR_BodyRegion::Pelvis:        return SpineFunctional;
		case EATR_BodyRegion::LeftUpperArm:  return LeftUpperArm;
		case EATR_BodyRegion::LeftForearm:   return LeftForearm;
		case EATR_BodyRegion::LeftHand:      return LeftHand;
		case EATR_BodyRegion::RightUpperArm: return RightUpperArm;
		case EATR_BodyRegion::RightForearm:  return RightForearm;
		case EATR_BodyRegion::RightHand:     return RightHand;
		case EATR_BodyRegion::LeftThigh:     return LeftThigh;
		case EATR_BodyRegion::LeftShin:      return LeftShin;
		case EATR_BodyRegion::LeftFoot:      return LeftFoot;
		case EATR_BodyRegion::RightThigh:    return RightThigh;
		case EATR_BodyRegion::RightShin:     return RightShin;
		case EATR_BodyRegion::RightFoot:     return RightFoot;
		default:                             return 0;
		}
	}
}

namespace ATR_EchoDigits
{
	int32 CountFingers(const uint32 DigitMask, const bool bLeft)
	{
		const uint32 Fingers = bLeft ? (DigitMask & LeftFingers) >> LeftFingersShift
		                             : (DigitMask & RightFingers) >> RightFingersShift;
		return FMath::CountBits(Fingers);
	}
}

void FATR_EchoHealthModel::Init(const int32 EchoCount)
{
	SoA.Init(EchoCount);
	DetailRecords.Empty();
	PendingDeltas.Empty();

	// Healthy-baseline capability cache for the whole population. Emission is
	// SUPPRESSED: this is initialization, not a change — without the guard the
	// 10k recomputes flood the pending queue past MaxPendingDeltas and the
	// overflow collapse spams warnings / burns O(n²) at every world startup.
	bSuppressDeltaEmission = true;
	for (int32 i = 0; i < EchoCount; ++i)
	{
		RecomputeCapabilities(i);
	}
	bSuppressDeltaEmission = false;

	checkf(PendingDeltas.IsEmpty(), TEXT("Echo health Init must not queue replication deltas"));
}

void FATR_EchoHealthModel::ResetEcho(const int32 EchoIndex)
{
	if (!SoA.IsValidIndex(EchoIndex)) { return; }

	SoA.ResetEcho(EchoIndex);
	DetailRecords.Remove(EchoIndex);
	RecomputeCapabilities(EchoIndex);
	EmitDelta(EchoIndex, EATR_EchoHealthDeltaType::FullStructuralRefresh);
}

void FATR_EchoHealthModel::HandleSwapRemove(const int32 RemovedIndex, const int32 LastIndex)
{
	if (!SoA.IsValidIndex(RemovedIndex) || !SoA.IsValidIndex(LastIndex)) { return; }

	// What clients currently believe each index looks like (we only ever
	// replicated damage, so a clean row is clean on clients too).
	const bool bDestWasDamaged = IsRowDamaged(RemovedIndex);
	const bool bTailWasDamaged = IsRowDamaged(LastIndex);

	// Deltas queued for either row are now mislabeled — drop them. The rows are
	// re-announced below; the removed Echo despawns via the snapshot stream.
	PendingDeltas.RemoveAll([RemovedIndex, LastIndex](const FATR_EchoHealthDelta& D)
	{
		return D.EchoIndex == RemovedIndex || D.EchoIndex == LastIndex;
	});

	DetailRecords.Remove(RemovedIndex);

	if (RemovedIndex != LastIndex)
	{
		// Move row LastIndex into the vacated slot (mirrors the subsystem SoA swap).
		SoA.BrainIntegrity[RemovedIndex]  = SoA.BrainIntegrity[LastIndex];
		SoA.MajorPartMask[RemovedIndex]   = SoA.MajorPartMask[LastIndex];
		SoA.DigitMask[RemovedIndex]       = SoA.DigitMask[LastIndex];
		SoA.CapabilityFlags[RemovedIndex] = SoA.CapabilityFlags[LastIndex];
		// Keep the DESTINATION row's sequence monotonic: clients track sequence
		// by index, so the moved-in state must look "newer" than the slot held.
		SoA.HealthSequence[RemovedIndex] =
			FMath::Max(SoA.HealthSequence[RemovedIndex], SoA.HealthSequence[LastIndex]);

		if (FATR_EchoDetailedDamageRecord* MovedDetail = DetailRecords.Find(LastIndex))
		{
			DetailRecords.Add(RemovedIndex, *MovedDetail);
			DetailRecords.Remove(LastIndex);
		}

		// Re-announce the slot if either the moved-in Echo carries damage or the
		// previous occupant did (clients must clear the stale damage either way).
		if (bDestWasDamaged || IsRowDamaged(RemovedIndex))
		{
			EmitDelta(RemovedIndex, EATR_EchoHealthDeltaType::FullStructuralRefresh);
		}
	}

	// Reset the vacated tail row to the healthy baseline for reuse. Sequence is
	// deliberately preserved (SoA::ResetEcho) so post-reuse deltas read newer.
	SoA.ResetEcho(LastIndex);
	DetailRecords.Remove(LastIndex);
	RecomputeCapabilities(LastIndex); // healthy baseline cache (may emit CapabilityChanged)
	if (bTailWasDamaged)
	{
		// Clients last saw this index damaged (the moved row or the removed
		// Echo) — push the clean baseline so no stale gore lingers there.
		EmitDelta(LastIndex, EATR_EchoHealthDeltaType::FullStructuralRefresh);
	}
}

bool FATR_EchoHealthModel::IsRowDamaged(const int32 EchoIndex) const
{
	if (!SoA.IsValidIndex(EchoIndex)) { return false; }
	return SoA.BrainIntegrity[EchoIndex] != 255
		|| SoA.MajorPartMask[EchoIndex] != ATR_EchoParts::AllParts
		|| SoA.DigitMask[EchoIndex] != ATR_EchoDigits::AllDigits;
}

// ─────────────────────────────────────────────────────────────────────────────
// Structural damage
// ─────────────────────────────────────────────────────────────────────────────

void FATR_EchoHealthModel::ApplyBrainDamage(const int32 EchoIndex, const float Damage01)
{
	if (!SoA.IsValidIndex(EchoIndex) || Damage01 <= 0.f || IsDead(EchoIndex)) { return; }

	const uint8 Loss = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Damage01 * 255.f), 1, 255));
	uint8& Brain = SoA.BrainIntegrity[EchoIndex];
	Brain = Brain > Loss ? Brain - Loss : 0;

	RecomputeCapabilities(EchoIndex); // emits EchoKilled if the brain just died

	if (Brain > 0)
	{
		EmitDelta(EchoIndex, EATR_EchoHealthDeltaType::BrainChanged);
	}

	if (GetDefault<UATR_HealthSettings>()->bLogEchoStructuralChanges)
	{
		UE_LOG(LogATR_Health, Verbose, TEXT("Echo %d brain -%d -> %d"), EchoIndex, Loss, Brain);
	}
}

void FATR_EchoHealthModel::SeverParts(const int32 EchoIndex, const uint32 PartBits)
{
	if (!SoA.IsValidIndex(EchoIndex) || PartBits == 0 || IsDead(EchoIndex)) { return; }

	const uint32 ToRemove = ATR_EchoParts::WithDistalParts(PartBits);
	uint32& Parts = SoA.MajorPartMask[EchoIndex];
	const uint32 ActuallyRemoved = Parts & ToRemove;
	if (ActuallyRemoved == 0) { return; }

	Parts &= ~ToRemove;

	// Digits go with their hand/foot.
	uint32& Digits = SoA.DigitMask[EchoIndex];
	if (ActuallyRemoved & ATR_EchoParts::LeftHand) { Digits &= ~ATR_EchoDigits::LeftFingers; }
	if (ActuallyRemoved & ATR_EchoParts::RightHand) { Digits &= ~ATR_EchoDigits::RightFingers; }
	if (ActuallyRemoved & ATR_EchoParts::LeftFoot) { Digits &= ~ATR_EchoDigits::LeftToes; }
	if (ActuallyRemoved & ATR_EchoParts::RightFoot) { Digits &= ~ATR_EchoDigits::RightToes; }

	// Head detached -> brain destroyed -> dead (no severed-head gameplay).
	if (ActuallyRemoved & ATR_EchoParts::HeadPresent)
	{
		SoA.BrainIntegrity[EchoIndex] = 0;
	}

	// Severed (not merely non-functional) parts drive stump visuals.
	GetOrCreateDetailRecord(EchoIndex).SeveredPartBits |= ActuallyRemoved;

	RecomputeCapabilities(EchoIndex);
	EmitDelta(EchoIndex, EATR_EchoHealthDeltaType::MajorPartMaskChanged);

	if (GetDefault<UATR_HealthSettings>()->bLogEchoStructuralChanges)
	{
		UE_LOG(LogATR_Health, Verbose, TEXT("Echo %d severed parts 0x%05X -> mask 0x%05X"), EchoIndex, ActuallyRemoved, Parts);
	}
}

void FATR_EchoHealthModel::SeverDigits(const int32 EchoIndex, const uint32 DigitBits)
{
	if (!SoA.IsValidIndex(EchoIndex) || DigitBits == 0 || IsDead(EchoIndex)) { return; }

	uint32& Digits = SoA.DigitMask[EchoIndex];
	const uint32 ActuallyRemoved = Digits & DigitBits;
	if (ActuallyRemoved == 0) { return; }

	Digits &= ~DigitBits;

	GetOrCreateDetailRecord(EchoIndex); // damaged -> becomes detail-relevant

	RecomputeCapabilities(EchoIndex); // finger loss can flip grab capability
	EmitDelta(EchoIndex, EATR_EchoHealthDeltaType::DigitMaskChanged);

	if (GetDefault<UATR_HealthSettings>()->bLogEchoStructuralChanges)
	{
		UE_LOG(LogATR_Health, Verbose, TEXT("Echo %d lost digits 0x%05X -> mask 0x%05X"), EchoIndex, ActuallyRemoved, Digits);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Capability rules — cached, recomputed only on structural change
// ─────────────────────────────────────────────────────────────────────────────

void FATR_EchoHealthModel::RecomputeCapabilities(const int32 EchoIndex)
{
	using namespace ATR_EchoParts;
	using namespace ATR_EchoCapability;

	const uint32 Parts = SoA.MajorPartMask[EchoIndex];
	const uint8 Brain = SoA.BrainIntegrity[EchoIndex];
	const uint16 OldFlags = SoA.CapabilityFlags[EchoIndex];

	uint16 Flags = 0;

	if (Brain == 0 || !(Parts & HeadPresent))
	{
		// Qualified: the IsDead(int32) member function shadows the enum here.
		Flags = ATR_EchoCapability::IsDead;
	}
	else
	{
		const bool bSpine = (Parts & SpineFunctional) != 0;
		const bool bTorso = (Parts & TorsoFunctional) != 0;

		// Arm chains: grabbing needs the whole chain down to a hand.
		const bool bLeftArmChain = (Parts & LeftUpperArm) && (Parts & LeftForearm);
		const bool bRightArmChain = (Parts & RightUpperArm) && (Parts & RightForearm);
		const bool bLeftGrab = bLeftArmChain && (Parts & LeftHand);
		const bool bRightGrab = bRightArmChain && (Parts & RightHand);

		// Legs: thigh+shin stands; feet decide running.
		const bool bLeftLeg = (Parts & LeftThigh) && (Parts & LeftShin);
		const bool bRightLeg = (Parts & RightThigh) && (Parts & RightShin);
		const bool bBothFeet = (Parts & LeftFoot) && (Parts & RightFoot);

		const bool bWalk = (bLeftLeg || bRightLeg) && bSpine && bTorso;           // one bad leg = limp, still walks
		const bool bRun = bLeftLeg && bRightLeg && bBothFeet && bSpine && bTorso;

		// Crawling drags by the arms; hands not required, torso is. Spine
		// destruction makes a crawler or a twitcher per project settings.
		const bool bSpineAllowsCrawl = bSpine || GetDefault<UATR_HealthSettings>()->bSpineDestroyedAllowsCrawl;
		const bool bCrawl = !bWalk && (bLeftArmChain || bRightArmChain) && bTorso && bSpineAllowsCrawl;

		const bool bBite = (Parts & JawPresent) && (Parts & NeckFunctional);

		if (bWalk) { Flags |= CanWalk; }
		if (bRun) { Flags |= CanRun; }
		if (bCrawl) { Flags |= CanCrawl; }
		if (bLeftGrab) { Flags |= CanGrabLeft; }
		if (bRightGrab) { Flags |= CanGrabRight; }
		if (bLeftGrab || bRightGrab) { Flags |= CanGrabAny; }
		if (bBite) { Flags |= CanBite; }
		if (bLeftGrab && bRightGrab && bWalk) { Flags |= CanClimb; }
		if ((bLeftArmChain || bRightArmChain) && bWalk) { Flags |= CanBreakDoor; }
		if (bLeftArmChain || bRightArmChain) { Flags |= CanBreakWindow; }
		if (bWalk && ((Flags & CanGrabAny) || bBite)) { Flags |= CanAttackStanding; }
		if (bCrawl && ((Flags & CanGrabAny) || bBite)) { Flags |= CanAttackCrawling; }
		if (!bWalk && !bCrawl) { Flags |= IsImmobile; }
	}

	if (Flags == OldFlags) { return; }

	SoA.CapabilityFlags[EchoIndex] = Flags;

	// A fresh death outranks a plain capability delta.
	const bool bJustDied = (Flags & ATR_EchoCapability::IsDead) && !(OldFlags & ATR_EchoCapability::IsDead);
	EmitDelta(EchoIndex, bJustDied ? EATR_EchoHealthDeltaType::EchoKilled : EATR_EchoHealthDeltaType::CapabilityChanged);

	if (GetDefault<UATR_HealthSettings>()->bLogEchoStructuralChanges)
	{
		UE_LOG(LogATR_Health, Verbose, TEXT("Echo %d capabilities 0x%04X -> 0x%04X%s"),
			EchoIndex, OldFlags, Flags, bJustDied ? TEXT(" (KILLED)") : TEXT(""));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Queries
// ─────────────────────────────────────────────────────────────────────────────

bool FATR_EchoHealthModel::IsDead(const int32 EchoIndex) const
{
	return SoA.IsValidIndex(EchoIndex) && (SoA.CapabilityFlags[EchoIndex] & ATR_EchoCapability::IsDead) != 0;
}

bool FATR_EchoHealthModel::HasCapability(const int32 EchoIndex, const uint16 CapabilityBit) const
{
	return SoA.IsValidIndex(EchoIndex) && (SoA.CapabilityFlags[EchoIndex] & CapabilityBit) != 0;
}

uint16 FATR_EchoHealthModel::GetCapabilityFlags(const int32 EchoIndex) const
{
	return SoA.IsValidIndex(EchoIndex) ? SoA.CapabilityFlags[EchoIndex] : 0;
}

uint32 FATR_EchoHealthModel::GetMajorPartMask(const int32 EchoIndex) const
{
	return SoA.IsValidIndex(EchoIndex) ? SoA.MajorPartMask[EchoIndex] : 0;
}

uint32 FATR_EchoHealthModel::GetDigitMask(const int32 EchoIndex) const
{
	return SoA.IsValidIndex(EchoIndex) ? SoA.DigitMask[EchoIndex] : 0;
}

uint8 FATR_EchoHealthModel::GetBrainIntegrity(const int32 EchoIndex) const
{
	return SoA.IsValidIndex(EchoIndex) ? SoA.BrainIntegrity[EchoIndex] : 0;
}

bool FATR_EchoHealthModel::CanGrabStrong(const int32 EchoIndex, const bool bLeftHand) const
{
	if (!HasCapability(EchoIndex, bLeftHand ? ATR_EchoCapability::CanGrabLeft : ATR_EchoCapability::CanGrabRight))
	{
		return false;
	}
	const int32 Fingers = ATR_EchoDigits::CountFingers(SoA.DigitMask[EchoIndex], bLeftHand);
	return Fingers >= GetDefault<UATR_HealthSettings>()->MinFingersForStrongGrab;
}

FATR_EchoDetailedDamageRecord& FATR_EchoHealthModel::GetOrCreateDetailRecord(const int32 EchoIndex)
{
	if (FATR_EchoDetailedDamageRecord* Existing = DetailRecords.Find(EchoIndex))
	{
		return *Existing;
	}
	FATR_EchoDetailedDamageRecord& New = DetailRecords.Add(EchoIndex);
	New.GoreSeed = FMath::Rand(); // deterministic enough for cosmetics; replicated via detail sync later
	return New;
}

// ─────────────────────────────────────────────────────────────────────────────
// Replication deltas
// ─────────────────────────────────────────────────────────────────────────────

void FATR_EchoHealthModel::EmitDelta(const int32 EchoIndex, const EATR_EchoHealthDeltaType Type)
{
	if (bSuppressDeltaEmission)
	{
		return; // Init baseline — nothing to replicate, sequences stay untouched
	}

	// Bandwidth guard: collapse a runaway queue into one refresh per Echo.
	const int32 MaxPending = GetDefault<UATR_HealthSettings>()->MaxPendingDeltas;
	if (PendingDeltas.Num() >= MaxPending)
	{
		TSet<int32> DirtyEchoes;
		for (const FATR_EchoHealthDelta& D : PendingDeltas) { DirtyEchoes.Add(D.EchoIndex); }
		DirtyEchoes.Add(EchoIndex);

		PendingDeltas.Reset();
		for (const int32 Dirty : DirtyEchoes)
		{
			PendingDeltas.Add(MakeFullRefreshDelta(Dirty));
			++SoA.HealthSequence[Dirty];
		}

		UE_LOG(LogATR_Health, Warning, TEXT("Echo health delta queue overflow — collapsed to %d full refreshes"), DirtyEchoes.Num());
		return;
	}

	++SoA.HealthSequence[EchoIndex];

	FATR_EchoHealthDelta D = MakeFullRefreshDelta(EchoIndex);
	D.DeltaType = Type;
	PendingDeltas.Add(D);
}

FATR_EchoHealthDelta FATR_EchoHealthModel::MakeFullRefreshDelta(const int32 EchoIndex) const
{
	FATR_EchoHealthDelta D;
	D.EchoIndex = EchoIndex;
	D.Sequence = SoA.IsValidIndex(EchoIndex) ? SoA.HealthSequence[EchoIndex] : 0;
	D.DeltaType = EATR_EchoHealthDeltaType::FullStructuralRefresh;
	D.MajorPartMask = GetMajorPartMask(EchoIndex);
	D.DigitMask = GetDigitMask(EchoIndex);
	D.BrainIntegrity = GetBrainIntegrity(EchoIndex);
	D.CapabilityFlags = GetCapabilityFlags(EchoIndex);
	return D;
}

void FATR_EchoHealthModel::ApplyDeltaFromServer(const FATR_EchoHealthDelta& Delta)
{
	if (!SoA.IsValidIndex(Delta.EchoIndex)) { return; }

	// Serial-number arithmetic so uint16 wrap can't wedge a long-lived client.
	const uint16 Current = SoA.HealthSequence[Delta.EchoIndex];
	const int16  Ahead   = static_cast<int16>(Delta.Sequence - Current);
	if (Ahead <= 0 && Current != 0) { return; } // stale or duplicate

	SoA.HealthSequence[Delta.EchoIndex]  = Delta.Sequence;
	SoA.BrainIntegrity[Delta.EchoIndex]  = Delta.BrainIntegrity;
	SoA.MajorPartMask[Delta.EchoIndex]   = Delta.MajorPartMask;
	SoA.DigitMask[Delta.EchoIndex]       = Delta.DigitMask;
	SoA.CapabilityFlags[Delta.EchoIndex] = Delta.CapabilityFlags;
}

void FATR_EchoHealthModel::DrainPendingDeltas(TArray<FATR_EchoHealthDelta>& Out, const int32 MaxCount)
{
	const int32 Count = FMath::Min(MaxCount, PendingDeltas.Num());
	if (Count <= 0) { return; }

	Out.Append(PendingDeltas.GetData(), Count);
	PendingDeltas.RemoveAt(0, Count, EAllowShrinking::No);
}

// ─────────────────────────────────────────────────────────────────────────────
// Debug
// ─────────────────────────────────────────────────────────────────────────────

FString FATR_EchoHealthModel::GetDebugString(const int32 EchoIndex) const
{
	if (!SoA.IsValidIndex(EchoIndex))
	{
		return FString::Printf(TEXT("Echo %d: invalid index"), EchoIndex);
	}

	const uint16 Flags = SoA.CapabilityFlags[EchoIndex];
	return FString::Printf(TEXT("Echo %d: brain=%d parts=0x%05X digits=0x%05X caps=0x%04X seq=%d%s%s%s%s pending=%d"),
		EchoIndex, SoA.BrainIntegrity[EchoIndex], SoA.MajorPartMask[EchoIndex], SoA.DigitMask[EchoIndex],
		Flags, SoA.HealthSequence[EchoIndex],
		(Flags & ATR_EchoCapability::IsDead) ? TEXT(" DEAD") : TEXT(""),
		(Flags & ATR_EchoCapability::CanWalk) ? TEXT(" walk") : TEXT(""),
		(Flags & ATR_EchoCapability::CanCrawl) ? TEXT(" crawl") : TEXT(""),
		(Flags & ATR_EchoCapability::IsImmobile) ? TEXT(" immobile") : TEXT(""),
		PendingDeltas.Num());
}
