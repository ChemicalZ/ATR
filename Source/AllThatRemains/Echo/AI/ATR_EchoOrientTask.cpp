// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoOrientTask.h"
#include "ATR_EchoAIController.h"
#include "../ATR_EchoSettings.h"
#include "StateTreeExecutionContext.h"
#include "GameFramework/Pawn.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

EStateTreeRunStatus FATR_EchoOrientTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(EchoAI_OrientTask_Tick);

	const FInstanceDataType& Data = Context.GetInstanceData(*this);

	const AATR_EchoAIController* AIC = Cast<AATR_EchoAIController>(Context.GetOwner());
	APawn* Pawn = AIC ? AIC->GetPawn() : nullptr;
	if (!Pawn)
		return EStateTreeRunStatus::Running;

	FVector ToTarget = Data.TargetLocation - Pawn->GetActorLocation();
	ToTarget.Z = 0.f;
	if (ToTarget.IsNearlyZero())
		return EStateTreeRunStatus::Running; // no meaningful direction yet

	const float Rate       = GetDefault<UATR_EchoSettings>()->OrientTurnRateDegPerSec;
	const float DesiredYaw = ToTarget.Rotation().Yaw;
	const float CurYaw     = Pawn->GetActorRotation().Yaw;
	const float NewYaw     = FMath::FixedTurn(CurYaw, DesiredYaw, Rate * DeltaTime);

	Pawn->SetActorRotation(FRotator(0.f, NewYaw, 0.f));
	return EStateTreeRunStatus::Running;
}
