// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GenericTeamAgentInterface.h"
#include "GameFramework/Character.h"
#include "ATR_EchoRuntimeTypes.h"
#include "ATR_EchoCombatTypes.h"
#include "../Health/ATR_HealthTypes.h"
#include "ATR_ActiveEcho.generated.h"

class UATR_EchoSubsystem;
class UATR_WeaponDamageProfile;

// Fully-realized Echo actor. Pooled — spawned only near players or for scripted sequences.
// Owns only movement (CMC) and anim state. All AI lives on AATR_EchoAIController.
//
// Lifecycle:
//   Spawn → EnterPool()                           (dormant: hidden, no collision, no tick)
//   PromoteEcho: InitFromSoA()                    (teleport to SoA position, seed velocity)
//              → Controller->Possess(this)        (OnPossess → AI wakes at correct location)
//   DemoteEcho: WriteBackToSoA → EnterPool()      (actor returns to pool)
//             → Controller->UnPossess()           (OnUnPossess → AI stops, controller returns to pool)
//
// SourceIndex always mirrors the SoA row. INDEX_NONE when pooled.
// Server owns simulation. Clients drive visuals via CMC replication.
UCLASS()
class ALLTHATREMAINS_API AATR_ActiveEcho : public ACharacter, public IGenericTeamAgentInterface
{
	GENERATED_BODY()

public:
	AATR_ActiveEcho();
	
	virtual FGenericTeamId GetGenericTeamId() const override { return FGenericTeamId(TeamNumber); }
	virtual void SetGenericTeamId(const FGenericTeamId& NewId) override { TeamNumber = NewId.GetId(); }

	// --- State ---

	// SoA row this Actor was promoted from. INDEX_NONE when pooled.
	// Replicated so clients can register in their local IndexToActor and suppress ISM.
	UPROPERTY(ReplicatedUsing = OnRep_SourceIndex)
	int32 SourceIndex = INDEX_NONE;

	// Stable per-echo walk-speed multiplier (Echo|Simulation.HordeWalkSpeedVariation).
	// Set by the subsystem at promotion from the echo's EchoId so a promoted walker keeps
	// the same pace identity it had in the horde tier. Folded into every moving locomotion
	// branch in ApplyStructuralStateToMovement(). 1 = baseline speed. Server-authoritative.
	float SpeedScalar = 1.f;

	// Local cache of Sub->AnimState[SourceIndex]. Flushed to SoA on demotion.
	// Drive your AnimBlueprint from this.
	UPROPERTY(ReplicatedUsing = OnRep_AnimStateCache, BlueprintReadOnly, Category = "Echo")
	uint8 AnimStateCache = 0;

	// PRESENTATION-ONLY canonical intent, replicated so clients can drive animation/FX (chase
	// vs search vs investigate vs obstacle). This is NOT AI memory — clients never make
	// decisions from it; the server owns intent selection. Server-written each intent tick.
	UPROPERTY(ReplicatedUsing = OnRep_EchoIntent, BlueprintReadOnly, Category = "Echo")
	EATR_EchoIntent ReplicatedIntent = EATR_EchoIntent::Idle;

	// Server-only setter — updates ReplicatedIntent (replicates to clients on change).
	void SetEchoIntentForPresentation(EATR_EchoIntent NewIntent);

	// Set true by StateTree tasks that must not be interrupted (e.g., grab, death sequence).
	// RunPromotionPass skips demotion while this is true.
	// StateTree is responsible for clearing it in ExitState; EnterPool resets it as a safety net.
	UPROPERTY(BlueprintReadWrite, Category = "Echo")
	bool bBlockDemotion = false;

	// --- Combat hooks ---
	// Called by the melee StateTree task (FATR_EchoMeleeTask). BlueprintNativeEvent so designers can
	// override/extend (anim montage, attach, FX). The native implementations resolve REAL combat
	// server-side through the Echo structural health model and UATR_EchoSettings (Echo|Combat):
	// no arms = no grab, missing fingers = weak grip, no jaw = no bite.

	// Attempt to grab Target. Resolves capability → facing cone → miss roll; a whiffed grab can
	// still rake a scratch across the target. On success CurrentGrip is set (weak/strong) and the
	// task holds the grab, bites, and pulls. Server-side only — returns false elsewhere.
	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Combat")
	bool TryGrabTarget(AActor* Target);

	// Attempt a bite on Target (already grabbed, within bite range). Rolls a wound tier
	// (scratch/deep scratch/laceration — weighted by grip strength) and applies it to the
	// target's UATR_HumanHealthComponent as a contaminated Bite damage event.
	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Combat")
	bool TryBiteTarget(AActor* Target);

	// Pull Target toward this echo while grabbed: a per-second velocity drag toward the echo,
	// scaled by Strength (Echo|Combat.MeleePullStrength), MeleePullSpeed, and grip strength.
	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Combat")
	void PullTarget(AActor* Target, float Strength);

	// --- Combat state ---

	// Grip currently held on the melee target. Server-resolved; replicated so client
	// anim/FX can show weak vs strong holds. None when not grabbing.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Echo|Combat")
	EATR_EchoGripType CurrentGrip = EATR_EchoGripType::None;

	// Rolled deterministically from SoA Index in InitFromSoA; same Index → same condition
	// across promote/demote cycles. Drives grab multipliers, grip cap, and barge resistance.
	UPROPERTY(BlueprintReadOnly, Category = "Echo|Combat")
	FATR_EchoBodyCondition BodyCondition;

	// Called by the melee task whenever its grab releases (slip-away, task exit).
	void NotifyGrabReleased();

	// Called by a grabbed player (server) when they win the struggle and rip this grip loose.
	// Drops the grip and staggers the echo (reuses the barge-stagger gate) so it can't immediately
	// re-grab — the player's window to get away. Stagger length = Echo|Combat.GrabBreakStaggerSeconds.
	void OnGrabBrokenByTarget();

	// Re-derive CMC speed from the health model's cached capability flags:
	// run = full speed, walk-only = limp (LimpSpeedScale), crawl = CrawlSpeed,
	// immobile/dead = 0. Called on promotion and after surviving structural damage.
	void ApplyStructuralStateToMovement();

	// --- Shoulder barge ---
	// A fast-moving player colliding with this echo can shove it aside and break its grip.
	// Resolved server-side in NotifyHit from real physical terms:
	//   Power = (speed / BargeReferenceSpeed)            — momentum from running
	//         × (player mass / echo mass)                — weight of myself vs the echo
	//         × glancing factor (angle of attack)        — clipping a shoulder ≫ dead-center torso
	//         ÷ echo StrengthScalar                      — strong echoes hold their ground better
	// Success knocks the echo sideways out of the path, staggers it (no grabbing, grip drops),
	// and costs the player speed (more for center hits — lowering the shoulder has a price).
	virtual void NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp,
		bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit) override;

	// True while this echo is reeling from a successful barge: it cannot grab, and the melee
	// task releases any held grip. Checked by FATR_EchoMeleeTask each tick.
	bool IsBargeStaggered(float TimeSeconds) const { return TimeSeconds < BargeStaggeredUntilTime; }

	// --- Lifecycle ---

	// Put actor in dormant pool state: hidden, collision off, AI off, CMC stopped.
	// Called immediately after pool spawn and after every demotion.
	void EnterPool();

	// Seed position + velocity + animstate from SoA row, then activate all systems.
	// Called by UATR_EchoSubsystem::PromoteEcho — do not call directly.
	void InitFromSoA(const UATR_EchoSubsystem* Sub, int32 Index);

	// Flush live position, velocity, and animstate back into SoA row.
	// Called by UATR_EchoSubsystem::DemoteEcho before returning to pool.
	void WriteBackToSoA(UATR_EchoSubsystem* Sub) const;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	// --- Cosmetic Blueprint hooks (presentation ONLY) ---
	// No return values: by the time these fire, C++ has already resolved and applied the outcome.
	// Use them for montages, attaching the target hand-to-chest, blood FX, audio.

	// A grab connected. Attach/constraint visuals + grab montage go here.
	UFUNCTION(BlueprintImplementableEvent, Category = "Echo|Combat", meta = (DisplayName = "On Grab Connected"))
	void BP_OnGrabConnected(AActor* Target, EATR_EchoGripType Grip);

	// A grab attempt failed (whiff/bad angle/no arms). bScratchedTarget = the whiff still raked
	// a scratch (already applied by C++) — play swipe FX accordingly.
	UFUNCTION(BlueprintImplementableEvent, Category = "Echo|Combat", meta = (DisplayName = "On Grab Missed"))
	void BP_OnGrabMissed(AActor* Target, EATR_GrabOutcome Outcome, bool bScratchedTarget);

	// The grab ended (escape, state exit). Detach visuals here.
	UFUNCTION(BlueprintImplementableEvent, Category = "Echo|Combat", meta = (DisplayName = "On Grab Released"))
	void BP_OnGrabReleased(AActor* Target);

	// A bite resolved (including Miss). Wound severity already decided/logged by C++.
	UFUNCTION(BlueprintImplementableEvent, Category = "Echo|Combat", meta = (DisplayName = "On Bite Resolved"))
	void BP_OnBiteResolved(AActor* Target, EATR_BiteWound Wound);

	// The player barged through this echo. Outcome already applied by C++ (knockback +
	// stagger) — play stumble montage / impact FX here. Power > 1 = an emphatic hit.
	// (Parameter named BargeInstigator because AActor already declares 'Instigator'.)
	UFUNCTION(BlueprintImplementableEvent, Category = "Echo|Combat", meta = (DisplayName = "On Barged"))
	void BP_OnBarged(AActor* BargeInstigator, float Power);

	// Server time until which this echo is barge-staggered, and the last barge resolution
	// time (cooldown so sustained contact doesn't re-resolve every frame).
	float BargeStaggeredUntilTime = -1.f;
	float LastBargeTime           = -1000.f;

	virtual void BeginPlay() override;

	// Previous SourceIndex on this machine — lets OnRep unregister the old slot before registering the new one.
	int32 ClientPrevSourceIndex = INDEX_NONE;

	UFUNCTION() void OnRep_SourceIndex();

	// AnimBP / FX hooks. Empty native default — override in a Blueprint subclass
	// for event-driven anim/intent transitions instead of polling.
	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Replication")
	void OnRep_AnimStateCache();
	virtual void OnRep_AnimStateCache_Implementation();

	UFUNCTION(BlueprintNativeEvent, Category = "Echo|Replication")
	void OnRep_EchoIntent();
	virtual void OnRep_EchoIntent_Implementation();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Team")
	uint8 TeamNumber = 2;

	// --- Combat internals (server-side resolution) ---

	// Resolve one grab attempt: capability → facing cone → miss roll. May apply
	// a failed-grab scratch as a side effect (Result.bScratchedTarget).
	FATR_GrabResult ResolveGrabAttempt(AActor* Target);

	// Apply a resolved wound tier to the target's UATR_HumanHealthComponent.
	// Returns true if at least one wound was created (false = no component,
	// fully mitigated, or already dead).
	bool ApplyWoundToTarget(AActor* Target, EATR_BiteWound Tier);

	// Weighted bite-target selection (code-default table — forearms favored;
	// promote to a DataAsset if designers need per-archetype variation).
	static EATR_BodyRegion PickBiteRegion();

	// Echo|Combat.BiteDamageProfile resolved once on first bite (server only).
	UPROPERTY(Transient)
	TObjectPtr<UATR_WeaponDamageProfile> ResolvedBiteProfile;
	bool bBiteProfileResolved = false;

	// MaxWalkSpeed captured at BeginPlay (Blueprint-tuned baseline) so structural
	// speed effects (limp/crawl) always scale from the undamaged value.
	float BaseMaxWalkSpeed = 0.f;
};
