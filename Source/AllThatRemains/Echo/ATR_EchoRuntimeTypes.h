// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "ATR_EchoRuntimeTypes.generated.h"

class AActor;
class APawn;
class AATR_EchoAIController;

// ─────────────────────────────────────────────────────────────────────────────
// Canonical Echo runtime data structures.
//
// These types describe the *subsystem-authoritative* Echo intelligence model.
// The Echo subsystem owns one FATR_EchoRuntimeState per live Echo; the active
// AIController / StateTree are execution adapters that read intent/move-requests
// out of this state and report perception/movement facts back into it.
//
// Phase 1 introduces the storage only — population of awareness/intent/search is
// wired up in later phases. Names mirror the design document; values are tuned
// elsewhere (Project Settings), not here.
// ─────────────────────────────────────────────────────────────────────────────

// How much simulation fidelity an Echo currently receives. Drives whether it has
// a full actor/AIController/perception stack or only cheap subsystem data.
UENUM(BlueprintType)
enum class EATR_EchoSimulationTier : uint8
{
	Abstract,     // no actor, no AI — cell/population/pressure simulation only
	LowDetail,    // usually no actor — cheap individual stimulus/agitation/search
	VisualProxy,  // actor or ISM proxy for presentation, no full AIController
	Active        // full pawn + AIController + StateTree + perception
};

// High-level behavioral intent chosen by the subsystem and executed by the StateTree.
UENUM(BlueprintType)
enum class EATR_EchoIntent : uint8
{
	Dormant,                  // inert — no stimulus, not simulated for behavior
	Idle,                     // awake but stationary
	Wander,                   // ambient drifting
	TurnTowardStimulus,       // weak stimulus — orient only
	InvestigateLocation,      // move toward a heard/known location
	ChaseVisibleActor,        // actor confirmed visible right now
	ChaseLastSeenLocation,    // sight lost — move to last known location
	SearchProjectedDirection, // reached last-seen — search along projected travel
	FanSearchArea,            // projection exhausted — fan out search points
	JoinHordePressure,        // pulled by agitation field, not a specific target
	Attack,                   // in attack range of a confirmed target
	HandleObstacle,           // movement blocked — run obstacle fallback/break hook
	ReturnToIdle              // memory/urgency expired — wind down
};

// What kind of awareness is currently driving an Echo. Mirrors EATR_EchoIntent but
// describes the *source* of knowledge rather than the chosen behavior.
UENUM(BlueprintType)
enum class EATR_AwarenessMode : uint8
{
	None,
	HeardLocation,   // location-only knowledge from a noise stimulus
	SawTarget,       // currently-confirmed visible actor
	LostSightSearch, // had sight, now operating on memory/search
	SmellTrail,      // (future) following a smell trail
	HordeAgitated,   // pulled by indirect agitation/pressure
	ObstacleBlocked  // movement blocked by an obstacle
};

// Category of a stimulus event ingested by the subsystem. Stimuli are location-based;
// they never grant direct actor-target knowledge (see SourceActor_DebugOnly below).
UENUM(BlueprintType)
enum class EATR_StimulusType : uint8
{
	Noise,
	Smell,
	Blood,
	EchoAgitation,
	DoorImpact,
	WindowImpact,
	Combat,
	Scripted
};

// What a move request targets. Replaces the old implicit "null actor → move to
// FVector::ZeroVector" ambiguity in the legacy move task.
UENUM(BlueprintType)
enum class EATR_EchoMoveTargetType : uint8
{
	None,
	Actor,
	Location
};

// Why a movement request failed. Feeds obstacle hooks and failure-classified search.
UENUM(BlueprintType)
enum class EATR_MoveFailureReason : uint8
{
	None,
	InvalidTarget,
	NoPath,
	BlockedByDynamicActor,
	BlockedByDoor,
	BlockedByWindow,
	BlockedByFence,
	TargetUnreachable,
	NavmeshMissing,
	AbortedByNewIntent
};

// Confirmed/remembered knowledge an Echo holds about a threat. Sight confirms an
// actor only while currently visible; once lost, behavior runs off the cached
// locations/velocities here. Hearing only ever writes LastHeardLocation.
struct FATR_EchoAwarenessState
{
	EATR_AwarenessMode Mode = EATR_AwarenessMode::None;

	// Set only while the actor is currently visible. Weak so destruction auto-clears.
	TWeakObjectPtr<AActor> ConfirmedVisibleActor;

	FVector LastSeenLocation        = FVector::ZeroVector;
	FVector LastSeenVelocity        = FVector::ZeroVector;
	FVector ProjectedSearchLocation = FVector::ZeroVector;

	FVector LastHeardLocation       = FVector::ZeroVector;
	FVector LastSmelledLocation     = FVector::ZeroVector;
	FVector HordePressureDirection  = FVector::ZeroVector;

	float LastSeenTime    = -1.f;
	float LastHeardTime   = -1.f;
	float LastSmelledTime = -1.f;

	float Confidence = 0.f; // 0..1 — decays over time; gates search escalation
	float Urgency    = 0.f; // 0..1 — how aggressively to pursue/investigate

	bool bHasCurrentLineOfSight = false;
};

// Directional / fan search progression state for lost-target behavior.
struct FATR_EchoSearchState
{
	FVector Origin          = FVector::ZeroVector;
	FVector PrimaryDirection = FVector::ForwardVector;

	int32 SearchStepIndex = 0;

	float StartedTime       = -1.f;
	float SearchRadius      = 600.f;
	float MaxSearchDuration = 12.f;

	bool bSearchActive = false;
};

// A single world stimulus (noise, blood, impact, etc.). Location-based by design.
USTRUCT(BlueprintType)
struct FATR_StimulusEvent
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Stimulus")
	EATR_StimulusType Type = EATR_StimulusType::Noise;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Stimulus")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Stimulus")
	FVector Direction = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Stimulus")
	float Strength = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Stimulus")
	float Radius = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Stimulus")
	float TimeSeconds = 0.f;

	// Optional metadata ONLY — for analytics/debug/noise-ownership/gameplay credit.
	// Must never be consumed as behavioral target knowledge.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Stimulus")
	TWeakObjectPtr<AActor> SourceActor_DebugOnly;
};

// Explicit movement order. Type disambiguates actor vs location so movement can
// never accidentally path to world origin.
USTRUCT(BlueprintType)
struct FATR_EchoMoveRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Move")
	EATR_EchoMoveTargetType Type = EATR_EchoMoveTargetType::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Move")
	TWeakObjectPtr<AActor> Actor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Move")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Echo|Move")
	float AcceptanceRadius = 50.f;
};

// Last/most-recent movement execution facts reported back from the active layer.
// "Equivalent" to the document's FATR_EchoMovementIntent — holds the outstanding
// request plus the most recent result so the subsystem can react to blocked/aborted moves.
struct FATR_EchoMovementIntent
{
	FATR_EchoMoveRequest   Request;
	EATR_MoveFailureReason LastFailure = EATR_MoveFailureReason::None;

	bool  bMoveInProgress  = false;
	bool  bLastMoveSucceeded = false;
	float LastResultTime   = -1.f;
};

// One cell of the indirect horde-agitation field. Agitation spreads as a scalar pressure
// plus a weighted direction — Echoes sample it to become curious / investigate / join a
// horde WITHOUT ever receiving another Echo's exact target actor.
struct FATR_AgitationCell
{
	float   Agitation         = 0.f;
	FVector WeightedDirection  = FVector::ZeroVector; // accumulated direction*amount; normalize on read
	float   LastUpdatedTime    = -1.f;
};

// Obstacle hook record. Breaking is deferred, but the classified failure + location
// are recorded now so HandleObstacle has something to act on.
struct FATR_EchoObstacleIntent
{
	bool bHasObstacle = false;

	EATR_MoveFailureReason Reason = EATR_MoveFailureReason::None;

	TWeakObjectPtr<AActor> ObstacleActor;
	FVector ObstacleLocation = FVector::ZeroVector;

	float LastObstacleTime = -1.f;
};

// Canonical per-Echo runtime state owned by the subsystem. One per live Echo,
// stored parallel to the SoA arrays and addressed by a *stable* EchoId (the SoA
// index is not stable across swap-remove). Plain struct — never GC-traced; weak
// pointers inside auto-clear on destruction.
struct FATR_EchoRuntimeState
{
	int32 EchoId = INDEX_NONE;

	EATR_EchoSimulationTier Tier   = EATR_EchoSimulationTier::Abstract;
	EATR_EchoIntent         Intent = EATR_EchoIntent::Dormant;

	FVector Location         = FVector::ZeroVector;
	FVector Velocity         = FVector::ZeroVector;
	FVector FacingDirection  = FVector::ForwardVector;

	FATR_EchoAwarenessState Awareness;
	FATR_EchoSearchState    Search;
	FATR_EchoMovementIntent Movement;
	FATR_EchoObstacleIntent Obstacle;

	float Aggression    = 0.5f;
	float Agitation     = 0.f;
	float LastUpdateTime = -1.f;

	// Active-layer bridge — valid only while Tier == Active. Weak so a pooled/destroyed
	// controller or pawn auto-nulls without dangling.
	TWeakObjectPtr<class AATR_EchoAIController> ActiveController;
	TWeakObjectPtr<APawn>                       ActivePawn;

	// Reset everything except the identity. Used when a freed SoA slot is reused.
	void ResetForReuse(int32 InEchoId)
	{
		*this = FATR_EchoRuntimeState{};
		EchoId = InEchoId;
	}
};
