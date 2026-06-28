// Fill out your copyright notice in the Description page of Project Settings.


#include "ATR_Player.h"

#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "../Health/ATR_HealthSettings.h"
#include "../Health/Human/ATR_HumanHealthComponent.h"
#include "../Health/Data/ATR_WeaponDamageProfile.h"
#include "../Echo/ATR_ActiveEcho.h"
#include "../Echo/ATR_EchoSubsystem.h"
#include "../Echo/ATR_EchoSettings.h"
#include "UI/SATR_StatusOverlayWidget.h"
#include "UI/SATR_RespawnScreenWidget.h"
#include "NavigationSystem.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Net/UnrealNetwork.h"

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

	// Establish the realistic walk pace and cache it as the baseline that sprint and grab drag
	// both scale from (UE's 600 cm/s default is an unrealistic near-run).
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxWalkSpeed = WalkSpeed;
		BaseWalkSpeed     = WalkSpeed;
		RampedMaxSpeed    = WalkSpeed;
	}

	// First pawn for this player fixes the center of the respawn radius. Successor
	// pawns get OriginalSpawnLocation pre-set in Server_RequestRespawn (deferred
	// spawn) so this branch doesn't overwrite the threaded origin.
	if (HasAuthority() && !bSpawnOriginSet)
	{
		OriginalSpawnLocation = GetActorLocation();
		bSpawnOriginSet = true;
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
	ApplyGrabEffects(DeltaTime);

#if !UE_BUILD_SHIPPING
	// DEV overlay: a stand-in for real UMG combat UI. Build the shipping HUD as a UMG widget bound
	// to IsGrabbed() / GetGrabCount() / GetStruggleProgress01(), then turn bDebugMovementGrab off.
	if (bDebugMovementGrab && IsLocallyControlled() && GEngine)
	{
		const UCharacterMovementComponent* CMC = GetCharacterMovement();
		const float Speed   = CMC ? CMC->Velocity.Size2D() : 0.f;
		const TCHAR* Tier   = (RampedMaxSpeed >= SprintSpeed - 1.f) ? TEXT("SPRINT")
							: (RampedMaxSpeed >  JogSpeed * 0.99f)   ? TEXT("JOG") : TEXT("WALK");
		GEngine->AddOnScreenDebugMessage(1001, 0.f, FColor::Cyan,
			FString::Printf(TEXT("Speed %.0f  cap %.0f  [%s]  charge %.2f"), Speed, RampedMaxSpeed, Tier, SprintCharge));

		if (IsGrabbed())
		{
			const int32 Bars = FMath::RoundToInt(StruggleProgress01 * 20.f);
			const FString Meter = FString::ChrN(Bars, TEXT('|')) + FString::ChrN(20 - Bars, TEXT('.'));
			GEngine->AddOnScreenDebugMessage(1002, 0.f, FColor::Yellow,
				FString::Printf(TEXT("GRABBED x%d   STRUGGLE [%s] %.0f%%   — MASH Sprint!"),
					GetGrabCount(), *Meter, StruggleProgress01 * 100.f));
		}
		else
		{
			GEngine->RemoveOnScreenDebugMessage(1002);
		}
	}
#endif
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

		if (SprintAction)
		{
			EIC->BindAction(SprintAction, ETriggerEvent::Started,   this, &AATR_Player::OnSprintStarted);
			EIC->BindAction(SprintAction, ETriggerEvent::Completed, this, &AATR_Player::OnSprintCompleted);
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

// ─── Echo grab (hold + struggle) ────────────────────────────────────────────────

void AATR_Player::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Only the grabbed player needs its own grab list / struggle bar.
	DOREPLIFETIME_CONDITION(AATR_Player, ActiveGrabs, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AATR_Player, StruggleProgress01, COND_OwnerOnly);
}

void AATR_Player::RegisterGrab(AATR_ActiveEcho* Echo, EATR_EchoGripType Grip)
{
	if (!HasAuthority() || !IsValid(Echo))
		return;

	for (FATR_PlayerGrab& G : ActiveGrabs)
	{
		if (G.Echo == Echo) { G.Grip = Grip; return; } // already gripping — refresh strength
	}

	FATR_PlayerGrab New;
	New.Echo = Echo;
	New.Grip = Grip;
	ActiveGrabs.Add(New);

	UE_LOG(LogATR_Health, Log, TEXT("[%s] HOLD started by %s (grip=%d) — now held by %d"),
		*GetName(), *GetNameSafe(Echo), (int32)Grip, ActiveGrabs.Num());
}

void AATR_Player::UnregisterGrab(AATR_ActiveEcho* Echo)
{
	if (!HasAuthority())
		return;

	for (int32 i = ActiveGrabs.Num() - 1; i >= 0; --i)
	{
		if (ActiveGrabs[i].Echo == Echo)
		{
			ActiveGrabs.RemoveAt(i);
			UE_LOG(LogATR_Health, Log, TEXT("[%s] HOLD released by %s — %d remaining"),
				*GetName(), *GetNameSafe(Echo), ActiveGrabs.Num());
		}
	}
}

void AATR_Player::OnSprintStarted(const FInputActionValue& /*Value*/)
{
	// Grabbed: the sprint key is the struggle mash — never sprint while held.
	if (ActiveGrabs.Num() > 0)
	{
		if (bSprintHeld) { bSprintHeld = false; Server_SprintRelease(); }
		UE_LOG(LogATR_Health, Log, TEXT("[%s] STRUGGLE mash (grabbed by %d)"), *GetName(), ActiveGrabs.Num());
		Server_Struggle();
		return;
	}

	// Press = begin holding (jog) AND one tap toward sprint charge.
	bSprintHeld  = true;
	SprintCharge = FMath::Clamp(SprintCharge + SprintTapCharge, 0.f, 1.f);
	LastSprintInputTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	UE_LOG(LogATR_Health, Log, TEXT("[%s] SPRINT press (hold=jog, tap charge=%.2f)"), *GetName(), SprintCharge);
	Server_SprintPress();
}

void AATR_Player::OnSprintCompleted(const FInputActionValue& /*Value*/)
{
	if (bSprintHeld)
	{
		bSprintHeld = false;
		UE_LOG(LogATR_Health, Log, TEXT("[%s] SPRINT release (back to walk)"), *GetName());
		Server_SprintRelease();
	}
}

void AATR_Player::Server_SprintPress_Implementation()
{
	bSprintHeld  = true;
	SprintCharge = FMath::Clamp(SprintCharge + SprintTapCharge, 0.f, 1.f);
	LastSprintInputTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
}

void AATR_Player::Server_SprintRelease_Implementation()
{
	bSprintHeld = false;
}

void AATR_Player::Server_Struggle_Implementation()
{
	if (ActiveGrabs.Num() == 0)
		return;
	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	StruggleProgress += S ? S->StrugglePressGain : 1.f;
	UE_LOG(LogATR_Health, Log, TEXT("[%s] Struggle +%.2f -> %.2f (grips=%d)"),
		*GetName(), S ? S->StrugglePressGain : 1.f, StruggleProgress, ActiveGrabs.Num());
}

void AATR_Player::ApplyGrabEffects(float DeltaTime)
{
	UCharacterMovementComponent* CMC = GetCharacterMovement();
	if (!CMC)
		return;

	if (BaseWalkSpeed <= 0.f)
		BaseWalkSpeed = CMC->MaxWalkSpeed;

	// Server prunes dead/demoted echoes (their actors pool/destroy without unregistering).
	if (HasAuthority())
	{
		for (int32 i = ActiveGrabs.Num() - 1; i >= 0; --i)
		{
			if (!IsValid(ActiveGrabs[i].Echo))
				ActiveGrabs.RemoveAt(i);
		}
	}

	const int32 NumGrabs = ActiveGrabs.Num();
	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();

	// --- SPEED: resolve MaxWalkSpeed where movement is simulated (server + owning client).
	//   grabbed → walk baseline × per-grip drag (sprint suppressed, ramp reset);
	//   free     → walk / jog / sprint, slewed at a capped rate so top speed builds over ~SprintAccelTime.
	if (HasAuthority() || IsLocallyControlled())
	{
		if (NumGrabs > 0 && S)
		{
			const float Scale = FMath::Pow(FMath::Clamp(S->GrabHoldSpeedScale, 0.05f, 1.f), (float)NumGrabs);
			CMC->MaxWalkSpeed = BaseWalkSpeed * Scale;
			// Held makes no sense while grabbed; reset so escaping restarts the ramp from walk.
			bSprintHeld    = false;
			SprintCharge   = 0.f;
			RampedMaxSpeed = BaseWalkSpeed;
		}
		else
		{
			// Bug 1 fix: only build speed while ACTUALLY moving, so holding sprint while standing
			// still can't pre-charge the ramp and dump you into instant jog the moment you step off.
			const bool bMoving = !CMC->GetCurrentAcceleration().IsNearlyZero();

			// Bug 2 fix: sprint intent is NOT the instantaneous key state (tapping releases between
			// presses, which used to snap the target back to walk). It's "key held OR tapped within
			// the grace window", so rapid taps read as one continuous intent and charge can climb.
			const UWorld* W = GetWorld();
			const float   Now = W ? W->GetTimeSeconds() : 0.f;
			const bool bIntent = bMoving && (bSprintHeld || (Now - LastSprintInputTime) < SprintTapGraceSeconds);

			if (!bIntent)
			{
				// Not trying to run (idle, or let go): no charge build-up, fall back to a walk.
				SprintCharge = 0.f;
			}
			else
			{
				// Charge decays unless you keep tapping; it rides the target between jog and sprint.
				SprintCharge = FMath::Max(0.f, SprintCharge - SprintChargeDecayPerSecond * DeltaTime);
			}

			const float Target = bIntent
				? FMath::Lerp(JogSpeed, SprintSpeed, FMath::Clamp(SprintCharge, 0.f, 1.f))
				: BaseWalkSpeed;

			// Rate-limit the ramp: full WalkSpeed→SprintSpeed takes SprintAccelTime seconds (the
			// hard accel cap). Decelerate faster than you accelerate, as a real body does.
			const float UpRate = (SprintSpeed - BaseWalkSpeed) / FMath::Max(0.1f, SprintAccelTime);
			const float Step   = ((Target >= RampedMaxSpeed) ? UpRate : UpRate * 3.f) * DeltaTime;
			RampedMaxSpeed = (RampedMaxSpeed < Target)
				? FMath::Min(Target, RampedMaxSpeed + Step)
				: FMath::Max(Target, RampedMaxSpeed - Step);

			CMC->MaxWalkSpeed = RampedMaxSpeed;

			// Log only when crossing a tier boundary (walk → jog → sprint), not every frame.
			if (IsLocallyControlled())
			{
				const int32 Tier = (RampedMaxSpeed >= SprintSpeed - 1.f) ? 2
								  : (RampedMaxSpeed >  JogSpeed * 0.99f)   ? 1 : 0;
				if (Tier != LastSpeedTier)
				{
					static const TCHAR* TierNames[] = { TEXT("WALK"), TEXT("JOG"), TEXT("SPRINT (top)") };
					UE_LOG(LogATR_Health, Log, TEXT("[%s] Speed tier -> %s (%.0f cm/s, charge=%.2f)"),
						*GetName(), TierNames[Tier], RampedMaxSpeed, SprintCharge);
					LastSpeedTier = Tier;
				}
			}
		}

		// Directional speed limits, applied last so they clamp whatever tier was just resolved.
		const FVector Accel  = CMC->GetCurrentAcceleration();
		const FVector InDir  = Accel.GetSafeNormal2D();
		if (!InDir.IsNearlyZero())
		{
			// Sharp-turn scrub: shed speed by how far the steering input diverges from current
			// velocity, so you can plant-and-cut instead of arcing wide at sprint. Only meaningful
			// once you're actually moving fast enough to have momentum to fight.
			const FVector VelDir = CMC->Velocity.GetSafeNormal2D();
			if (CMC->Velocity.SizeSquared2D() > FMath::Square(JogSpeed * 0.5f) && !VelDir.IsNearlyZero())
			{
				const float TurnDot   = FVector::DotProduct(VelDir, InDir); // 1 aligned … -1 reversed
				const float CosStart  = FMath::Cos(FMath::DegreesToRadians(SharpTurnStartAngleDegrees));
				const float CosFull   = FMath::Cos(FMath::DegreesToRadians(FMath::Max(SharpTurnStartAngleDegrees + 1.f, SharpTurnFullAngleDegrees)));
				if (TurnDot < CosStart)
				{
					const float Sharp     = FMath::Clamp((CosStart - TurnDot) / (CosStart - CosFull), 0.f, 1.f);
					const float TurnScale = FMath::Lerp(1.f, SharpTurnMinSpeedScale, Sharp);
					CMC->MaxWalkSpeed *= TurnScale;
				}
			}

			// Backpedal cap: you can't run backward. Scale in by how rearward the input is, so
			// strafing keeps full speed and only the backward component is throttled.
			const float Fwd = FVector::DotProduct(InDir, GetActorForwardVector().GetSafeNormal2D());
			if (Fwd < 0.f)
			{
				const float BackAmt = FMath::Clamp(-Fwd, 0.f, 1.f);
				const float Cap     = FMath::Lerp(CMC->MaxWalkSpeed, MaxBackpedalSpeed, BackAmt);
				CMC->MaxWalkSpeed   = FMath::Min(CMC->MaxWalkSpeed, Cap);
			}
		}
	}

	if (NumGrabs == 0)
	{
		if (HasAuthority() && (StruggleProgress != 0.f || StruggleProgress01 != 0.f))
		{
			StruggleProgress   = 0.f;
			StruggleProgress01 = 0.f;
		}
		return;
	}

	// --- PULL: nudge the locally controlled pawn toward the grabbing echo(es). Applied as movement
	// input so it composes naturally with player input and client prediction (no server impulse).
	if (IsLocallyControlled() && S && S->GrabPullInputScale > 0.f)
	{
		FVector PullDir = FVector::ZeroVector;
		for (const FATR_PlayerGrab& G : ActiveGrabs)
		{
			if (IsValid(G.Echo))
			{
				FVector To = G.Echo->GetActorLocation() - GetActorLocation();
				To.Z = 0.f;
				PullDir += To.GetSafeNormal();
			}
		}
		if (!PullDir.IsNearlyZero())
		{
			PullDir.Normalize();
			AddMovementInput(PullDir, FMath::Clamp(S->GrabPullInputScale, 0.f, 1.f));
		}
	}

	// --- BREAK: server resolves the struggle meter. Decay forces sustained mashing; crossing the
	// next grip's threshold rips it loose and staggers that echo (its re-grab window).
	if (HasAuthority() && S)
	{
		StruggleProgress = FMath::Max(0.f, StruggleProgress - S->StruggleDecayPerSecond * DeltaTime);

		// Target the EASIEST current grip (lowest threshold) — weak grips pop first when swarmed.
		int32 NextIdx       = INDEX_NONE;
		float NextThreshold = TNumericLimits<float>::Max();
		for (int32 i = 0; i < ActiveGrabs.Num(); ++i)
		{
			const float T = (ActiveGrabs[i].Grip == EATR_EchoGripType::Strong)
				? S->StruggleBreakThresholdStrong : S->StruggleBreakThresholdWeak;
			if (T < NextThreshold) { NextThreshold = T; NextIdx = i; }
		}

		if (NextIdx != INDEX_NONE)
		{
			StruggleProgress01 = (NextThreshold > 0.f)
				? FMath::Clamp(StruggleProgress / NextThreshold, 0.f, 1.f) : 0.f;

			if (StruggleProgress >= NextThreshold)
			{
				AATR_ActiveEcho* Broken = ActiveGrabs[NextIdx].Echo;
				ActiveGrabs.RemoveAt(NextIdx);
				if (IsValid(Broken))
					Broken->OnGrabBrokenByTarget(); // drop grip + stagger so it can't instantly re-grab
				StruggleProgress = FMath::Max(0.f, StruggleProgress - NextThreshold);
				UE_LOG(LogATR_Health, Log, TEXT("[%s] BROKE hold on %s (threshold %.1f) — %d grip(s) left"),
					*GetName(), *GetNameSafe(Broken), NextThreshold, ActiveGrabs.Num());
			}
		}
	}
}

void AATR_Player::HandleDeath(EATR_DeathCause Cause)
{
	UE_LOG(LogATR_Health, Log, TEXT("Player %s died: cause %d"), *GetNameSafe(this), static_cast<int32>(Cause));

	// Fires on every machine (server directly from Die(), clients via RepNotify),
	// so the corpse looks the same everywhere. The respawn UI/flow lives on the
	// PlayerController (it also binds OnDeath); this is the cosmetic body reaction.

	// Stop locomotion and drop any echo grabs (the registry pruning in
	// ApplyGrabEffects handles the echo side once this pawn stops mattering).
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->StopMovementImmediately();
		CMC->DisableMovement();
	}

	// Stop reading input on the dead body.
	if (IsLocallyControlled())
	{
		DisableInput(Cast<APlayerController>(GetController()));
	}

	// Ragdoll: let the capsule pass through pawns, hand the mesh to physics.
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		MeshComp->SetCollisionProfileName(TEXT("Ragdoll"));
		MeshComp->SetAllBodiesSimulatePhysics(true);
		MeshComp->SetSimulatePhysics(true);
		MeshComp->WakeAllRigidBodies();
		// FP camera was head-socketed; detaching keeps it from snapping with the ragdoll.
		if (FPCamera)
		{
			FPCamera->AttachToComponent(GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
		}
	}

	// Controlling client: raise the death/respawn screen.
	if (IsLocallyControlled())
	{
		ShowRespawnScreen(Cause);
	}
}

// ─── HUD + respawn (pawn-owned, zero editor setup) ───────────────────────────────

void AATR_Player::NotifyControllerChanged()
{
	Super::NotifyControllerChanged();

	// Fires on the server (PossessedBy) and the owning client (OnRep_Controller).
	// Stand up the local HUD once we actually control this pawn; a freshly
	// possessed living body also means any leftover respawn screen is stale.
	if (IsLocallyControlled())
	{
		CreateHud();

		if (RespawnScreen.IsValid())
		{
			if (UGameViewportClient* VP = GetGameInstance() ? GetGameInstance()->GetGameViewportClient() : nullptr)
			{
				VP->RemoveViewportWidgetContent(RespawnScreen.ToSharedRef());
			}
			RespawnScreen.Reset();
		}

		if (APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			PC->SetShowMouseCursor(false);
			PC->SetInputMode(FInputModeGameOnly());
		}
	}
}

void AATR_Player::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyHud();
	Super::EndPlay(EndPlayReason);
}

void AATR_Player::CreateHud()
{
	UGameViewportClient* VP = GetGameInstance() ? GetGameInstance()->GetGameViewportClient() : nullptr;
	if (!VP)
	{
		return;
	}

	if (!StatusOverlay.IsValid())
	{
		SAssignNew(StatusOverlay, SATR_StatusOverlayWidget);
		VP->AddViewportWidgetContent(StatusOverlay.ToSharedRef(), /*ZOrder*/ 100);
	}
	StatusOverlay->SetSources(this, Health);
}

void AATR_Player::DestroyHud()
{
	if (UGameViewportClient* VP = GetGameInstance() ? GetGameInstance()->GetGameViewportClient() : nullptr)
	{
		if (StatusOverlay.IsValid()) { VP->RemoveViewportWidgetContent(StatusOverlay.ToSharedRef()); }
		if (RespawnScreen.IsValid()) { VP->RemoveViewportWidgetContent(RespawnScreen.ToSharedRef()); }
	}
	StatusOverlay.Reset();
	RespawnScreen.Reset();
}

void AATR_Player::ShowRespawnScreen(EATR_DeathCause Cause)
{
	UGameViewportClient* VP = GetGameInstance() ? GetGameInstance()->GetGameViewportClient() : nullptr;
	if (!VP)
	{
		return;
	}

	if (!RespawnScreen.IsValid())
	{
		SAssignNew(RespawnScreen, SATR_RespawnScreenWidget)
			.OnRespawnRequested(FSimpleDelegate::CreateUObject(this, &AATR_Player::Server_RequestRespawn));
		VP->AddViewportWidgetContent(RespawnScreen.ToSharedRef(), /*ZOrder*/ 200);
	}
	RespawnScreen->SetDeathCause(Cause);

	// Surface the mouse so the Respawn button is clickable.
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetShowMouseCursor(true);
		FInputModeGameAndUI Mode;
		Mode.SetWidgetToFocus(RespawnScreen);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PC->SetInputMode(Mode);
	}
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(RespawnScreen, EFocusCause::SetDirectly);
	}
}

void AATR_Player::ATR_Kill()
{
	// Exec runs on the controlling client; route to the server where the sim lives.
	Server_DebugKill();
}

void AATR_Player::Server_DebugKill_Implementation()
{
	if (Health)
	{
		Health->DebugKill();
	}
}

void AATR_Player::Server_RequestRespawn_Implementation()
{
	UWorld* World = GetWorld();
	AController* C = GetController();
	if (!World || !HasAuthority() || !C)
	{
		return;
	}

	// Scatter around the ORIGINAL spawn (threaded across pawns), not the corpse.
	const FVector Origin = bSpawnOriginSet ? OriginalSpawnLocation : GetActorLocation();

	FVector SpawnLoc = Origin;
	if (const UNavigationSystemV1* Nav = UNavigationSystemV1::GetCurrent(World))
	{
		FNavLocation Result;
		if (Nav->GetRandomReachablePointInRadius(Origin, RespawnRadius, Result))
		{
			SpawnLoc = Result.Location;
		}
	}

	// NavMesh points sit on the ground; lift to the capsule centre so we don't
	// spawn embedded in the floor.
	if (const UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		SpawnLoc.Z += Capsule->GetScaledCapsuleHalfHeight();
	}

	FRotator SpawnRot = C->GetControlRotation();
	SpawnRot.Pitch = 0.f;
	SpawnRot.Roll  = 0.f;

	const FTransform SpawnTM(SpawnRot, SpawnLoc);

	// Deferred spawn so OriginalSpawnLocation is set BEFORE the successor's
	// BeginPlay runs (otherwise it would capture the scattered point as its origin).
	AATR_Player* NewPawn = World->SpawnActorDeferred<AATR_Player>(
		GetClass(), SpawnTM, nullptr, GetInstigator(),
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

	if (!NewPawn)
	{
		UE_LOG(LogATR_Health, Warning, TEXT("[%s] Respawn failed to spawn a new pawn"), *GetName());
		return;
	}

	NewPawn->OriginalSpawnLocation = Origin;
	NewPawn->bSpawnOriginSet = true;
	NewPawn->FinishSpawning(SpawnTM);

	C->Possess(NewPawn);

	UE_LOG(LogATR_Health, Log, TEXT("[%s] Respawned at %s (radius %.0f around %s)"),
		*GetName(), *SpawnLoc.ToCompactString(), RespawnRadius, *Origin.ToCompactString());

	Destroy();
}

