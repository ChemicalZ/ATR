// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ATR_EchoHealthTypes.generated.h"

// ─────────────────────────────────────────────────────────────────────────────
// Echo structural health — data layout.
//
// Echoes fail STRUCTURALLY, never physiologically: no pain, fear, fatigue,
// blood pressure, oxygen, nutrition, sleep, infection, fever, shock, or body
// temperature. Death comes only from brain destruction. Everything else
// (missing limbs, jaw, digits, spine) just changes capability.
//
// Layout is SoA and replication-delta-friendly: one row of parallel arrays
// per Echo, masks for parts/digits, cached capability flags, and a per-Echo
// sequence number for client delta ordering. Sparse detail records exist only
// for Echoes that are actually damaged ("do not tick unused fine detail").
//
// Owned/driven by FATR_EchoHealthModel. The Echo subsystem links this in
// after review — nothing here touches existing Echo code.
// ─────────────────────────────────────────────────────────────────────────────

// MajorPartMask bits. 1 = present/functional, 0 = missing/destroyed.
namespace ATR_EchoParts
{
	enum Bits : uint32
	{
		HeadPresent       = 1u << 0,
		JawPresent        = 1u << 1,
		NeckFunctional    = 1u << 2,
		TorsoFunctional   = 1u << 3,
		LeftUpperArm      = 1u << 4,
		LeftForearm       = 1u << 5,
		LeftHand          = 1u << 6,
		RightUpperArm     = 1u << 7,
		RightForearm      = 1u << 8,
		RightHand         = 1u << 9,
		LeftThigh         = 1u << 10,
		LeftShin          = 1u << 11,
		LeftFoot          = 1u << 12,
		RightThigh        = 1u << 13,
		RightShin         = 1u << 14,
		RightFoot         = 1u << 15,
		SpineFunctional   = 1u << 16,
		// 17-19 reserved

		AllParts = (1u << 17) - 1
	};

	// Distal dependencies: losing a proximal part takes everything below it.
	// (Forearm gone -> hand gone; thigh gone -> shin+foot gone; head gone ->
	// jaw gone and the Echo is dead — see FATR_EchoHealthModel.)
	ALLTHATREMAINS_API uint32 WithDistalParts(uint32 PartBits);
}

// DigitMask bits. 1 = present/functioning.
namespace ATR_EchoDigits
{
	enum Bits : uint32
	{
		LeftFingersShift  = 0,  // bits 0-4, thumb = bit 0
		RightFingersShift = 5,  // bits 5-9
		LeftToesShift     = 10, // bits 10-14
		RightToesShift    = 15, // bits 15-19

		LeftFingers  = 0x1Fu << LeftFingersShift,
		RightFingers = 0x1Fu << RightFingersShift,
		LeftToes     = 0x1Fu << LeftToesShift,
		RightToes    = 0x1Fu << RightToesShift,

		AllDigits = LeftFingers | RightFingers | LeftToes | RightToes
	};

	ALLTHATREMAINS_API int32 CountFingers(uint32 DigitMask, bool bLeft);
}

// Cached capability flags. Recomputed ONLY when structure changes.
namespace ATR_EchoCapability
{
	enum Bits : uint16
	{
		CanWalk           = 1u << 0,
		CanRun            = 1u << 1,
		CanCrawl          = 1u << 2,
		CanGrabLeft       = 1u << 3,
		CanGrabRight      = 1u << 4,
		CanGrabAny        = 1u << 5,
		CanBite           = 1u << 6,
		CanClimb          = 1u << 7,
		CanBreakDoor      = 1u << 8,
		CanBreakWindow    = 1u << 9,
		CanAttackStanding = 1u << 10,
		CanAttackCrawling = 1u << 11,
		IsImmobile        = 1u << 12,
		IsDead            = 1u << 13
	};
}

// What changed in a structural delta (compact wire format).
UENUM(BlueprintType)
enum class EATR_EchoHealthDeltaType : uint8
{
	BrainChanged,
	MajorPartMaskChanged,
	DigitMaskChanged,
	CapabilityChanged,
	EchoKilled,
	FullStructuralRefresh
};

// One compact structural delta. The server never replicates full Echo health
// state unless newly relevant or out of sync — it sends these. Clients apply
// by per-Echo sequence; the server stays authoritative.
USTRUCT(BlueprintType)
struct FATR_EchoHealthDelta
{
	GENERATED_BODY()

	UPROPERTY() int32 EchoIndex = INDEX_NONE;

	// Per-Echo monotonic sequence; clients drop stale/out-of-order deltas
	// (or request FullStructuralRefresh on a gap).
	UPROPERTY() uint16 Sequence = 0;

	UPROPERTY() EATR_EchoHealthDeltaType DeltaType = EATR_EchoHealthDeltaType::FullStructuralRefresh;

	// Snapshot of the changed fields (full snapshot for refresh/kill).
	UPROPERTY() uint32 MajorPartMask = 0;
	UPROPERTY() uint32 DigitMask = 0;
	UPROPERTY() uint8 BrainIntegrity = 0;
	UPROPERTY() uint16 CapabilityFlags = 0;
};

// Sparse per-Echo gore/damage detail. Created ONLY for damaged, relevant
// Echoes (visual dismemberment, wound presentation). Never allocated for the
// horde at large.
USTRUCT()
struct FATR_EchoDetailedDamageRecord
{
	GENERATED_BODY()

	// Deterministic seed for gore/dismember visuals on clients.
	UPROPERTY() uint32 GoreSeed = 0;

	// Part bits that were severed (vs. simply non-functional) — drives
	// stump/dangle visuals.
	UPROPERTY() uint32 SeveredPartBits = 0;

	// Accumulated cosmetic damage 0..255 (decals, exposed bone tiers).
	UPROPERTY() uint8 AccumulatedDamage = 0;
};

// SoA storage: index-parallel arrays, one row per Echo. Mirrors the design
// doc's FEchoHealthSoA. Kept as a plain aggregate — FATR_EchoHealthModel owns
// behavior; the Echo subsystem will own the instance after linking.
struct FATR_EchoHealthSoA
{
	TArray<uint8> BrainIntegrity;    // 0 = destroyed (dead), 255 = intact
	TArray<uint32> MajorPartMask;    // ATR_EchoParts bits
	TArray<uint32> DigitMask;        // ATR_EchoDigits bits
	TArray<uint16> CapabilityFlags;  // ATR_EchoCapability bits, cached
	TArray<uint16> HealthSequence;   // per-Echo delta ordering

	int32 Num() const { return BrainIntegrity.Num(); }

	bool IsValidIndex(const int32 Index) const { return BrainIntegrity.IsValidIndex(Index); }

	void Init(const int32 Count)
	{
		BrainIntegrity.Init(255, Count);
		MajorPartMask.Init(ATR_EchoParts::AllParts, Count);
		DigitMask.Init(ATR_EchoDigits::AllDigits, Count);
		CapabilityFlags.Init(0, Count);
		HealthSequence.Init(0, Count);
	}

	void ResetEcho(const int32 Index)
	{
		BrainIntegrity[Index] = 255;
		MajorPartMask[Index] = ATR_EchoParts::AllParts;
		DigitMask[Index] = ATR_EchoDigits::AllDigits;
		CapabilityFlags[Index] = 0;
		// HealthSequence intentionally NOT reset: clients must see post-reuse
		// deltas as newer than anything from the previous occupant.
	}
};
