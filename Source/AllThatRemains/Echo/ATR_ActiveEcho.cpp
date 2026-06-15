// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_ActiveEcho.h"
#include "ATR_EchoSubsystem.h"
#include "ATR_EchoSettings.h"
#include "../Health/Human/ATR_HumanHealthComponent.h"
#include "../Health/Data/ATR_WeaponDamageProfile.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "Components/CapsuleComponent.h"
#include "Logging/StructuredLog.h"

namespace
{
	// "EATR_GrabOutcome::GrabbedWeak" → "GrabbedWeak" for compact structured log fields.
	template <typename TEnum>
	FString EnumShort(TEnum Value)
	{
		FString S = UEnum::GetValueAsString(Value);
		int32 Idx = INDEX_NONE;
		S.FindLastChar(TEXT(':'), Idx);
		return Idx != INDEX_NONE ? S.Mid(Idx + 1) : S;
	}

	// Technical constants — NOT behavior tuning (those live in UATR_EchoSettings).
	constexpr int32 GBodyConditionSeedMul = 9781; // per-row deterministic RNG seed mix
	constexpr int32 GBodyConditionSeedAdd = 4243;
	constexpr float GMinStrengthScalarDiv = 0.1f; // divide-by-strength guard in barge power
}

// ─── Construction ─────────────────────────────────────────────────────────────

AATR_ActiveEcho::AATR_ActiveEcho()
{
	// Start dormant — EnterPool/InitFromSoA control actual tick state.
	PrimaryActorTick.bCanEverTick      = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates              = true;
	bUseControllerRotationYaw = false;
	AutoPossessAI = EAutoPossessAI::Disabled; // subsystem controls possession via controller pool

	// CMC — zombie defaults, tunable in Blueprint subclass
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->MaxWalkSpeed               = 150.f;
		CMC->bOrientRotationToMovement  = true;
		CMC->RotationRate               = FRotator(0.f, 360.f, 0.f);
		CMC->MaxAcceleration            = 512.f;
		CMC->BrakingDecelerationWalking = 512.f;
		CMC->bCanWalkOffLedges          = true;
		CMC->SetIsReplicated(true);
	}
}

// ─── BeginPlay ────────────────────────────────────────────────────────────────

void AATR_ActiveEcho::BeginPlay()
{
	Super::BeginPlay();

	// Capture the Blueprint-tuned baseline so limp/crawl scaling never compounds.
	if (const UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		BaseMaxWalkSpeed = CMC->MaxWalkSpeed;
	}
}

void AATR_ActiveEcho::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void AATR_ActiveEcho::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AATR_ActiveEcho, SourceIndex);
	DOREPLIFETIME(AATR_ActiveEcho, AnimStateCache);
	DOREPLIFETIME(AATR_ActiveEcho, ReplicatedIntent);
	DOREPLIFETIME(AATR_ActiveEcho, CurrentGrip);
}

void AATR_ActiveEcho::SetEchoIntentForPresentation(EATR_EchoIntent NewIntent)
{
	// Server authority only — replicates to clients on change for animation/FX.
	if (ReplicatedIntent != NewIntent)
		ReplicatedIntent = NewIntent;
}

void AATR_ActiveEcho::OnRep_EchoIntent()
{
	// AnimBP/FX can poll ReplicatedIntent directly each frame, or override this in a Blueprint
	// subclass for event-driven intent transitions. No gameplay decision is made here.
}

void AATR_ActiveEcho::OnRep_SourceIndex()
{
	auto* Sub = GetWorld() ? GetWorld()->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	if (!Sub) return;

	// Unregister old slot (covers demotion: SourceIndex flips to INDEX_NONE).
	// Use InitializeCount/IsValidIndex rather than ActiveEntities because clients
	// receive partial horde snapshots and may see a promoted actor before the
	// matching SoA index has been replicated as horde data.
	if (ClientPrevSourceIndex != INDEX_NONE
		&& ClientPrevSourceIndex >= 0
		&& ClientPrevSourceIndex < Sub->InitializeCount
		&& Sub->IndexToActor.IsValidIndex(ClientPrevSourceIndex)
		&& Sub->IndexToActor[ClientPrevSourceIndex] == this)
	{
		Sub->IndexToActor[ClientPrevSourceIndex] = nullptr;
	}

	// Register new slot (covers promotion). Route through the canonical relevancy
	// API so ActiveEntities expansion, the client mask, and coarse grid stay in
	// sync — avoids the previous inline ActiveEntities write that bypassed those.
	if (SourceIndex != INDEX_NONE
		&& SourceIndex >= 0
		&& SourceIndex < Sub->InitializeCount
		&& Sub->IndexToActor.IsValidIndex(SourceIndex))
	{
		Sub->IndexToActor[SourceIndex] = this;
		Sub->MarkEchoClientRelevant(SourceIndex);
	}

	ClientPrevSourceIndex = SourceIndex;
}

void AATR_ActiveEcho::OnRep_AnimStateCache()
{
	// AnimBP polls AnimStateCache directly each frame — no push needed here.
	// Override in Blueprint subclass if event-driven anim transitions are required.
}

void AATR_ActiveEcho::EnterPool()
{
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);

	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->StopMovementImmediately();
		CMC->SetComponentTickEnabled(false);
	}

	bBlockDemotion   = false; // safety net — StateTree (on controller) should clear this in ExitState
	SourceIndex      = INDEX_NONE;
	AnimStateCache   = 0;
	ReplicatedIntent = EATR_EchoIntent::Idle;
	CurrentGrip             = EATR_EchoGripType::None;
	BargeStaggeredUntilTime = -1.f;   // pooled echoes carry no stagger into their next life
	LastBargeTime           = -1000.f;
}

void AATR_ActiveEcho::InitFromSoA(const UATR_EchoSubsystem* Sub, int32 Index)
{
	if (!ensureAlways(Sub && Index >= 0 && Index < Sub->ActiveEntities)) return;

	// SoA stores feet/ground Z; capsule center must be offset up by half-height
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	SetActorLocation(FVector(Sub->Positions[Index]) + FVector(0.f, 0.f, HalfHeight), false, nullptr, ETeleportType::ResetPhysics);
	SetActorRotation(FRotator(0.f, Sub->Yaws[Index], 0.f));

	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		// Seed velocity so SoA integration → CMC handoff is seamless
		CMC->Velocity = FVector(Sub->Velocities[Index]);
		CMC->SetComponentTickEnabled(true);
	}

	AnimStateCache = Sub->AnimState[Index];

	// Roll body condition — deterministic per SoA row so the same horde member keeps its missing
	// fingers/arm across promote→demote→promote cycles. Distribution and strength range are
	// tuned in Project Settings > Echo|Combat (remainder after the three chances = NoArms).
	{
		const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
		const FRandomStream Rng(Index * GBodyConditionSeedMul + GBodyConditionSeedAdd);
		const float ConditionRoll = Rng.FRand();

		const float HealthyUpTo        = S->BodyHealthyChance;
		const float MissingFingersUpTo = HealthyUpTo + S->BodyMissingFingersChance;
		const float MissingHandUpTo    = MissingFingersUpTo + S->BodyMissingHandChance;

		BodyCondition.ArmCondition =
			ConditionRoll < HealthyUpTo        ? EATR_EchoArmCondition::Healthy :
			ConditionRoll < MissingFingersUpTo ? EATR_EchoArmCondition::MissingFingers :
			ConditionRoll < MissingHandUpTo    ? EATR_EchoArmCondition::MissingHand :
			                                     EATR_EchoArmCondition::NoArms;
		BodyCondition.StrengthScalar = Rng.FRandRange(S->BodyStrengthScalarMin, S->BodyStrengthScalarMax);
	}


	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	SetActorTickEnabled(true);

	// SourceIndex is set by PromoteToActive before this call on the promotion
	// path, but seed it defensively so the structural lookup below can't miss.
	SourceIndex = Index;

	// A damaged Echo promotes with its limp/crawl speed already applied.
	ApplyStructuralStateToMovement();
	// AI starts via controller OnPossess — subsystem calls Possess() after InitFromSoA().
}

void AATR_ActiveEcho::WriteBackToSoA(UATR_EchoSubsystem* Sub) const
{
	if (!ensureAlways(Sub && SourceIndex >= 0 && SourceIndex < Sub->ActiveEntities)) return;

	// Write feet Z back so SoA stays ground-relative (matches ISM assumption)
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Sub->Positions[SourceIndex]  = FVector3f(GetActorLocation()) - FVector3f(0.f, 0.f, HalfHeight);
	Sub->Yaws[SourceIndex]       = GetActorRotation().Yaw;
	Sub->AnimState[SourceIndex]  = AnimStateCache;

	if (const UCharacterMovementComponent* CMC = GetCharacterMovement())
		Sub->Velocities[SourceIndex] = FVector3f(CMC->Velocity);
}

// --- Combat hooks (server-side resolution through the Echo structural health model) ---

bool AATR_ActiveEcho::TryGrabTarget_Implementation(AActor* Target)
{
	if (!HasAuthority() || !IsValid(Target))
		return false;

	const FATR_GrabResult Result = ResolveGrabAttempt(Target);
	CurrentGrip = Result.Grip;

	if (Result.IsGrabbed())
		BP_OnGrabConnected(Target, Result.Grip);
	else
		BP_OnGrabMissed(Target, Result.Outcome, Result.bScratchedTarget);

	UE_LOGFMT(LogATR_EchoCombat, Log, "Grab Echo={Echo} Target={Target} Outcome={Outcome} Grip={Grip} Scratched={Scratched}",
		("Echo", SourceIndex), ("Target", GetNameSafe(Target)),
		("Outcome", EnumShort(Result.Outcome)), ("Grip", EnumShort(Result.Grip)),
		("Scratched", Result.bScratchedTarget));

	return Result.IsGrabbed();
}

FATR_GrabResult AATR_ActiveEcho::ResolveGrabAttempt(AActor* Target)
{
	FATR_GrabResult Result;

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	UATR_EchoSubsystem* Sub = GetWorld() ? GetWorld()->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	if (!S || !Sub || SourceIndex == INDEX_NONE)
	{
		Result.Outcome = EATR_GrabOutcome::Missed;
		return Result;
	}

	const FATR_EchoHealthModel& Model = Sub->GetHealthModel();

	// Physically incapable — no arms/hands left at all. FAIL-OPEN on unknown rows (flags 0).
	const uint16 Caps = Model.GetCapabilityFlags(SourceIndex);
	if (Caps != 0 && !(Caps & ATR_EchoCapability::CanGrabAny))
	{
		Result.Outcome = EATR_GrabOutcome::NoGrip;
		return Result;
	}

	// Facing cone — lunging at someone behind you doesn't connect.
	const FVector ToTarget = (Target->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
	const float CosHalfCone = FMath::Cos(FMath::DegreesToRadians(S->GrabFacingConeDegrees * 0.5f));
	if (FVector::DotProduct(GetActorForwardVector().GetSafeNormal2D(), ToTarget) < CosHalfCone)
	{
		Result.Outcome = EATR_GrabOutcome::BadAngle;
		return Result;
	}

	// Whiff roll — clumsy dead hands. A miss can still rake a scratch.
	if (FMath::FRand() < S->GrabMissChance)
	{
		Result.Outcome = EATR_GrabOutcome::Missed;
		if (FMath::FRand() < S->ScratchOnMissChance)
			Result.bScratchedTarget = ApplyWoundToTarget(Target, EATR_BiteWound::Scratch);
		return Result;
	}

	// Grip strength: any hand with enough fingers holds strong; otherwise weak.
	const bool bStrong = Model.CanGrabStrong(SourceIndex, /*bLeftHand =*/ true)
	                  || Model.CanGrabStrong(SourceIndex, /*bLeftHand =*/ false);
	Result.Outcome = bStrong ? EATR_GrabOutcome::GrabbedStrong : EATR_GrabOutcome::GrabbedWeak;
	Result.Grip    = bStrong ? EATR_EchoGripType::Strong : EATR_EchoGripType::Weak;
	return Result;
}

bool AATR_ActiveEcho::TryBiteTarget_Implementation(AActor* Target)
{
	if (!HasAuthority() || !IsValid(Target))
		return false;

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	UATR_EchoSubsystem* Sub = GetWorld() ? GetWorld()->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	if (!S || !Sub || SourceIndex == INDEX_NONE)
		return false;

	// No jaw (or no neck function) = no bite. The grab can still hold.
	// FAIL-OPEN on unknown rows (flags 0) — see ResolveGrabAttempt.
	const uint16 BiteCaps = Sub->GetHealthModel().GetCapabilityFlags(SourceIndex);
	if (BiteCaps != 0 && !(BiteCaps & ATR_EchoCapability::CanBite))
	{
		UE_LOGFMT(LogATR_EchoCombat, Verbose, "Bite Echo={Echo} blocked: no jaw/neck", ("Echo", SourceIndex));
		return false;
	}

	// Wound tier — a strong grip holds the victim still for a real laceration.
	const bool  bStrongGrip = CurrentGrip == EATR_EchoGripType::Strong;
	const float LacChance   = bStrongGrip ? S->BiteLacerationChanceStrongGrip : S->BiteLacerationChanceWeakGrip;
	const float DeepChance  = bStrongGrip ? S->BiteDeepScratchChanceStrongGrip : S->BiteDeepScratchChanceWeakGrip;

	const float Roll = FMath::FRand();
	const EATR_BiteWound Tier =
		(Roll < LacChance)              ? EATR_BiteWound::Laceration :
		(Roll < LacChance + DeepChance) ? EATR_BiteWound::DeepScratch :
		                                  EATR_BiteWound::Scratch;

	const bool bLanded = ApplyWoundToTarget(Target, Tier);
	BP_OnBiteResolved(Target, Tier);

	UE_LOGFMT(LogATR_EchoCombat, Log, "Bite Echo={Echo} Target={Target} Grip={Grip} Tier={Tier} Landed={Landed}",
		("Echo", SourceIndex), ("Target", GetNameSafe(Target)),
		("Grip", EnumShort(CurrentGrip)), ("Tier", EnumShort(Tier)), ("Landed", bLanded));

	return bLanded;
}

void AATR_ActiveEcho::PullTarget_Implementation(AActor* Target, float Strength)
{
	if (!HasAuthority() || !IsValid(Target) || CurrentGrip == EATR_EchoGripType::None)
		return;

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	const UWorld* World = GetWorld();
	if (!S || !World)
		return;

	const float GripScale = CurrentGrip == EATR_EchoGripType::Weak ? S->WeakGripPullScale : 1.f;
	const float PullSpeed = S->MeleePullSpeed * FMath::Max(Strength, 0.f) * GripScale;
	if (PullSpeed <= 0.f)
		return;

	const FVector Dir = (GetActorLocation() - Target->GetActorLocation()).GetSafeNormal2D();
	if (Dir.IsNearlyZero())
		return;

	const float Dt = World->GetDeltaSeconds();

	if (const ACharacter* Char = Cast<ACharacter>(Target))
	{
		if (UCharacterMovementComponent* CMC = Char->GetCharacterMovement())
		{
			CMC->AddImpulse(Dir * PullSpeed * Dt, /*bVelocityChange =*/ true);
			UE_LOGFMT(LogATR_EchoCombat, VeryVerbose, "Pull Echo={Echo} Target={Target} Speed={Speed}",
				("Echo", SourceIndex), ("Target", GetNameSafe(Target)), ("Speed", PullSpeed));
		}
		return;
	}

	if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Target->GetRootComponent()))
	{
		if (Prim->IsSimulatingPhysics())
			Prim->AddImpulse(Dir * PullSpeed * Dt, NAME_None, /*bVelChange =*/ true);
	}
}

void AATR_ActiveEcho::NotifyGrabReleased()
{
	if (CurrentGrip != EATR_EchoGripType::None)
	{
		UE_LOGFMT(LogATR_EchoCombat, Verbose, "GrabReleased Echo={Echo}", ("Echo", SourceIndex));
		CurrentGrip = EATR_EchoGripType::None;
	}
}

bool AATR_ActiveEcho::ApplyWoundToTarget(AActor* Target, const EATR_BiteWound Tier)
{
	if (Tier == EATR_BiteWound::Miss || !IsValid(Target))
	{
		return false;
	}

	UATR_HumanHealthComponent* Health = Target->FindComponentByClass<UATR_HumanHealthComponent>();
	if (!Health)
	{
		return false; // not a simulated human — nothing to wound
	}

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	if (!S)
	{
		return false;
	}

	// Tier scale: laceration is the full bite, lower tiers are partial. Knobs live
	// on the EchoSettings so designers tune tier strength without touching code.
	const float TierScale = Tier == EATR_BiteWound::Laceration  ? S->LacerationTierScale
	                      : Tier == EATR_BiteWound::DeepScratch ? S->DeepScratchTierScale
	                      :                                       S->ScratchTierScale;

	// Resolve the data-driven bite profile once (server only). Profile is the
	// SINGLE source of truth for bite damage payload (severity/penetration/
	// contamination). No profile = no damage applied (linking pass keeps the
	// system content-free without baking duplicate fallback values).
	if (!bBiteProfileResolved)
	{
		ResolvedBiteProfile  = S->BiteDamageProfile.LoadSynchronous();
		bBiteProfileResolved = true;
	}
	if (!ResolvedBiteProfile)
	{
		UE_LOGFMT(LogATR_EchoCombat, Warning, "Bite Echo={Echo} skipped: Echo|Combat.BiteDamageProfile not assigned",
			("Echo", SourceIndex));
		return false;
	}

	FATR_DamageEvent Event;
	Event.Instigator    = this;
	Event.Region        = PickBiteRegion();
	Event.WeaponProfile = ResolvedBiteProfile;
	Event.EventScale01  = TierScale;

	return Health->ApplyDamageEvent(Event).Num() > 0;
}

EATR_BodyRegion AATR_ActiveEcho::PickBiteRegion()
{
	// Code-default bite-target weighting: an echo latches onto whatever is
	// nearest while dragging someone in — forearms first, then upper arms and
	// shoulders/chest, hands, neck. Promote to a DataAsset if designers need
	// per-archetype tables.
	struct FWeighted { EATR_BodyRegion Region; float Weight; };
	static constexpr FWeighted Table[] =
	{
		{ EATR_BodyRegion::LeftForearm,   16.f },
		{ EATR_BodyRegion::RightForearm,  16.f },
		{ EATR_BodyRegion::LeftUpperArm,  12.f },
		{ EATR_BodyRegion::RightUpperArm, 12.f },
		{ EATR_BodyRegion::Chest,         12.f },
		{ EATR_BodyRegion::LeftHand,       8.f },
		{ EATR_BodyRegion::RightHand,      8.f },
		{ EATR_BodyRegion::Neck,           8.f },
		{ EATR_BodyRegion::Abdomen,        5.f },
		{ EATR_BodyRegion::Head,           3.f },
	};

	float Total = 0.f;
	for (const FWeighted& W : Table) { Total += W.Weight; }

	float Roll = FMath::FRand() * Total;
	for (const FWeighted& W : Table)
	{
		Roll -= W.Weight;
		if (Roll <= 0.f) { return W.Region; }
	}
	return EATR_BodyRegion::Chest;
}

void AATR_ActiveEcho::ApplyStructuralStateToMovement()
{
	UCharacterMovementComponent* CMC = GetCharacterMovement();
	UATR_EchoSubsystem* Sub = GetWorld() ? GetWorld()->GetSubsystem<UATR_EchoSubsystem>() : nullptr;
	if (!CMC || !Sub || SourceIndex == INDEX_NONE)
	{
		return;
	}

	if (BaseMaxWalkSpeed <= 0.f)
	{
		BaseMaxWalkSpeed = CMC->MaxWalkSpeed;
	}

	const FATR_EchoHealthModel& Model = Sub->GetHealthModel();
	const uint16 Caps = Model.GetCapabilityFlags(SourceIndex);

	// FAIL-OPEN: an undamaged row (or one whose flags aren't known on this
	// machine, Caps == 0) must behave exactly like pre-linking — restore the
	// baseline and never constrain. This also heals pool actors that last
	// hosted a crawler/immobile Echo and would otherwise keep its 0 speed.
	using namespace ATR_EchoCapability;
	if (Caps == 0 || !Model.IsRowDamaged(SourceIndex))
	{
		if (BaseMaxWalkSpeed > 0.f)
		{
			CMC->MaxWalkSpeed = BaseMaxWalkSpeed;
		}
		return;
	}

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();

	if (Caps & IsDead)
	{
		CMC->MaxWalkSpeed = 0.f;
		CMC->StopMovementImmediately();
	}
	else if (Caps & CanRun)
	{
		CMC->MaxWalkSpeed = BaseMaxWalkSpeed; // locomotion chain intact
	}
	else if (Caps & CanWalk)
	{
		CMC->MaxWalkSpeed = BaseMaxWalkSpeed * (S ? S->LimpSpeedScale : 0.6f); // one bad leg — limp
	}
	else if (Caps & CanCrawl)
	{
		CMC->MaxWalkSpeed = S ? S->CrawlSpeed : 60.f; // dragging by the arms
	}
	else
	{
		// Immobile (live brain, but no walk and no crawl). Don't StopMovementImmediately —
		// the capsule stays so the player can still push past, and AI velocity stays valid
		// for any cosmetic twitch. Just floor the speed.
		CMC->MaxWalkSpeed = S ? FMath::Max(10.f, S->CrawlSpeed * 0.25f) : 15.f;
	}
}

void AATR_ActiveEcho::NotifyHit(UPrimitiveComponent* MyComp, AActor* Other, UPrimitiveComponent* OtherComp,
	bool bSelfMoved, FVector HitLocation, FVector HitNormal, FVector NormalImpulse, const FHitResult& Hit)
{
	Super::NotifyHit(MyComp, Other, OtherComp, bSelfMoved, HitLocation, HitNormal, NormalImpulse, Hit);

	// Server-side shoulder-barge resolution against a player character running into us.
	if (!HasAuthority()) return;

	const ACharacter* OtherChar = Cast<ACharacter>(Other);
	if (!OtherChar || !OtherChar->IsPlayerControlled()) return;

	const UWorld* W = GetWorld();
	const float Now = W ? W->GetTimeSeconds() : 0.f;

	const UATR_EchoSettings* S = GetDefault<UATR_EchoSettings>();
	if (!S || (Now - LastBargeTime) < S->BargeCooldownSeconds) return;

	// 1) Speed gate — a stroll into an echo is not a barge.
	const FVector  PlayerVel = OtherChar->GetVelocity();
	const FVector2D V2D(PlayerVel.X, PlayerVel.Y);
	const float Speed = V2D.Size();
	if (Speed < S->BargeMinSpeed) return;
	const FVector2D VDir = V2D / Speed;

	// 2) Must actually be running INTO us, not brushing past.
	const FVector2D ToEcho2D(GetActorLocation().X - OtherChar->GetActorLocation().X,
	                         GetActorLocation().Y - OtherChar->GetActorLocation().Y);
	const float DistToEcho = ToEcho2D.Size();
	if (DistToEcho < 1.f) return;
	const FVector2D ToEchoDir = ToEcho2D / DistToEcho;
	if (FVector2D::DotProduct(VDir, ToEchoDir) < S->BargeMinApproachDot) return;

	// 3) ANGLE OF ATTACK — lateral offset of this echo's center from the player's movement
	//    line, normalized by the combined capsule radii. 0 = dead-center torso hit (hardest),
	//    1 = clipping the shoulder/arm at the capsule edge (easiest to power through).
	const float MyR    = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float OtherR = OtherChar->GetCapsuleComponent()->GetScaledCapsuleRadius();

	const float LateralCm  = FMath::Abs(VDir.X * ToEcho2D.Y - VDir.Y * ToEcho2D.X); // |cross| = perpendicular offset
	const float OffsetNorm = FMath::Clamp(LateralCm / FMath::Max(MyR + OtherR, 1.f), 0.f, 1.f);
	const float Glancing   = FMath::Lerp(S->BargeCenterEffectiveness, 1.f, OffsetNorm);

	// 4) WEIGHT — player mass vs echo mass (CMC mass), clamped to keep extremes sane.
	const float MyMass    = FMath::Max(GetCharacterMovement()->Mass, 1.f);
	const float OtherMass = FMath::Max(OtherChar->GetCharacterMovement()->Mass, 1.f);
	const float MassRatio = FMath::Clamp(OtherMass / MyMass, S->BargeMassRatioMin, S->BargeMassRatioMax);

	// 5) Resolve. Strong echoes (high StrengthScalar) hold their ground better.
	const float Power = (Speed / S->BargeReferenceSpeed) * MassRatio * Glancing
		/ FMath::Max(BodyCondition.StrengthScalar, GMinStrengthScalarDiv);

	LastBargeTime = Now;

	if (Power < S->BargeSuccessPowerThreshold)
	{
		UE_LOGFMT(LogATR_EchoCombat, Verbose,
			"Barge Echo={Echo} Instigator={Inst} Outcome=Held Power={Power} Speed={Speed} Offset={Offset} MassRatio={MassRatio}",
			SourceIndex, GetNameSafe(Other), Power, Speed, OffsetNorm, MassRatio);
		return; // the echo holds its ground — capsule keeps blocking
	}

	// SUCCESS — knock the echo sideways out of the player's path (with some carry-through),
	// stagger it (melee task drops the grip and can't re-grab), and bleed player speed —
	// more for a dead-center hit, almost nothing for a shoulder clip.
	const float     SideSign  = (VDir.X * ToEcho2D.Y - VDir.Y * ToEcho2D.X) >= 0.f ? 1.f : -1.f;
	const FVector2D Perp      = FVector2D(-VDir.Y, VDir.X) * SideSign; // push AWAY from the path line
	FVector2D KnockDir2D = (Perp * S->BargeKnockbackSideMix + VDir * S->BargeKnockbackForwardMix).GetSafeNormal();
	if (KnockDir2D.IsNearlyZero()) KnockDir2D = VDir;

	const float CappedPower = FMath::Min(Power, S->BargePowerCap);
	const float KnockSpeed  = S->BargeKnockbackSpeed * CappedPower;
	LaunchCharacter(FVector(KnockDir2D.X, KnockDir2D.Y, 0.f) * KnockSpeed + FVector(0.f, 0.f, S->BargeKnockbackUpSpeed),
		/*bXYOverride=*/true, /*bZOverride=*/true);

	BargeStaggeredUntilTime = Now + S->BargeStaggerSeconds * CappedPower;

	// Speed cost: lowering the shoulder through center mass costs momentum; edge clips are cheap.
	if (UCharacterMovementComponent* MutableOtherCMC = OtherChar->GetCharacterMovement())
	{
		const float Loss = S->BargePlayerSpeedLossAtCenter * (1.f - OffsetNorm);
		MutableOtherCMC->Velocity *= FMath::Clamp(1.f - Loss, 0.f, 1.f);
	}

	UE_LOGFMT(LogATR_EchoCombat, Log,
		"Barge Echo={Echo} Instigator={Inst} Outcome=KnockedAside Power={Power} Speed={Speed} Offset={Offset} Glancing={Glancing} MassRatio={MassRatio} Strength={Strength} StaggerUntil={StaggerUntil}",
		SourceIndex, GetNameSafe(Other), Power, Speed, OffsetNorm, Glancing, MassRatio,
		BodyCondition.StrengthScalar, BargeStaggeredUntilTime);

	BP_OnBarged(Other, Power);
}

