// Fill out your copyright notice in the Description page of Project Settings.


#include "ATR_Player.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Camera/CameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "../Health/ATR_HealthSettings.h"
#include "../Health/Human/ATR_HumanHealthComponent.h"
#include "../Health/Data/ATR_WeaponDamageProfile.h"
#include "../Echo/ATR_ActiveEcho.h"
#include "../Echo/ATR_EchoSubsystem.h"

// Sets default values
AATR_Player::AATR_Player()
{
	PrimaryActorTick.bCanEverTick = true;

	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = true; // FP camera reads pawn yaw; movement uses yaw too
	bUseControllerRotationRoll = false;

	// FP camera attaches in BeginPlay so we can target the head bone if it exists.
	FPCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FPCamera"));
	FPCamera->SetupAttachment(RootComponent);
	FPCamera->SetRelativeLocation(FVector(0.f, 0.f, BaseEyeHeight));
	FPCamera->bUsePawnControlRotation = true;

	// Third-person (debug) — disabled by default; ToggleCameraView swaps actives.
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 350.0f;
	SpringArm->SocketOffset = FVector(0.f, 60.f, 60.f); // over-the-shoulder
	SpringArm->bUsePawnControlRotation = true;

	TPCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("TPCamera"));
	TPCamera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	TPCamera->bUsePawnControlRotation = false;
	TPCamera->SetAutoActivate(false);

	// Full physiological simulation — replicates observable state by default.
	Health = CreateDefaultSubobject<UATR_HumanHealthComponent>(TEXT("Health"));
}

// Called when the game starts or when spawned
void AATR_Player::BeginPlay()
{
	Super::BeginPlay();
	
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (auto* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}

	// Attach FP camera to the head bone if the mesh has one — otherwise leave it on
	// the capsule (already in place from the constructor). Re-anchor each BeginPlay
	// in case the mesh was swapped in a Blueprint subclass.
	if (FPCamera && GetMesh() && !FPCameraSocket.IsNone() && GetMesh()->DoesSocketExist(FPCameraSocket))
	{
		FPCamera->AttachToComponent(GetMesh(), FAttachmentTransformRules::SnapToTargetIncludingScale, FPCameraSocket);
		FPCamera->SetRelativeLocation(FVector::ZeroVector);
	}

	// Default view: first-person.
	ApplyCameraView();

	// Death reaction — fires on the server from Die() and on clients via RepNotify.
	if (Health)
	{
		Health->OnDeath.AddDynamic(this, &AATR_Player::HandleDeath);
	}
}

void AATR_Player::ToggleCameraView()
{
	bFirstPerson = !bFirstPerson;
	ApplyCameraView();
}

void AATR_Player::ApplyCameraView()
{
	if (FPCamera) FPCamera->SetActive(bFirstPerson);
	if (TPCamera) TPCamera->SetActive(!bFirstPerson);
}

void AATR_Player::Move(const FInputActionValue& Value)
{
	const FVector2D Input = Value.Get<FVector2D>();
	if (Controller)
	{
		const FRotator YawRotation(0, Controller->GetControlRotation().Yaw, 0);
		const FVector Forward = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector Right   = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		AddMovementInput(Forward, Input.Y);
		AddMovementInput(Right,   Input.X);
	}
}

void AATR_Player::Look(const FInputActionValue& Value)
{
	const FVector2D Input = Value.Get<FVector2D>();
	if (Controller)
	{
		AddControllerYawInput(Input.X);
		AddControllerPitchInput(Input.Y);
	}
}

// Called every frame
void AATR_Player::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

// Called to bind functionality to input
void AATR_Player::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);
	
	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EIC->BindAction(JumpAction, ETriggerEvent::Started,   this, &ACharacter::Jump);
		EIC->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
		EIC->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AATR_Player::Move);
		EIC->BindAction(LookAction, ETriggerEvent::Triggered, this, &AATR_Player::Look);

		if (DebugAttackAction)
		{
			EIC->BindAction(DebugAttackAction, ETriggerEvent::Started, this, &AATR_Player::OnDebugAttack);
		}

		if (ToggleCameraAction)
		{
			EIC->BindAction(ToggleCameraAction, ETriggerEvent::Started, this, &AATR_Player::ToggleCameraView);
		}
	}

}

// ─── Debug melee ──────────────────────────────────────────────────────────────

void AATR_Player::OnDebugAttack(const FInputActionValue& /*Value*/)
{
	if (!Health || !Health->IsConscious())
	{
		return;
	}

	// Use whichever camera is active for AIM DIRECTION only; the server ignores the
	// origin we send and traces from the pawn's eye height to keep reach consistent
	// across FP/TP views and to reject spoofed origins.
	UCameraComponent* Cam = (bFirstPerson && FPCamera) ? FPCamera.Get() : TPCamera.Get();
	if (!Cam) return;

	Server_DebugMeleeAttack(Cam->GetComponentLocation(), Cam->GetForwardVector());
}

bool AATR_Player::Server_DebugMeleeAttack_Validate(FVector_NetQuantize /*Origin*/, FVector_NetQuantizeNormal Direction)
{
	return !Direction.IsNearlyZero();
}

void AATR_Player::Server_DebugMeleeAttack_Implementation(FVector_NetQuantize /*Origin*/, FVector_NetQuantizeNormal Direction)
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority())
	{
		return;
	}

	// Trace ALWAYS originates at the pawn's eye height server-side. We deliberately
	// ignore the client-sent Origin: it's TP-camera-distance-biased and spoofable.
	// Reach (DebugMeleeRange) is measured from the pawn, identical for FP and TP.
	const FVector TraceOrigin = GetActorLocation() + FVector(0.f, 0.f, BaseEyeHeight);

	const UATR_HealthSettings* HS = GetDefault<UATR_HealthSettings>();
	const FVector Dir = FVector(Direction).GetSafeNormal();
	const FVector End = TraceOrigin + Dir * HS->DebugMeleeRange;

	FCollisionQueryParams Params(FName(TEXT("ATR_DebugMelee")), /*bTraceComplex =*/ false, this);
	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, TraceOrigin, End, ECC_Pawn, Params);

#if ENABLE_DRAW_DEBUG
	if (HS->bDrawDebugMeleeTrace)
	{
		DrawDebugLine(World, TraceOrigin, bHit ? Hit.ImpactPoint : End, bHit ? FColor::Red : FColor::Silver, false, 2.f, 0, 1.f);
		if (bHit)
		{
			DrawDebugPoint(World, Hit.ImpactPoint, 12.f, FColor::Red, false, 2.f);
		}
	}
#endif

	AActor* HitActor = bHit ? Hit.GetActor() : nullptr;
	if (!HitActor)
	{
		return;
	}

	// Damage payload comes ONLY from the profile asset — there is no duplicate
	// explicit-fields fallback. No profile assigned = trace-only debug swing.
	UATR_WeaponDamageProfile* Profile = HS->DebugMeleeProfile.LoadSynchronous();
	if (!Profile)
	{
		UE_LOG(LogATR_Health, Warning, TEXT("Debug melee swing skipped: Debug|Melee.DebugMeleeProfile not assigned"));
		return;
	}

	FATR_DamageEvent Event;
	Event.Instigator    = this;
	Event.Region        = ATR_Health::RegionFromHitLocation(HitActor, Hit.ImpactPoint);
	Event.WeaponProfile = Profile;

	if (const AATR_ActiveEcho* Echo = Cast<AATR_ActiveEcho>(HitActor))
	{
		// Structural damage through the subsystem (head = brain, limbs = sever rolls).
		if (Echo->SourceIndex != INDEX_NONE)
		{
			if (UATR_EchoSubsystem* Sub = World->GetSubsystem<UATR_EchoSubsystem>())
			{
				Sub->ApplyDamageToEcho(Echo->SourceIndex, Event);
			}
		}
	}
	else if (UATR_HumanHealthComponent* VictimHealth = HitActor->FindComponentByClass<UATR_HumanHealthComponent>())
	{
		// Humans (other players / NPCs) take the same event physiologically.
		VictimHealth->ApplyDamageEvent(Event);
	}
}

void AATR_Player::HandleDeath(EATR_DeathCause Cause)
{
	UE_LOG(LogATR_Health, Log, TEXT("Player %s died: cause %d"), *GetNameSafe(this), static_cast<int32>(Cause));

	// Minimal linking-pass reaction: stop locomotion. Ragdoll/spectator flow
	// belongs to the game-mode pass.
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->StopMovementImmediately();
		CMC->DisableMovement();
	}
}

