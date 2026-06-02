#pragma once

#include "CoreMinimal.h"
#include "StateTreeTaskBase.h"
#include "AIController.h"
#include "ZombieMoveToTask.generated.h"

// 1. Define the data the task needs. Bindings in the editor will link to these.
USTRUCT(BlueprintType)
struct FZombieMoveToTaskInstanceData
{
	GENERATED_BODY()

	// The player to chase (Input)
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<AActor> TargetActor = nullptr;

	// The location to search (Input)
	UPROPERTY(EditAnywhere, Category = "Input")
	FVector TargetLocation = FVector::ZeroVector;

	// How close we need to get
	UPROPERTY(EditAnywhere, Category = "Parameter")
	float AcceptanceRadius = 50.0f;
};

// 2. Define the logic of the task
USTRUCT(BlueprintType, meta = (DisplayName = "Zombie Move To"))
struct FZombieMoveToTask : public FStateTreeTaskCommonBase
{
	GENERATED_BODY()

	using FInstanceDataType = FZombieMoveToTaskInstanceData;

	// Tells State Tree which data struct to use
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	// Called when the State becomes active
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	
	// Called every frame while the state is active
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
};
```

#### ZombieMoveToTask.cpp
```cpp
#include "ZombieMoveToTask.h"
#include "AIController.h"
#include "VisualLogger/VisualLogger.h"
#include "StateTreeExecutionContext.h"

EStateTreeRunStatus FZombieMoveToTask::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());

	if (!AIC) return EStateTreeRunStatus::Failed;

	// Trigger the move command immediately on entry
	if (InstanceData.TargetActor)
	{
		AIC->MoveToActor(InstanceData.TargetActor, InstanceData.AcceptanceRadius);
	}
	else
	{
		AIC->MoveToLocation(InstanceData.TargetLocation, InstanceData.AcceptanceRadius);
	}

	return EStateTreeRunStatus::Running;
}

EStateTreeRunStatus FZombieMoveToTask::Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const
{
	AAIController* AIC = Cast<AAIController>(Context.GetOwner());
	if (!AIC) return EStateTreeRunStatus::Failed;

	// Check the AI's pathfollowing status
	EPathFollowingStatus::Type Status = AIC->GetMoveStatus();

	if (Status == EPathFollowingStatus::Idle)
	{
		// We reached the destination or the path failed
		return EStateTreeRunStatus::Succeeded;
	}

	return EStateTreeRunStatus::Running;
}
```

---

### 2. Implementation in the State Tree Editor

1.  **Context Actor:** Set the Context Actor to `AIController`.
2.  **Parameters/Evaluators:** Use a C++ **Evaluator** (or your existing C++ Perception logic) to populate global State Tree variables: `TargetActor` and `LastSeenLocation`.
3.  **States:**
    *   **Chase State:**
        *   Add the `Zombie Move To` task.
        *   **Bind** `Target Actor` to your "Seen Player" variable.
        *   **Transition:** On `State Succeeded` (meaning we reached them) or if the variable becomes null.
    *   **Search State:**
        *   Add the same `Zombie Move To` task.
        *   **Bind** `Target Location` to your "Last Seen Location" variable.
        *   **Transition:** On `State Succeeded`, move to an "Idle/Wander" state.

### 3. Why this solves your "Abuse" problem
*   **Performance:** Unlike Behavior Trees which "force" a re-evaluation of the whole tree, State Trees only evaluate transitions. This is massive for multiplayer servers.
*   **Hysteresis via Transitions:** You can add a **Condition** to the transition from "Chase" to "Search." For example: "Only transition to Search if I haven't seen the player for 2.0 seconds." 
*   **Explicit Data:** Because the `TargetActor` is an explicit input to the task, the zombie won't "flicker" between targets unless your C++ scoring logic (provided in the previous turn) explicitly updates that variable.

### 4. Multiplayer Consideration
Ensure that your `AIController` is only running the State Tree on the **Server**. In your `AIController`'s `BeginPlay`:
```cpp
if (HasAuthority()) 
{
    StateTreeComponent->StartLogic();
}
```
This ensures the zombie’s movement is authoritative and synchronized for all players.