// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ATR_EchoHealthTypes.h"

// ─────────────────────────────────────────────────────────────────────────────
// Echo structural health model — behavior over FATR_EchoHealthSoA.
//
// Pure mechanics, no UObject, no ticking: every method is called in response
// to a damage event (event-driven, like the human pipeline). The Echo
// subsystem will own one instance and route melee/ballistic results into it
// after review; the replication component will drain DrainPendingDeltas into
// its existing delta stream. Nothing here references existing Echo code.
//
// Rules implemented (design doc):
//   brain destroyed            -> dead
//   head detached              -> brain = 0 -> dead (no severed-head gameplay)
//   jaw missing                -> cannot bite
//   no functional arms         -> no grab
//   one bad leg                -> walk (limp), no run
//   no legs                    -> crawl if an arm works
//   no arms and no legs        -> immobile while brain intact
//   spine destroyed            -> immobile or crawler (UATR_HealthSettings)
//
// Capability flags are CACHED: recomputed only when structure changes.
// ─────────────────────────────────────────────────────────────────────────────
class ALLTHATREMAINS_API FATR_EchoHealthModel
{
public:
	// Allocate rows for the whole population (matches the Echo subsystem's
	// SoA sizing; called once at init).
	void Init(int32 EchoCount);

	// Reuse a row for a newly spawned Echo (pooling); emits a refresh delta.
	void ResetEcho(int32 EchoIndex);

	// ── Structural damage entry points (all server-side, event-driven) ─────

	// Brain damage 0..1 of total integrity. The ONLY path to death.
	void ApplyBrainDamage(int32 EchoIndex, float Damage01);

	// Sever/destroy parts (ATR_EchoParts bits). Distal parts and their digits
	// go with them. Severing the head zeroes the brain (rule above).
	void SeverParts(int32 EchoIndex, uint32 PartBits);

	// Destroy individual digits (ATR_EchoDigits bits).
	void SeverDigits(int32 EchoIndex, uint32 DigitBits);

	// Mark spine/torso/neck non-functional without visible severing
	// (deep trauma). Same mask, different verb for call-site clarity.
	void DestroyFunction(int32 EchoIndex, uint32 PartBits) { SeverParts(EchoIndex, PartBits); }

	// ── Queries ────────────────────────────────────────────────────────────

	bool IsDead(int32 EchoIndex) const;
	bool HasCapability(int32 EchoIndex, uint16 CapabilityBit) const;
	uint16 GetCapabilityFlags(int32 EchoIndex) const;
	uint32 GetMajorPartMask(int32 EchoIndex) const;
	uint32 GetDigitMask(int32 EchoIndex) const;
	uint8 GetBrainIntegrity(int32 EchoIndex) const;

	// True when this hand can make STRONG grabs (enough fingers — threshold
	// from UATR_HealthSettings). A present hand below the threshold grabs weak.
	bool CanGrabStrong(int32 EchoIndex, bool bLeftHand) const;

	const FATR_EchoHealthSoA& GetSoA() const { return SoA; }

	// ── Sparse detail (only for damaged, relevant Echoes) ──────────────────

	FATR_EchoDetailedDamageRecord& GetOrCreateDetailRecord(int32 EchoIndex);
	const FATR_EchoDetailedDamageRecord* FindDetailRecord(int32 EchoIndex) const { return DetailRecords.Find(EchoIndex); }
	void ClearDetailRecord(int32 EchoIndex) { DetailRecords.Remove(EchoIndex); }

	// ── Replication delta queue ────────────────────────────────────────────

	// Move up to MaxCount pending deltas into Out (FIFO). The replication
	// component calls this each net update with its bandwidth cap.
	void DrainPendingDeltas(TArray<FATR_EchoHealthDelta>& Out, int32 MaxCount);

	int32 NumPendingDeltas() const { return PendingDeltas.Num(); }

	// Full-state delta for newly-relevant or out-of-sync clients.
	FATR_EchoHealthDelta MakeFullRefreshDelta(int32 EchoIndex) const;

	// ── Debug ──────────────────────────────────────────────────────────────

	FString GetDebugString(int32 EchoIndex) const;

private:
	// Recompute cached capability flags from masks/brain. Called only when
	// structure changed; emits CapabilityChanged when the cache moves.
	void RecomputeCapabilities(int32 EchoIndex);

	// Queue a delta snapshotting current state. Bumps the per-Echo sequence.
	// On queue overflow (settings cap) the queue collapses into one
	// FullStructuralRefresh per dirty Echo.
	void EmitDelta(int32 EchoIndex, EATR_EchoHealthDeltaType Type);

	FATR_EchoHealthSoA SoA;

	// Sparse: only damaged/relevant Echoes have entries.
	TMap<int32, FATR_EchoDetailedDamageRecord> DetailRecords;

	TArray<FATR_EchoHealthDelta> PendingDeltas;
};
