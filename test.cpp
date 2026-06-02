#include "MyAIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/Actor.h"

void AMyAIController::BeginPlay()
{
    Super::BeginPlay();
    if (GetPerceptionComponent())
    {
        GetPerceptionComponent()->OnPerceptionUpdated.AddDynamic(this, &AMyAIController::HandlePerceptionUpdated);
    }
}

void AMyAIController::HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors)
{
    // Ensure this logic only runs on the Server
    if (!HasAuthority()) return;

    AActor* BestTarget = nullptr;
    float HighestScore = -1.0f;

    // We use GetKnownPerceivedActors to include players we might have lost sight of 
    // but are still in our memory (to handle "chasing around corners")
    TArray<AActor*> PerceivedActors;
    GetPerceptionComponent()->GetKnownPerceivedActors(nullptr, PerceivedActors);

    AActor* CurrentTarget = Cast<AActor>(GetBlackboardComponent()->GetValueAsObject(FName("TargetActor")));

    for (AActor* PotentialTarget : PerceivedActors)
    {
        if (!IsValid(PotentialTarget)) continue;

        // 1. Calculate Base Score (e.g., Inverse Distance: closer is better)
        float DistanceSq = GetPawn()->GetSquaredDistanceTo(PotentialTarget);
        float Score = 1000000.0f / (DistanceSq + 1.0f); 

        // 2. Apply Hysteresis (Loyalty Bonus)
        // If this actor is our current target, give them a 20% score boost.
        // This prevents the AI from "flickering" between two players at equal distance.
        if (PotentialTarget == CurrentTarget)
        {
            Score *= 1.2f; 
        }

        // 3. Update Best Target
        if (Score > HighestScore)
        {
            HighestScore = Score;
            BestTarget = PotentialTarget;
        }
    }

    // 4. Update the Blackboard
    if (BestTarget != CurrentTarget)
    {
        GetBlackboardComponent()->SetValueAsObject(FName("TargetActor"), BestTarget);
        UE_LOG(LogTemp, Log, TEXT("AI switched target to: %s"), BestTarget ? *BestTarget->GetName() : TEXT("None"));
    }
}
```

### Why this works for Multiplayer:
1.  **Batch Processing:** Instead of re-calculating the "best" target 5 times if 5 players enter a room at once, `OnPerceptionUpdated` waits for the perception tick to finish and runs the loop once.
2.  **Memory over Sight:** By using `GetKnownPerceivedActors`, your scoring logic can account for players who just ducked behind a pillar. If you used `GetCurrentlyPerceivedActors`, the AI would "forget" the player instantly, causing the score to drop to zero and the AI to stop mid-chase.
3.  **The Hysteresis Buffer:** The `Score *= 1.2f` (20% bonus) is the secret sauce. It ensures that a new player has to be 20% "more attractive" as a target to break the AI's current focus. You can expose this `LoyaltyBonus` as a variable to tune how "stubborn" your AI is.

### Performance Tip
If you have a very high number of actors, you can further optimize by checking `UAIPerceptionComponent::GetActorsPerception` inside the loop to see if the `SuccessfullySensed` stimulus for Sight is still active, allowing you to prioritize visible targets over remembered ones in your scoring math.


=========


#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "AZombieAIController.generated.h"

UCLASS()
class MYGAME_API AZombieAIController : public AAIController
{
    GENERATED_BODY()

public:
    AZombieAIController();

protected:
    virtual void BeginPlay() override;

    // The batch update function
    UFUNCTION()
    void HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors);

    // Scoring logic to handle target switching and hysteresis
    float CalculateTargetScore(AActor* PotentialTarget, const FAIStimulus& Stimulus, AActor* CurrentTarget);

    UPROPERTY(EditDefaultsOnly, Category = "AI|Tuning")
    float LoyaltyBonusMultiplier = 1.2f; // 20% bonus to stay on same target

    UPROPERTY(EditDefaultsOnly, Category = "AI|Tuning")
    FName TargetActorKey = "TargetActor";

    UPROPERTY(EditDefaultsOnly, Category = "AI|Tuning")
    FName SearchLocationKey = "SearchLocation";
};
```

#### AZombieAIController.cpp
```cpp
#include "AZombieAIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "BehaviorTree/BlackboardComponent.h"

AZombieAIController::AZombieAIController()
{
    // Ensure you have an AIPerceptionComponent assigned in the editor or create one here
}

void AZombieAIController::BeginPlay()
{
    Super::BeginPlay();
    if (GetPerceptionComponent())
    {
        GetPerceptionComponent()->OnPerceptionUpdated.AddDynamic(this, &AZombieAIController::HandlePerceptionUpdated);
    }
}

void AZombieAIController::HandlePerceptionUpdated(const TArray<AActor*>& UpdatedActors)
{
    if (!HasAuthority()) return; // Server only logic

    TArray<AActor*> PerceivedActors;
    GetPerceptionComponent()->GetKnownPerceivedActors(UAISense_Sight::StaticClass(), PerceivedActors);

    AActor* BestTarget = nullptr;
    float HighestScore = -1.0f;
    FAIStimulus BestStimulus;

    AActor* CurrentTarget = Cast<AActor>(GetBlackboardComponent()->GetValueAsObject(TargetActorKey));

    for (AActor* Actor : PerceivedActors)
    {
        FActorPerceptionBlueprintInfo Info;
        GetPerceptionComponent()->GetActorsPerception(Actor, Info);

        // Find the sight stimulus
        for (const FAIStimulus& Stim : Info.LastSensedStimuli)
        {
            if (Stim.Type == UAISense_Sight::GetSenseID())
            {
                float Score = CalculateTargetScore(Actor, Stim, CurrentTarget);
                if (Score > HighestScore)
                {
                    HighestScore = Score;
                    BestTarget = Actor;
                    BestStimulus = Stim;
                }
                break;
            }
        }
    }

    if (BestTarget)
    {
        // Update Search Location always (even if we can't see them, this is their last known pos)
        GetBlackboardComponent()->SetValueAsVector(SearchLocationKey, BestStimulus.GetLastStoredLocation());

        if (BestStimulus.WasSuccessfullySensed())
        {
            // We HAVE sight: Update TargetActor
            GetBlackboardComponent()->SetValueAsObject(TargetActorKey, BestTarget);
        }
        else
        {
            // We LOST sight: Clear TargetActor but SearchLocation remains set
            // The Behavior Tree will now transition from "Chase" to "Search"
            GetBlackboardComponent()->SetValueAsObject(TargetActorKey, nullptr);
        }
    }
    else
    {
        // No one in memory: Clear everything
        GetBlackboardComponent()->ClearValue(TargetActorKey);
        GetBlackboardComponent()->ClearValue(SearchLocationKey);
    }
}

float AZombieAIController::CalculateTargetScore(AActor* PotentialTarget, const FAIStimulus& Stimulus, AActor* CurrentTarget)
{
    float DistanceSq = GetPawn()->GetSquaredDistanceTo(PotentialTarget);
    // Basic Score: Inverse distance (closer is better)
    float Score = 1000000.0f / (DistanceSq + 1.0f);

    // Visibility Bonus: Heavily prioritize players we can actually see over ghosts in memory
    if (Stimulus.WasSuccessfullySensed())
    {
        Score *= 2.0f;
    }

    // Hysteresis (Loyalty Bonus): Prevents "flickering" between two players
    if (PotentialTarget == CurrentTarget)
    {
        Score *= LoyaltyBonusMultiplier;
    }

    return Score;
}
```

---

### 2. The Behavior Tree Flow (The Execution)
Structure your Behavior Tree with a **Selector** at the root. The order from left-to-right defines the priority:

1.  **Chase Player (High Priority):**
    *   **Decorator:** Blackboard `TargetActor` is **Set**.
    *   **Task:** `MoveTo` (TargetActor) with `AcceptableRadius`.
2.  **Search Area (Mid Priority):**
    *   **Decorator:** Blackboard `SearchLocation` is **Set**.
    *   **Task:** `MoveTo` (SearchLocation).
    *   **Sequence:** Once it reaches the location, you can add a `RotateToFace` or a `Wait` task to look around.
3.  **Wander/Idle (Low Priority):**
    *   Standard wandering logic.

---

### 3. Anti-Abuse and Polish

*   **Max Age (The Automatic Cleanup):** In your `UAIPerceptionComponent` settings for the Sight sense, set **Max Age** (e.g., 5.0 seconds). If the zombie hasn't seen a player for 5 seconds, the perception system will automatically call `OnPerceptionUpdated` and remove that actor from `KnownPerceivedActors`. This naturally clears your Search state so zombies don't search forever.
*   **Forget Stale Actors:** In **Project Settings > AI System**, ensure **Forget Stale Actors** is checked. This ensures the AI memory doesn't bloat in a multiplayer match.
*   **Search Interruption:** Because the scoring logic runs on every perception update, if a new player runs in front of the zombie while it is halfway to the "Last Known Location," the `BestTarget` will update, the `TargetActor` blackboard key will be set, and the Behavior Tree will immediately abort the "Search" sequence to restart the "Chase" sequence.
*   **Navmesh Pathing:** Use `MoveTo` with **`bAllowPartialPath`** set to false. This prevents zombies from getting "stuck" trying to reach a player who jumped onto a ledge they can't path to; instead, they will simply search as close as they can get.

===============================

3. The Behavior Tree Flow
The “Best Practical Way” to implement the movement is a Priority Selector.

Selector (Root)
Sequence: Chase (High Priority)
Decorator: Blackboard TargetActor is Set.
Task: MoveTo (TargetActor). This will update every frame as the player moves.
Sequence: Search (Medium Priority)
Decorator: Blackboard SearchLocation is Set.
Task: MoveTo (SearchLocation). The zombie moves to where the player was last seen.
Task: Wait (2s) while playing a “looking around” animation.
Task: Clear Blackboard Value (SearchLocation). This prevents searching the same spot forever.
Sequence: Idle/Patrol (Low Priority)
Standard wander logic.
4. Preventing Exploits (Anti-Abuse)
To ensure players can’t easily “loop” the AI or make it search forever:

Max Age: In the AI Perception Component > Sight Config, set Max Age (e.g., 8 seconds). After 8 seconds of not seeing the player, the Perception system will eliminate that actor from its memory. This automatically clears the SearchLocation via the C++ code above.
Hysteresis Balance: If a player is being chased and another player stands 1 meter closer, the zombie shouldn’t immediately swap. The Loyalty Bonus in C++ ensures the zombie stays focused on the current victim unless the new player is significantly closer or more aggressive.
Navmesh Constraints: If a player reaches a location the zombie cannot path to (like a roof), the zombie will reach the SearchLocation (the point on the ground below the player), finish its “Search” sequence, and return to “Patrol.” This prevents the zombie from staring at a wall indefinitely.
Why this is the Best Practical Way:
Interruption: Because the BT uses a Selector, if the zombie is currently in the Search sequence and “sees” a new player, the C++ will set the TargetActor key. The BT will immediately abort the Search and jump back into the Chase sequence.
Multiplayer Ready: Scoring happens on the server, ensuring all clients see the zombie targeting the most relevant player.
Natural Transition: When a target is eliminated, the zombie will naturally move to their last location, “realize” they are gone, and then resume patrolling for new victims.