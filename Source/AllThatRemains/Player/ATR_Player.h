// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GenericTeamAgentInterface.h"
#include "InputActionValue.h"
#include "GameFramework/Character.h"
#include "Engine/NetSerialization.h"
#include "../Health/ATR_HealthTypes.h"
#include "ATR_Player.generated.h"

class UATR_HumanHealthComponent;

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
	UFUNCTION()
	void HandleDeath(EATR_DeathCause Cause);

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
