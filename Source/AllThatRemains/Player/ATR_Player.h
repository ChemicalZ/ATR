// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GenericTeamAgentInterface.h"
#include "InputActionValue.h"
#include "GameFramework/Character.h"
#include "Engine/NetSerialization.h"
#include "../Health/ATR_HealthTypes.h"
#include "../Echo/ATR_EchoCombatTypes.h"
#include "ATR_Player.generated.h"

class UATR_HumanHealthComponent;
class AATR_ActiveEcho;

// One echo currently gripping the player. Server-authoritative; the array replicates to the
// owning client so the predicting machine applies the same drag/pull and HUD can show grips.
USTRUCT()
struct FATR_PlayerGrab
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<AATR_ActiveEcho> Echo = nullptr;

	UPROPERTY()
	EATR_EchoGripType Grip = EATR_EchoGripType::None;
};

UCLASS()
class ALLTHATREMAINS_API AATR_Player : public ACharacter, public IGenericTeamAgentInterface
{
	GENERATED_BODY()

public:
	// Sets default values for this character's properties
	AATR_Player();

	virtual FGenericTeamId GetGenericTeamId() const override { return FGenericTeamId(TeamNumber); }
	virtual void SetGenericTeamId(const FGenericTeamId& NewId) override { TeamNumber = NewId.GetId(); }


	// First-person camera, attached to the mesh (defaults to its head bone if the bone exists,
	// falls back to the capsule top). This is the PRIMARY view for gameplay.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UCameraComponent> FPCamera;

	// Third-person (over-the-shoulder) debug view. Toggle with ToggleCameraAction.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<class UCameraComponent> TPCamera;

	// Full human physiological simulation (wounds/vitals/survival). Server
	// simulates; observable state replicates back for HUD/anim. Echo bites and
	// scratches land here via ApplyDamageEvent.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UATR_HumanHealthComponent> Health;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<class UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<class UInputAction> MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<class UInputAction> LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<class UInputAction> JumpAction;

	// Debug melee swing (development tool): camera-forward ray, resolved on the
	// SERVER as a melee hit (Debug|Melee settings in Health & Survival).
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<class UInputAction> DebugAttackAction;

	// Toggle between first-person (default) and third-person debug view.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<class UInputAction> ToggleCameraAction;

	// Bone name on the mesh that FPCamera attaches to. Empty = attach to capsule top.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Camera")
	FName FPCameraSocket = TEXT("head");

	UFUNCTION(BlueprintCallable, Category = "Camera")
	void ToggleCameraView();

	UFUNCTION(BlueprintPure, Category = "Camera")
	bool IsFirstPerson() const { return bFirstPerson; }

	// --- Movement (sprint) + Echo grab (hold + struggle) ---
	// The sprint key doubles as the struggle input: hold/press to sprint when free, MASH it to
	// break loose when an echo has you. Bound to SprintAction.

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<class UInputAction> SprintAction;

	// Three movement tiers, reached by the sprint key:
	//   no key  → WalkSpeed   (you always start here)
	//   HOLD    → JogSpeed    (a steady jog)
	//   MASH    → ramps toward SprintSpeed (top speed is earned by tapping, not free)
	// MaxWalkSpeed never JUMPS between tiers — it slews at a capped rate (SprintAccelTime), so
	// hitting top speed takes ~3–4 s of effort just like a real body accelerating.

	// Normal walk pace. Default ≈ 1.5 m/s (3.4 mph) — a brisk human walk, deliberately above
	// the shambling horde so a calm player out-walks lone walkers. Applied to CMC at BeginPlay.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ForceUnits="cm/s"))
	float WalkSpeed = 150.f;

	// Jog pace reached by simply HOLDING sprint (no mashing). Default ≈ 3.0 m/s (6.7 mph).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ForceUnits="cm/s"))
	float JogSpeed = 300.f;

	// Top sprint pace, reachable only by sustained tapping. Default ≈ 5.75 m/s (12.9 mph) — the
	// max short-burst sprint of an average adult American male with no athletic/sports background.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ForceUnits="cm/s"))
	float SprintSpeed = 575.f;

	// Seconds for MaxWalkSpeed to slew the full WalkSpeed→SprintSpeed range. The hard physical
	// limit on acceleration: even instant max charge can't beat this curve. ~4 s ≈ a real sprint.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.1", ForceUnits="s"))
	float SprintAccelTime = 4.f;

	// Sprint charge [0..1] added per sprint-key TAP. Charge lerps the target between Jog and Sprint.
	// With the defaults, sustained mashing (~3+ taps/s) climbs to full sprint; slower taps hover at jog.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ClampMax="1.0"))
	float SprintTapCharge = 0.25f;

	// Sprint charge lost per second — stop tapping and you fall back toward a jog.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0"))
	float SprintChargeDecayPerSecond = 0.6f;

	// How long after a tap the sprint intent stays "active" without the key being held. This bridges
	// the release gaps between rapid taps so mashing doesn't flicker the target back to walk.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ForceUnits="s"))
	float SprintTapGraceSeconds = 0.35f;

	// Hard cap on speed when moving BACKWARD relative to facing — you can't run in reverse. Scales
	// in by how rearward the input is (pure strafe = unaffected; pure backpedal = full cap). Default
	// ≈ 2 m/s (4.5 mph), a brisk backpedal, well under jog/sprint.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ForceUnits="cm/s"))
	float MaxBackpedalSpeed = 200.f;

	// Sharp-turn speed scrub: you can't hold top speed through a hard cut. When the angle between
	// your VELOCITY and your steering INPUT exceeds the start angle, speed is scrubbed toward
	// MinSpeedScale, reaching full scrub at the full angle. Sheds the momentum you can't carry
	// round the corner so the turn actually tightens; speed rebuilds as you straighten out.

	// Turn angle (deg) at which scrubbing BEGINS. Gentler turns than this are unaffected.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ClampMax="180.0", ForceUnits="deg"))
	float SharpTurnStartAngleDegrees = 25.f;

	// Turn angle (deg) at/above which speed is scrubbed all the way to MinSpeedScale.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.0", ClampMax="180.0", ForceUnits="deg"))
	float SharpTurnFullAngleDegrees = 90.f;

	// Speed multiplier at the sharpest (>= full angle) turn. 0.45 = cut to 45% of current speed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement", meta=(ClampMin="0.05", ClampMax="1.0"))
	float SharpTurnMinSpeedScale = 0.45f;

	// True while at least one echo grips the player. Drives drag/pull and HUD prompts.
	UFUNCTION(BlueprintPure, Category = "Combat")
	bool IsGrabbed() const { return ActiveGrabs.Num() > 0; }

	// Number of echoes currently gripping the player (each one stacks the speed drag).
	UFUNCTION(BlueprintPure, Category = "Combat")
	int32 GetGrabCount() const { return ActiveGrabs.Num(); }

	// Normalized progress [0..1] toward ripping the next grip loose — for a HUD struggle bar.
	UFUNCTION(BlueprintPure, Category = "Combat")
	float GetStruggleProgress01() const { return StruggleProgress01; }

	// Server-side grab registry, called by FATR_EchoMeleeTask on this player's machine (server).
	void RegisterGrab(AATR_ActiveEcho* Echo, EATR_EchoGripType Grip);
	void UnregisterGrab(AATR_ActiveEcho* Echo);

protected:
	// Activate FPCamera or TPCamera based on bFirstPerson.
	void ApplyCameraView();
public:

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

	void Move(const FInputActionValue& Value);
	void Look(const FInputActionValue& Value);

	// Local input → ship the camera ray to the server. Never applies damage locally.
	void OnDebugAttack(const FInputActionValue& Value);

	// Server-authoritative debug melee resolution: trace the ray, resolve the
	// hit body region, and route damage to the Echo subsystem (structural) or
	// the victim's UATR_HumanHealthComponent (physiological).
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_DebugMeleeAttack(FVector_NetQuantize Origin, FVector_NetQuantizeNormal Direction);

	// Bound to Health->OnDeath on all machines (server directly, clients via RepNotify).
	// Ragdolls the body everywhere; on the controlling client it also raises the
	// respawn screen.
	UFUNCTION()
	void HandleDeath(EATR_DeathCause Cause);

	// --- Respawn ---
	// The HUD lives on the pawn (not a custom controller/game-mode) so it needs ZERO
	// editor setup: any BP_ATR_Player in any map gets vitals + a death/respawn screen.

	// Ask the server to respawn this player: a fresh pawn is spawned at a NavMesh
	// point within RespawnRadius of the ORIGINAL spawn, possessed, and this corpse
	// destroyed. Triggered by the respawn screen's button.
	UFUNCTION(Server, Reliable)
	void Server_RequestRespawn();

	// Radius (cm) around the original spawn point that respawn points are drawn from.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Respawn", meta = (ClampMin = "0.0", ForceUnits = "cm"))
	float RespawnRadius = 1500.f;

	// Center of the respawn scatter. Server-only: captured at first spawn, then
	// threaded onto each successor pawn so respawns stay centered on the original
	// point instead of drifting with each new corpse.
	UPROPERTY()
	FVector OriginalSpawnLocation = FVector::ZeroVector;

	UPROPERTY()
	bool bSpawnOriginSet = false;

	// Debug: instant death to verify the death → ragdoll → respawn-screen → respawn
	// flow without waiting out the realistic bleed-out + grace timers. Console: ATR_Kill
	UFUNCTION(Exec)
	void ATR_Kill();

	UFUNCTION(Server, Reliable)
	void Server_DebugKill();

protected:
	// Possession hook (server PossessedBy + owning-client OnRep): the canonical
	// place to stand up the local HUD once we know who controls this pawn.
	virtual void NotifyControllerChanged() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Local-client Slate HUD (created on the controlling machine only).
	void CreateHud();
	void DestroyHud();
	void ShowRespawnScreen(EATR_DeathCause Cause);

	TSharedPtr<class SATR_StatusOverlayWidget> StatusOverlay;
	TSharedPtr<class SATR_RespawnScreenWidget> RespawnScreen;

	// Sprint key pressed/released. Free: a press starts holding (jog) and counts as one tap toward
	// sprint charge; release stops holding. Grabbed: each press is a struggle mash instead.
	void OnSprintStarted(const FInputActionValue& Value);
	void OnSprintCompleted(const FInputActionValue& Value);

	// Mirror the sprint key to the server so the speed ramp runs on both ends. Press = hold begins
	// + one charge tap; Release = stop holding.
	UFUNCTION(Server, Reliable)
	void Server_SprintPress();

	UFUNCTION(Server, Reliable)
	void Server_SprintRelease();

	UFUNCTION(Server, Reliable)
	void Server_Struggle();

	// Per-frame movement state: resolves MaxWalkSpeed (walk / jog / sprint ramp / grab drag),
	// pull-in on the locally controlled pawn, and the server-authoritative struggle/break.
	void ApplyGrabEffects(float DeltaTime);

	// --- Sprint ramp runtime state (set on the owner + the server; not replicated — both ends run
	// the same ramp from the same key events, and movement replication reconciles any drift). ---

	// True between sprint-key press and release (holding = at least a jog).
	bool bSprintHeld = false;

	// Tap charge [0..1]; raised by taps, decays over time. Lerps the target between Jog and Sprint.
	float SprintCharge = 0.f;

	// World time of the last sprint-key press, for the tap-grace window (bridges gaps between taps).
	float LastSprintInputTime = -1000.f;

	// The slew-rate-limited MaxWalkSpeed actually applied to the CMC (the "building up" value).
	float RampedMaxSpeed = 0.f;

	// Last logged speed tier (0=walk, 1=jog, 2=sprint) so the ramp only logs on transitions.
	int32 LastSpeedTier = 0;

	// On-screen debug overlay (grab count, struggle meter, speed/tier) + verbose logging. This is
	// a DEV stand-in for real UMG combat UI, which should bind to IsGrabbed/GetGrabCount/
	// GetStruggleProgress01. Defaults on for tuning — turn off on BP_ATR_Player when done.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugMovementGrab = true;

	// Echoes currently gripping the player. Server writes; replicated to the owner only.
	UPROPERTY(Replicated)
	TArray<FATR_PlayerGrab> ActiveGrabs;

	// Normalized struggle meter for HUD (owner only). Server owns the raw accumulator below.
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Combat", meta=(AllowPrivateAccess="true"))
	float StruggleProgress01 = 0.f;

	// Server-only raw struggle points; decays each tick (Echo|Combat.StruggleDecayPerSecond).
	float StruggleProgress = 0.f;

	// Walk baseline captured at BeginPlay (= WalkSpeed) so sprint and grab drag both scale from it.
	float BaseWalkSpeed = 0.f;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Team")
	uint8 TeamNumber = 1;

	// True when FPCamera is active; false = TPCamera (third-person debug).
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	bool bFirstPerson = true;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Called to bind functionality to input
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
};
