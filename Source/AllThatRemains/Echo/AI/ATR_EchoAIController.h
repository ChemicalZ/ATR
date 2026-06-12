// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Components/StateTreeComponent.h"
#include "../ATR_EchoRuntimeTypes.h"
#include "ATR_EchoAIController.generated.h"

class UATR_EchoSubsystem;

// Pooled AI controller for AATR_ActiveEcho.
//
// Owns all AI — perception and behavior. The character owns only movement.
//
// Lifecycle mirrors the actor pool:
//   Spawn → EnterPool()          (dormant: perception off, StateTree stopped, delegate unbound)
//   Possess(Echo) → OnPossess()  (active: perception on, delegate bound, StateTree running)
//   UnPossess()  → OnUnPossess() (stops movement, unbinds delegate, dormant)
//   Return to ControllerPool
//
// Team: Echoes are Team 2 (see TeamNumber default below). Sight/hearing ignore friendlies,
// so echoes never perceive each other. Players have no team (NoTeam) and are treated as
// neutral → detected. The controller and the possessed AATR_ActiveEcho must share this team
// id for friendly-filtering to work; both default to 2.
UCLASS()
class ALLTHATREMAINS_API AATR_EchoAIController : public AAIController
{
	GENERATED_BODY()

public:
	AATR_EchoAIController();

	virtual FGenericTeamId GetGenericTeamId() const override { return FGenericTeamId(TeamNumber); }
	virtual void SetGenericTeamId(const FGenericTeamId& NewId) override { TeamNumber = NewId.GetId(); }

	// --- Components ---

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UAIPerceptionComponent> AIPerception;

	// Assign a StateTree asset to this component in your Blueprint subclass.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Echo|Components")
	TObjectPtr<UStateTreeComponent> StateTreeComp;

	// --- Config ---

	// Hearing range (cm). Single source of truth for both the hearing sense config and the
	// distance-falloff applied when a heard noise is reported as a stimulus. Mirrored from
	// UATR_EchoSettings.ActiveHearingRange at possess time.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Echo|AI", meta = (ClampMin = 1.f, ForceUnits = "cm"))
	float HearingRange = 3000.f;

	// Forward trace length used to identify what physically blocked a move.
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Echo|AI", meta = (ClampMin = 0.f, ForceUnits = "cm"))
	float ObstacleTraceDistance = 200.f;

	// Stable EchoId of the possessed Echo, resolved at OnPossess. INDEX_NONE when pooled.
	int32 GetEchoId() const { return CachedEchoId; }

	// Issue an explicit, typed move for the Echo move task. Rejects None/invalid requests
	// (never moves to world origin). Returns the path-following request code so the task can
	// branch on AlreadyAtGoal / Failed / Running. The classified completion result is
	// reported to the subsystem via HandleMoveCompleted.
	EPathFollowingRequestResult::Type IssueMoveRequest(const FATR_EchoMoveRequest& Request);

	// Resolution of a move whose completion is reported asynchronously to the subsystem. A
	// StateTree task records the serial returned by GetLastIssuedMoveSerial() in EnterState and
	// polls GetMoveOutcomeForSerial() in Tick to decide Running / Succeeded / Failed — instead of
	// inferring success from path-following going Idle.
	enum class EEchoMoveOutcome : uint8 { Pending, Succeeded, Failed };

	// Serial stamped on the most recently issued move request (monotonic per Echo).
	uint32 GetLastIssuedMoveSerial() const { return LastIssuedMoveSerial; }

	// Pending while the given serial is still the outstanding request; Succeeded/Failed once its
	// classified completion is recorded; Failed if a newer request superseded it or state is gone.
	EEchoMoveOutcome GetMoveOutcomeForSerial(uint32 Serial) const;

	// --- Pool ---

	// Stop all AI logic and clear state. Called after UnPossess when returning to pool.
	void EnterPool();

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Team")
	uint8 TeamNumber = 2;
private:
	// Batch perception callback — fires once per perception tick after all stimuli are processed.
	UFUNCTION()
	void HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors);

	// Walk this update's perceived actors and report sight/hearing facts into the
	// subsystem's canonical awareness state. Hearing is reported as location-only.
	void ReportPerceptionFacts(const TArray<AActor*>& UpdatedActors);

	// Enable/disable the perception senses and (on disable) forget all perception memory.
	// Keeps a pooled controller from carrying stale stimuli into its next possession.
	void SetSensesEnabled(bool bEnabled);

	// Movement completion callback bound to ReceiveMoveCompleted. Classifies the result
	// and reports it to the subsystem (success or a EATR_MoveFailureReason).
	UFUNCTION()
	void HandleMoveCompleted(FAIRequestID RequestID, EPathFollowingResult::Type Result);

	// Forward a classified move result to the subsystem for the possessed Echo.
	void ReportMoveResultToSubsystem(bool bSuccess, EATR_MoveFailureReason Reason, float TimeSeconds, AActor* BlockingActor);

	// Identify what blocked the pawn via a short forward trace and classify it by actor tag
	// (door/window/fence) — generic dynamic block otherwise. OutBlocker may be null.
	EATR_MoveFailureReason ClassifyBlockingObstacle(AActor*& OutBlocker) const;

	// Previous visible sample of the confirmed target, used to derive observed velocity from a
	// position delta instead of reading the actor's movement component. No-cheat: this only ever
	// holds samples taken while the target was actually visible, and is cleared on sight loss.
	TWeakObjectPtr<AActor> PrevVisibleActor;
	FVector                PrevVisibleLocation = FVector::ZeroVector;
	float                  PrevVisibleTime     = -1.f;

	// Resolved once at OnPossess from the possessed AATR_ActiveEcho's SoA row.
	// Stable for the Echo's lifetime (EchoId never changes; SoA index can).
	int32 CachedEchoId = INDEX_NONE;

	// World subsystem cache — world subsystems outlive controllers, so a raw observer
	// pointer is safe. Cleared on unpossess/pool to avoid use across worlds.
	UATR_EchoSubsystem* CachedSubsystem = nullptr;

	// Id of the most recently submitted move; used to ignore stale completion callbacks.
	FAIRequestID ActiveMoveRequestId;

	// Serial of the most recently issued move request (mirrors Movement.MoveRequestSerial).
	uint32 LastIssuedMoveSerial = 0;
};