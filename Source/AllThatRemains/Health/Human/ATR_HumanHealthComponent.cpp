// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_HumanHealthComponent.h"

#include "../ATR_HealthSettings.h"
#include "../Data/ATR_BodyRegionDefinition.h"
#include "../Data/ATR_ConditionDefinition.h"
#include "../Data/ATR_DamageTypeDefinition.h"
#include "../Data/ATR_DiseaseDefinition.h"
#include "../Data/ATR_SubstanceDefinition.h"
#include "../Data/ATR_TreatmentDefinition.h"
#include "../Data/ATR_WeaponDamageProfile.h"
#include "GameFramework/Actor.h"
#include "Misc/StringBuilder.h"

namespace
{
	// Status-marker thresholds. These shape UI/AI visibility only — they apply
	// no penalties (amendment #3) — so they live as code defaults rather than
	// settings noise. Promote to UATR_HealthSettings if designers want them.
	constexpr float MarkerPain = 0.3f;
	constexpr float MarkerFever = 0.25f;
	constexpr float MarkerShock = 0.3f;
	constexpr float MarkerHypoxiaOx = 0.5f;
	constexpr float MarkerDehydration = 0.25f;
	constexpr float MarkerStarvation = 0.2f;
	constexpr float MarkerSleepDebt = 0.7f;
	constexpr float MarkerWeakGrip = 0.5f;

	float Saturate(const float V) { return FMath::Clamp(V, 0.f, 1.f); }
}

UATR_HumanHealthComponent::UATR_HumanHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UATR_HumanHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	GetMutableDefault<UATR_HealthSettings>()->ValidateAndClamp();
	Settings = GetDefault<UATR_HealthSettings>();

	// Resolve data tables once; null is fine (code defaults take over).
	RegionDef = Settings->BodyRegionDefinition.LoadSynchronous();
	DamageTypeDef = Settings->DamageTypeDefinition.LoadSynchronous();
	ConditionDef = Settings->ConditionDefinition.LoadSynchronous();

	Blood.BloodType = BloodType;
	Survival.ImmuneStrength01 = Saturate(2.f * BaseStats.ImmuneResilience);

	RecalculateDerivedStats();
}

void UATR_HumanHealthComponent::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Server owns the simulation. (Human-state replication is a linking-pass
	// concern; see the amendments doc.)
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsAlive())
	{
		return;
	}

	// Three biological buckets — wounds/conditions are NEVER per-frame.
	FastAcc += DeltaTime;
	MediumAcc += DeltaTime;
	SlowAcc += DeltaTime;

	const float FastPeriod = 1.f / Settings->FastTickHz;
	if (FastAcc >= FastPeriod)
	{
		FastTick(FastAcc);
		FastAcc = 0.f;
	}

	const float MediumPeriod = 1.f / Settings->MediumTickHz;
	if (MediumAcc >= MediumPeriod && IsAlive())
	{
		MediumTick(MediumAcc);
		MediumAcc = 0.f;
	}

	if (SlowAcc >= Settings->SlowTickSeconds && IsAlive())
	{
		SlowTick(SlowAcc);
		SlowAcc = 0.f;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Combat pipeline
// ─────────────────────────────────────────────────────────────────────────────

TArray<int32> UATR_HumanHealthComponent::ApplyDamageEvent(const FATR_DamageEvent& Event)
{
	TArray<int32> CreatedWoundIds;

	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsAlive() || Event.Region == EATR_BodyRegion::None)
	{
		return CreatedWoundIds;
	}

	TArray<FResolvedDamage> Resolved;
	ResolveDamageComponents(Event, Resolved);

	for (const FResolvedDamage& Damage : Resolved)
	{
		// Non-mechanical damage types don't create physical wounds.
		if (Damage.Type == EATR_DamageType::Toxic)
		{
			Vitals.Toxicity01 = Saturate(Vitals.Toxicity01 + Damage.Severity);
			continue;
		}
		if (Damage.Type == EATR_DamageType::Disease)
		{
			Vitals.Infection01 = Saturate(Vitals.Infection01 + Damage.Severity * 0.3f);
			continue;
		}

		const int32 WoundId = CreateWound(Event, Damage);
		CreatedWoundIds.Add(WoundId);

		const FATR_Wound* Wound = Wounds.FindByPredicate([WoundId](const FATR_Wound& W) { return W.WoundId == WoundId; });
		ApplyImmediateEffects(Event, Damage, *Wound);

		if (!IsAlive())
		{
			break; // catastrophic trauma ended it mid-event
		}
	}

	// Ballistic exit wound: a second, cleaner wound in the same region.
	if (IsAlive() && Event.WeaponProfile && Event.WeaponProfile->ExitWoundChance01 > 0.f
		&& FMath::FRand() < Event.WeaponProfile->ExitWoundChance01)
	{
		FATR_DamageEvent ExitEvent = Event;
		ExitEvent.WeaponProfile = nullptr;
		ExitEvent.DamageType = EATR_DamageType::Ballistic;
		ExitEvent.Severity01 = 0.6f * Event.EventScale01;
		ExitEvent.Penetration01 = 0.2f;
		ExitEvent.Contamination01 = 0.1f;

		TArray<FResolvedDamage> ExitResolved;
		ResolveDamageComponents(ExitEvent, ExitResolved);
		if (ExitResolved.Num() > 0)
		{
			CreatedWoundIds.Add(CreateWound(ExitEvent, ExitResolved[0]));
		}
	}

	RecalculateDerivedStats();

	if (Settings->bLogDamageEvents)
	{
		LastDamageDebug = FString::Printf(TEXT("Region=%d Profile=%s Components=%d Wounds=%d Cause=%d"),
			static_cast<int32>(Event.Region),
			Event.WeaponProfile ? *Event.WeaponProfile->GetName() : TEXT("explicit"),
			Resolved.Num(), CreatedWoundIds.Num(), static_cast<int32>(DeathCause));
		UE_LOG(LogATR_Health, Log, TEXT("[%s] DamageEvent: %s"), *GetOwner()->GetName(), *LastDamageDebug);
	}

	return CreatedWoundIds;
}

void UATR_HumanHealthComponent::ResolveDamageComponents(const FATR_DamageEvent& Event, TArray<FResolvedDamage>& Out) const
{
	const auto Mitigate = [this, &Event](const EATR_DamageType Type, float Severity, float Penetration, float Contamination) -> FResolvedDamage
	{
		const FATR_DamageMitigation M = QueryMitigation(Event.Region, Type);
		FResolvedDamage R;
		R.Type = Type;
		R.Severity = Saturate(Severity * M.SeverityScale);
		R.Penetration = Saturate(Penetration * M.PenetrationScale);
		R.Contamination = Saturate(Contamination * M.ContaminationScale);
		return R;
	};

	if (Event.WeaponProfile)
	{
		for (const FATR_WeaponDamageComponent& C : Event.WeaponProfile->DamageComponents)
		{
			const FResolvedDamage R = Mitigate(C.DamageType, C.Severity01 * Event.EventScale01, C.Penetration01, C.Contamination01);
			if (R.Severity > KINDA_SMALL_NUMBER)
			{
				Out.Add(R);
			}
		}
	}
	else if (Event.DamageType != EATR_DamageType::None)
	{
		const FResolvedDamage R = Mitigate(Event.DamageType, Event.Severity01 * Event.EventScale01, Event.Penetration01, Event.Contamination01);
		if (R.Severity > KINDA_SMALL_NUMBER)
		{
			Out.Add(R);
		}
	}
}

int32 UATR_HumanHealthComponent::CreateWound(const FATR_DamageEvent& Event, const FResolvedDamage& Damage)
{
	const FATR_DamageTypeRow Row = GetDamageTypeRow(Damage.Type);
	const FATR_BodyRegionRow RegionRow = GetRegionRow(Event.Region);

	const float ProfileBleedScale = Event.WeaponProfile ? Event.WeaponProfile->BleedScale : 1.f;
	const float ProfilePainScale = Event.WeaponProfile ? Event.WeaponProfile->PainScale : 1.f;

	FATR_Wound W;
	W.WoundId = NextWoundId++;
	W.Region = Event.Region;
	W.DetailKind = Event.DetailKind;
	W.DetailIndex = Event.DetailIndex;
	W.DamageType = Damage.Type;
	W.Severity01 = Damage.Severity;
	W.PeakSeverity01 = Damage.Severity;
	W.Depth01 = Saturate(Damage.Penetration * Row.DepthFactor);
	W.BleedRate = Damage.Severity * Row.BleedFactor * RegionRow.BleedScale * ProfileBleedScale;
	W.PainRate = Saturate(Damage.Severity * Row.PainFactor * RegionRow.PainScale * ProfilePainScale);
	W.Contamination01 = Damage.Contamination;
	W.InfectionRisk01 = Saturate(Damage.Contamination * Row.InfectionFactor);
	W.StructuralDamage01 = Saturate(Damage.Severity * Row.StructuralFactor);
	W.Stage = EATR_WoundStage::Open;

	// Burns lose fluid through damaged skin.
	if (Row.FluidLossFactor > 0.f)
	{
		Survival.Hydration01 = Saturate(Survival.Hydration01 - Damage.Severity * Row.FluidLossFactor);
	}

	Wounds.Add(W);
	OnWoundCreated.Broadcast(W);

	if (Settings->bLogDamageEvents)
	{
		UE_LOG(LogATR_Health, Verbose, TEXT("[%s] Wound %d: type=%d region=%d sev=%.2f depth=%.2f bleed=%.4f contam=%.2f"),
			*GetOwner()->GetName(), W.WoundId, static_cast<int32>(W.DamageType), static_cast<int32>(W.Region),
			W.Severity01, W.Depth01, W.BleedRate, W.Contamination01);
	}

	return W.WoundId;
}

void UATR_HumanHealthComponent::ApplyImmediateEffects(const FATR_DamageEvent& Event, const FResolvedDamage& Damage, const FATR_Wound Wound)
{
	const FATR_DamageTypeRow Row = GetDamageTypeRow(Damage.Type);
	const FATR_BodyRegionRow RegionRow = GetRegionRow(Event.Region);

	const bool bConcussive =
		Damage.Type == EATR_DamageType::Blunt || Damage.Type == EATR_DamageType::Crush ||
		Damage.Type == EATR_DamageType::Fall || Damage.Type == EATR_DamageType::Explosion;

	// Organ transfer: penetrating depth reaches organs; concussive force
	// transfers a fraction without penetration.
	const float EffectiveDepth = FMath::Max(Wound.Depth01, bConcussive ? Damage.Severity * 0.5f : 0.f);
	const float OrganDamage = Damage.Severity * EffectiveDepth;

	if (OrganDamage > 0.f)
	{
		Vitals.BrainFunction01 = Saturate(Vitals.BrainFunction01 - OrganDamage * RegionRow.BrainDamageScale);
		Vitals.HeartFunction01 = Saturate(Vitals.HeartFunction01 - OrganDamage * RegionRow.HeartDamageScale);

		if (RegionRow.LungDamageScale > 0.f)
		{
			// One lung takes the hit.
			float& Lung = FMath::RandBool() ? Vitals.LeftLungFunction01 : Vitals.RightLungFunction01;
			Lung = Saturate(Lung - OrganDamage * RegionRow.LungDamageScale);

			// Deep chest punctures collapse the lung.
			if ((Damage.Type == EATR_DamageType::Puncture || Damage.Type == EATR_DamageType::Ballistic) && Wound.Depth01 > 0.5f)
			{
				Lung = Saturate(Lung - 0.3f);
				SetCondition(EATR_ConditionType::Pneumothorax, Wound.Depth01, Event.Region, Wound.WoundId);
			}
		}
	}

	// Fractures (blunt/crush pressure on bone).
	const float FractureChance = (Event.WeaponProfile ? Event.WeaponProfile->FractureChance01 : 0.f)
		+ Wound.StructuralDamage01 * 0.5f;
	if (Row.StructuralFactor > 0.f && FMath::FRand() < FractureChance)
	{
		SetCondition(EATR_ConditionType::Fracture, Damage.Severity, Event.Region, Wound.WoundId);
	}

	// Concussion on head impacts.
	if (Event.Region == EATR_BodyRegion::Head && bConcussive)
	{
		const float StunChance = (Event.WeaponProfile ? Event.WeaponProfile->StunChance01 : 0.f) + Damage.Severity * 0.3f;
		if (FMath::FRand() < StunChance)
		{
			SetCondition(EATR_ConditionType::Concussion, Damage.Severity, EATR_BodyRegion::Head, Wound.WoundId);
			Vitals.Consciousness01 = Saturate(Vitals.Consciousness01 - Damage.Severity * 0.5f);
		}
	}

	// Internal bleeding from torso trauma — modeled as a hidden internal
	// wound so the bleeding/clotting machinery applies (untreatable from
	// outside; only clotting and time help).
	const bool bTorso = Event.Region == EATR_BodyRegion::Chest || Event.Region == EATR_BodyRegion::Abdomen || Event.Region == EATR_BodyRegion::Pelvis;
	if (bTorso && Row.InternalBleedFactor > 0.f && FMath::FRand() < Row.InternalBleedFactor * Damage.Severity)
	{
		FATR_Wound Internal;
		Internal.WoundId = NextWoundId++;
		Internal.Region = Event.Region;
		Internal.DamageType = Damage.Type;
		Internal.Severity01 = Damage.Severity * 0.7f;
		Internal.PeakSeverity01 = Internal.Severity01;
		Internal.Depth01 = 1.f;
		// Code-default shaping: full-severity internal bleed loses ~0.002 vol/s.
		Internal.BleedRate = Damage.Severity * Row.InternalBleedFactor * 0.002f;
		Internal.PainRate = Saturate(Damage.Severity * 0.5f);
		Internal.bInternal = true;
		Wounds.Add(Internal);
		SetCondition(EATR_ConditionType::InternalBleeding, Internal.Severity01, Event.Region, Internal.WoundId);
	}

	// Dismemberment of fine detail / limbs (axes, severe trauma).
	const float DismemberChance = Event.WeaponProfile ? Event.WeaponProfile->DismemberChance01 : 0.f;
	if (DismemberChance > 0.f && FMath::FRand() < DismemberChance * Damage.Severity)
	{
		FATR_PermanentImpairment Imp;
		Imp.Region = Event.Region;
		Imp.DetailKind = Event.DetailKind;
		Imp.DetailIndex = Event.DetailIndex;
		Imp.Severity01 = 1.f;

		if (Event.DetailKind == EATR_BodyDetailKind::Finger) { Imp.Type = EATR_ImpairmentType::LostFinger; }
		else if (Event.DetailKind == EATR_BodyDetailKind::Toe) { Imp.Type = EATR_ImpairmentType::LostToe; }
		else if (Damage.Severity >= 0.9f)
		{
			switch (Event.Region)
			{
			case EATR_BodyRegion::LeftHand: case EATR_BodyRegion::RightHand: Imp.Type = EATR_ImpairmentType::LostHand; break;
			case EATR_BodyRegion::LeftFoot: case EATR_BodyRegion::RightFoot: Imp.Type = EATR_ImpairmentType::LostFoot; break;
			case EATR_BodyRegion::LeftForearm: case EATR_BodyRegion::RightForearm:
			case EATR_BodyRegion::LeftShin: case EATR_BodyRegion::RightShin: Imp.Type = EATR_ImpairmentType::LostLimb; break;
			default: break;
			}
		}

		if (Imp.Type != EATR_ImpairmentType::None)
		{
			Impairments.Add(Imp);
			UE_LOG(LogATR_Health, Log, TEXT("[%s] Permanent impairment %d at region %d"),
				*GetOwner()->GetName(), static_cast<int32>(Imp.Type), static_cast<int32>(Imp.Region));
		}
	}

	// Pain/shock spike — pain rises instantly on trauma, decays slowly later.
	Vitals.Pain01 = Saturate(Vitals.Pain01 + Wound.PainRate * (1.f - 0.5f * BaseStats.PainTolerance));
	if (Damage.Severity > 0.5f)
	{
		Vitals.Shock01 = Saturate(Vitals.Shock01 + (Damage.Severity - 0.5f) * 0.4f);
	}

	// Catastrophic trauma: single hit destroys required function outright.
	if (Damage.Severity >= RegionRow.CatastrophicSeverityThreshold)
	{
		Die(EATR_DeathCause::CatastrophicTrauma);
	}
}

FATR_DamageMitigation UATR_HumanHealthComponent::QueryMitigation(const EATR_BodyRegion Region, const EATR_DamageType Type) const
{
	// Clothing/armor hook — never hardcoded armor logic here.
	if (GetOwner() && GetOwner()->Implements<UATR_DamageMitigationProvider>())
	{
		return IATR_DamageMitigationProvider::Execute_GetMitigationForHit(GetOwner(), Region, Type);
	}
	return FATR_DamageMitigation(); // unprotected
}

// ─────────────────────────────────────────────────────────────────────────────
// Fast tick: 1-4 Hz — bleeding, pressure, oxygen, shock, consciousness, death
// ─────────────────────────────────────────────────────────────────────────────

void UATR_HumanHealthComponent::FastTick(const float Dt)
{
	// ── Bleeding + clotting ──
	float TotalBleed = 0.f;
	float WorstNeckChoke = 0.f;

	for (FATR_Wound& W : Wounds)
	{
		if (!W.IsActive())
		{
			continue;
		}

		const bool bTourniquetHolds = W.Treatment.bTourniquet && ATR_Health::IsLimbRegion(W.Region);
		if (!bTourniquetHolds)
		{
			TotalBleed += W.BleedRate;
		}

		// Natural clotting — deep wounds clot slower; tourniquet pressure helps.
		const float ClotRate = Settings->ClotRatePerSecond * (1.f - 0.7f * W.Depth01) * (bTourniquetHolds ? 1.5f : 1.f);
		W.BleedRate = FMath::Max(0.f, W.BleedRate - ClotRate * Dt);

		if (W.Stage == EATR_WoundStage::Open && W.BleedRate <= KINDA_SMALL_NUMBER)
		{
			W.Stage = EATR_WoundStage::Clotted;
		}

		// Severe neck wounds compromise the airway.
		if (W.Region == EATR_BodyRegion::Neck)
		{
			WorstNeckChoke = FMath::Max(WorstNeckChoke, W.Severity01 * W.Depth01);
		}
	}

	Vitals.BloodVolume01 = Saturate(Vitals.BloodVolume01 - TotalBleed * Dt);

	// ── Derived circulation ──
	Vitals.BloodPressure01 = Saturate(Vitals.BloodVolume01 * Vitals.HeartFunction01 * (1.f - 0.5f * Vitals.Shock01));

	// ── Oxygenation: lungs × airway × perfusion ceiling, approached at a rate ──
	const float LungCapacity = 0.5f * (Vitals.LeftLungFunction01 + Vitals.RightLungFunction01);
	const float Airway = 1.f - WorstNeckChoke;
	const float OxygenCeiling = Saturate(LungCapacity * Airway * (0.5f + 0.5f * Vitals.BloodPressure01));

	const float OxRate = Settings->OxygenRecoveryPerSecond * (Vitals.Oxygenation01 > OxygenCeiling ? 2.f : 1.f);
	Vitals.Oxygenation01 = FMath::FInterpConstantTo(Vitals.Oxygenation01, OxygenCeiling, Dt, OxRate);

	// ── Shock dynamics ──
	const float VolumeDeficit = 1.f - Vitals.BloodVolume01;
	const float PainOverload = FMath::Max(0.f, Vitals.Pain01 - BaseStats.PainTolerance);
	Vitals.Shock01 = Saturate(Vitals.Shock01
		+ (VolumeDeficit * Settings->ShockFromBloodLossRate + PainOverload * Settings->ShockFromPainRate) * Dt
		- ((TotalBleed <= KINDA_SMALL_NUMBER && PainOverload <= 0.f) ? Settings->ShockDecayPerSecond * Dt : 0.f));

	// ── Hypoxic brain damage ──
	if (Vitals.Oxygenation01 <= Settings->CollapseOxygenation)
	{
		Vitals.BrainFunction01 = Saturate(Vitals.BrainFunction01 - Settings->HypoxiaBrainDamagePerSecond * Dt);
	}

	// ── Consciousness: minimum of the things that keep a brain awake ──
	float Target = Vitals.BrainFunction01;
	Target = FMath::Min(Target, FMath::GetMappedRangeValueClamped(
		FVector2D(Settings->CollapseOxygenation, 0.6f), FVector2D(0.f, 1.f), Vitals.Oxygenation01));
	Target = FMath::Min(Target, FMath::GetMappedRangeValueClamped(
		FVector2D(Settings->CollapseBloodPressure, 0.6f), FVector2D(0.f, 1.f), Vitals.BloodPressure01));
	Target -= PainOverload * 0.5f;
	Target -= SubstanceTotals.Sedation * 0.6f;
	Target -= FMath::Max(0.f, Survival.SleepDebt01 - 0.8f) * 1.5f;

	// Temperature confusion bands.
	if (Vitals.CoreTemperatureC < Settings->HypothermiaOnsetC)
	{
		Target -= (Settings->HypothermiaOnsetC - Vitals.CoreTemperatureC)
			/ FMath::Max(Settings->HypothermiaOnsetC - Settings->FatalCoreTempLowC, 1.f);
	}
	else if (Vitals.CoreTemperatureC > Settings->HyperthermiaOnsetC)
	{
		Target -= (Vitals.CoreTemperatureC - Settings->HyperthermiaOnsetC)
			/ FMath::Max(Settings->FatalCoreTempHighC - Settings->HyperthermiaOnsetC, 1.f);
	}

	Vitals.Consciousness01 = FMath::FInterpTo(Vitals.Consciousness01, Saturate(Target), Dt, 1.f);

	// Hysteresis latch so victims don't flicker awake.
	if (!bUnconscious && Vitals.Consciousness01 <= Settings->UnconsciousThreshold)
	{
		bUnconscious = true;
		OnConsciousnessChanged.Broadcast(false);
	}
	else if (bUnconscious && Vitals.Consciousness01 >= Settings->WakeThreshold)
	{
		bUnconscious = false;
		OnConsciousnessChanged.Broadcast(true);
	}

	// ── Death checks (cause-based; sustained paths use grace timers) ──
	if (Vitals.BrainFunction01 <= Settings->FatalBrainFunction) { Die(EATR_DeathCause::BrainFailure); return; }
	if (Vitals.HeartFunction01 <= Settings->FatalHeartFunction) { Die(EATR_DeathCause::CardiacArrest); return; }
	if (Vitals.Infection01 >= Settings->FatalInfection) { Die(EATR_DeathCause::Sepsis); return; }
	if (Vitals.Toxicity01 >= Settings->FatalToxicity) { Die(EATR_DeathCause::Toxicity); return; }

	CirculatoryCollapseSeconds = Vitals.BloodPressure01 <= Settings->CollapseBloodPressure ? CirculatoryCollapseSeconds + Dt : 0.f;
	if (CirculatoryCollapseSeconds >= Settings->CirculatoryGraceSeconds) { Die(EATR_DeathCause::CirculatoryCollapse); return; }

	HypoxiaSeconds = Vitals.Oxygenation01 <= Settings->CollapseOxygenation ? HypoxiaSeconds + Dt : 0.f;
	if (HypoxiaSeconds >= Settings->HypoxiaGraceSeconds) { Die(EATR_DeathCause::Hypoxia); return; }

	const bool bTempFatalLow = Vitals.CoreTemperatureC <= Settings->FatalCoreTempLowC;
	const bool bTempFatalHigh = Vitals.CoreTemperatureC >= Settings->FatalCoreTempHighC;
	TempOutOfRangeSeconds = (bTempFatalLow || bTempFatalHigh) ? TempOutOfRangeSeconds + Dt : 0.f;
	if (TempOutOfRangeSeconds >= Settings->TemperatureGraceSeconds)
	{
		Die(bTempFatalLow ? EATR_DeathCause::Hypothermia : EATR_DeathCause::Hyperthermia);
		return;
	}

	if (Settings->bLogVitalDeltas)
	{
		UE_LOG(LogATR_Health, VeryVerbose, TEXT("[%s] Vol=%.2f BP=%.2f Ox=%.2f Brain=%.2f Con=%.2f Pain=%.2f Shock=%.2f Temp=%.1f"),
			*GetOwner()->GetName(), Vitals.BloodVolume01, Vitals.BloodPressure01, Vitals.Oxygenation01,
			Vitals.BrainFunction01, Vitals.Consciousness01, Vitals.Pain01, Vitals.Shock01, Vitals.CoreTemperatureC);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Medium tick: 0.5-1 Hz — pain, fatigue, temperature, markers, derived stats
// ─────────────────────────────────────────────────────────────────────────────

void UATR_HumanHealthComponent::MediumTick(const float Dt)
{
	// ── Aggregate pain (single source of truth — amendment #3) ──
	float PainTarget = 0.f;
	for (const FATR_Wound& W : Wounds)
	{
		if (W.IsActive())
		{
			// Splinted fractures hurt half as much to carry around.
			PainTarget += W.PainRate * (W.Treatment.bSplinted ? 0.5f : 1.f);
		}
	}
	for (const FATR_Condition& C : Conditions)
	{
		const FATR_ConditionRow Row = [this, &C]
		{
			if (ConditionDef)
			{
				if (const FATR_ConditionRow* Found = ConditionDef->FindRow(C.Type)) { return *Found; }
			}
			FATR_ConditionRow Default;
			Default.Type = C.Type;
			Default.PainPerSeverity = (C.Type == EATR_ConditionType::Fracture || C.Type == EATR_ConditionType::Pneumothorax) ? 0.4f
				: (C.Type == EATR_ConditionType::InternalBleeding ? 0.3f : 0.f);
			return Default;
		}();
		PainTarget += Row.PainPerSeverity * C.Severity01;
	}
	for (const FATR_ActiveDisease& D : Diseases)
	{
		if (D.Definition) { PainTarget += D.Definition->Pain * D.State.Severity01; }
	}
	PainTarget = Saturate(PainTarget - SubstanceTotals.PainRelief);

	if (PainTarget > Vitals.Pain01)
	{
		Vitals.Pain01 = FMath::FInterpTo(Vitals.Pain01, PainTarget, Dt, 2.f); // pain arrives fast
	}
	else
	{
		Vitals.Pain01 = FMath::Max(PainTarget, Vitals.Pain01 - Settings->PainDecayPerSecond * Dt);
	}

	// ── Fatigue & exertion ──
	const float Recovery = Settings->FatigueRecoveryPerSecond * (0.5f + 0.5f * Survival.Calories01) * (1.f - 0.5f * Survival.SleepDebt01);
	Survival.Fatigue01 = Saturate(Survival.Fatigue01 + Settings->FatigueGainPerSecond * Exertion01 * Dt - Recovery * (1.f - Exertion01) * Dt);
	Exertion01 = FMath::FInterpConstantTo(Exertion01, 0.f, Dt, 0.1f); // callers re-notify while exerting

	// ── Body temperature ──
	float EnvDiff = (Environment.AirTemperatureC + Environment.RadiantHeatC) - Settings->NeutralAirTempC;
	if (EnvDiff < 0.f)
	{
		EnvDiff *= 1.f + Settings->WetnessChillScale * Survival.Wetness01; // wet cold bites harder
	}
	EnvDiff *= (1.f - 0.8f * Environment.Insulation01) * (1.f - 0.5f * Environment.Shelter01);

	const float FeverOffset = Vitals.Fever01 * Settings->FeverMaxOffsetC;
	const float ExertionHeat = Exertion01 * 1.5f; // working muscles warm the core
	const float TargetTemp = 37.f + FeverOffset + ExertionHeat + EnvDiff * 0.25f;

	float Delta = TargetTemp - Vitals.CoreTemperatureC;
	bShivering = Vitals.CoreTemperatureC < 36.5f && Survival.Calories01 > 0.05f;
	if (bShivering && Delta < 0.f)
	{
		Delta *= 0.7f; // shivering slows the slide, burning calories (slow tick)
	}
	Vitals.CoreTemperatureC += Delta * Settings->TempDriftPerDegreePerSecond * Dt * 60.f;

	UpdateStatusMarkers();
	RecalculateDerivedStats();
}

// ─────────────────────────────────────────────────────────────────────────────
// Slow tick: biological minutes — wounds, infection, disease, substances,
// healing, survival drains, permanent outcomes
// ─────────────────────────────────────────────────────────────────────────────

void UATR_HumanHealthComponent::SlowTick(const float Dt)
{
	const float Hours = Dt / 3600.f;

	// ── Survival drains ──
	float DiseaseDehydration = 0.f;
	float DiseaseImmuneSuppression = 0.f;
	float DiseaseFever = 0.f;

	for (FATR_ActiveDisease& D : Diseases)
	{
		if (!D.Definition) { continue; }

		if (D.State.IncubationRemaining > 0.f)
		{
			D.State.IncubationRemaining = FMath::Max(0.f, D.State.IncubationRemaining - Dt);
			continue;
		}

		// Progress vs. immune fight (antibiotics slow progression).
		const float Progress = D.Definition->BaseProgressionRate * D.State.TreatmentModifier01
			* (1.f - 0.7f * SubstanceTotals.InfectionSuppression);
		D.State.Stage01 = Saturate(D.State.Stage01 + Progress * Dt);
		D.State.ProgressionRate = Progress;

		const float Fight = D.Definition->ImmuneFightRate * Survival.ImmuneStrength01;
		D.State.Severity01 = Saturate(D.State.Severity01 + Progress * Dt - Fight * Dt);

		DiseaseDehydration += D.Definition->DehydrationRate * D.State.Severity01;
		DiseaseImmuneSuppression += D.Definition->ImmuneSuppression * D.State.Severity01;
		DiseaseFever += D.Definition->Fever * D.State.Severity01;
		Vitals.Infection01 = Saturate(Vitals.Infection01 + D.Definition->SepsisPressure * D.State.Severity01 * 0.0001f * Dt);
	}

	// Cured diseases: past onset with severity beaten down to zero.
	Diseases.RemoveAll([](const FATR_ActiveDisease& D)
	{
		return D.State.IncubationRemaining <= 0.f && D.State.Stage01 > 0.1f && D.State.Severity01 <= 0.f;
	});

	const float ExertionScale = 1.f + Exertion01 + (bShivering ? 0.5f : 0.f);
	Survival.Hydration01 = Saturate(Survival.Hydration01
		- Settings->HydrationDrainPerHour * Hours * ExertionScale
		- (SubstanceTotals.HydrationDrainPerSecond + DiseaseDehydration) * Dt);
	Survival.Calories01 = Saturate(Survival.Calories01 - Settings->CalorieDrainPerHour * Hours * ExertionScale);
	Survival.SleepDebt01 = Saturate(Survival.SleepDebt01
		+ (bSleeping ? -Settings->SleepRecoveryPerHourAsleep : Settings->SleepDebtPerHourAwake) * Hours);
	Survival.Wetness01 = Saturate(Survival.Wetness01 - Settings->WetnessDryPerHour * Hours);

	// Immune strength: innate resilience modified by current state.
	Survival.ImmuneStrength01 = Saturate(2.f * BaseStats.ImmuneResilience
		* (1.f - 0.5f * Survival.SleepDebt01)
		* (0.5f + 0.5f * Survival.Calories01)
		* (0.5f + 0.5f * Survival.NutritionQuality01)
		* (1.f - Saturate(DiseaseImmuneSuppression)));

	// Total deprivation kills through the existing failure paths: hypovolemia
	// (dehydration) and cardiac decline (starvation) — no special death rules.
	if (Survival.Hydration01 <= 0.f)
	{
		Vitals.BloodVolume01 = Saturate(Vitals.BloodVolume01 - Settings->BloodRegenPerSecond * 10.f * Dt);
	}
	if (Survival.Calories01 <= 0.f)
	{
		Survival.ProteinRepairReserve01 = Saturate(Survival.ProteinRepairReserve01 - 0.01f * Hours);
		if (Survival.ProteinRepairReserve01 <= 0.f)
		{
			Vitals.HeartFunction01 = Saturate(Vitals.HeartFunction01 - 0.005f * Hours);
		}
	}

	// Blood regeneration when fed and hydrated.
	if (Survival.Hydration01 > 0.3f && Survival.Calories01 > 0.2f)
	{
		Vitals.BloodVolume01 = Saturate(Vitals.BloodVolume01 + Settings->BloodRegenPerSecond * Survival.Hydration01 * Dt);
	}

	// ── Healing factor shared by wounds and organs ──
	const float HealFactor =
		(0.3f + 0.7f * Survival.Hydration01) *
		(0.3f + 0.7f * Survival.Calories01) *
		(0.5f + 0.5f * Survival.ProteinRepairReserve01) *
		(0.5f + 0.5f * Survival.NutritionQuality01) *
		(1.f - 0.5f * Survival.SleepDebt01) *
		(FMath::IsWithin(Vitals.CoreTemperatureC, Settings->HypothermiaOnsetC, Settings->HyperthermiaOnsetC) ? 1.f : 0.6f) *
		Vitals.BloodVolume01;

	// ── Wound lifecycle ──
	float InfectedWoundLoad = 0.f;

	for (FATR_Wound& W : Wounds)
	{
		if (!W.IsActive()) { continue; }

		W.AgeSeconds += Dt;

		// Tourniquet tissue damage past the safe window.
		if (W.Treatment.bTourniquet)
		{
			W.Treatment.TourniquetSeconds += Dt;
			if (W.Treatment.TourniquetSeconds > Settings->TourniquetSafeSeconds)
			{
				W.StructuralDamage01 = Saturate(W.StructuralDamage01 + Settings->TourniquetTissueDamageRate * Dt);
			}
		}

		// Dressings get dirty; dirty dressings contaminate.
		// Code-default decay: a dressing stays useful ~2 hours.
		if (W.Treatment.bBandaged)
		{
			W.Treatment.Cleanliness01 = Saturate(W.Treatment.Cleanliness01 - Dt / 7200.f);
			if (W.Treatment.Cleanliness01 < 0.3f && !W.bInternal)
			{
				W.Contamination01 = Saturate(W.Contamination01 + 0.0001f * Dt);
			}
		}

		// Infection pressure: contamination feeds risk, immune system (helped
		// by antibiotics) fights it. Internal wounds are effectively sterile.
		if (!W.bInternal)
		{
			const float Fight = Survival.ImmuneStrength01 * Settings->ImmuneFightRate * (1.f + 3.f * SubstanceTotals.InfectionSuppression);
			W.InfectionRisk01 = Saturate(W.InfectionRisk01 + W.Contamination01 * Settings->InfectionRiskGrowthRate * Dt - Fight * Dt);
		}

		// Stage machine (Open→Clotted handled on the fast tick by clotting).
		switch (W.Stage)
		{
		case EATR_WoundStage::Clotted:
			if (W.InfectionRisk01 > 0.6f) { W.Stage = EATR_WoundStage::Inflamed; }
			else if (W.AgeSeconds > 600.f) { W.Stage = EATR_WoundStage::Healing; }
			break;

		case EATR_WoundStage::Inflamed:
			if (W.InfectionRisk01 > 0.85f)
			{
				W.Stage = EATR_WoundStage::Infected;
				SetCondition(EATR_ConditionType::Infection, W.InfectionRisk01, W.Region, W.WoundId);
			}
			else if (W.InfectionRisk01 < 0.4f) { W.Stage = EATR_WoundStage::Healing; }
			break;

		case EATR_WoundStage::Infected:
			InfectedWoundLoad += W.Severity01;
			W.Severity01 = Saturate(W.Severity01 + 0.00005f * Dt); // festering
			if (W.InfectionRisk01 < 0.5f)
			{
				W.Stage = EATR_WoundStage::Inflamed;
				RemoveCondition(EATR_ConditionType::Infection, W.Region);
			}
			break;

		case EATR_WoundStage::Healing:
		{
			if (W.InfectionRisk01 > 0.6f) { W.Stage = EATR_WoundStage::Inflamed; break; }

			const float TreatmentMult = (W.Treatment.bSutured ? 1.5f : 1.f) * (1.f + 0.5f * W.Treatment.TreatmentQuality01);
			const float Healed = Settings->BaseHealRatePerSecond * HealFactor * TreatmentMult * Dt;
			W.Severity01 -= Healed;
			Survival.ProteinRepairReserve01 = Saturate(Survival.ProteinRepairReserve01 - Healed * 0.5f);

			if (W.Severity01 <= 0.f)
			{
				W.Severity01 = 0.f;

				// Permanent outcomes of severe, untreated structural damage.
				if (W.StructuralDamage01 >= Settings->ImpairmentStructuralThreshold && !W.Treatment.bSplinted && !W.Treatment.bSutured)
				{
					FATR_PermanentImpairment Imp;
					Imp.Region = W.Region;
					Imp.DetailKind = W.DetailKind;
					Imp.DetailIndex = W.DetailIndex;
					Imp.Severity01 = W.StructuralDamage01;
					switch (W.Region)
					{
					case EATR_BodyRegion::LeftHand: case EATR_BodyRegion::RightHand:
					case EATR_BodyRegion::LeftForearm: case EATR_BodyRegion::RightForearm:
					case EATR_BodyRegion::LeftUpperArm: case EATR_BodyRegion::RightUpperArm:
						Imp.Type = EATR_ImpairmentType::ReducedGrip; break;
					case EATR_BodyRegion::LeftThigh: case EATR_BodyRegion::RightThigh:
					case EATR_BodyRegion::LeftShin: case EATR_BodyRegion::RightShin:
					case EATR_BodyRegion::LeftFoot: case EATR_BodyRegion::RightFoot:
						Imp.Type = EATR_ImpairmentType::ReducedMobility; break;
					case EATR_BodyRegion::Chest:
						Imp.Type = EATR_ImpairmentType::ReducedLungCapacity; break;
					default:
						Imp.Type = EATR_ImpairmentType::NerveDamage; break;
					}
					Impairments.Add(Imp);
					W.Stage = EATR_WoundStage::Chronic; // aches forever
					W.PainRate = 0.1f * Imp.Severity01;
				}
				else
				{
					W.Stage = W.PeakSeverity01 >= Settings->ScarSeverityThreshold ? EATR_WoundStage::Scarred : EATR_WoundStage::Resolved;
				}

				// Conditions anchored to this wound resolve with it.
				RemoveCondition(EATR_ConditionType::Fracture, W.Region);
				RemoveCondition(EATR_ConditionType::Pneumothorax, W.Region);
				RemoveCondition(EATR_ConditionType::InternalBleeding, W.Region);

				if (Settings->bLogConditionChanges)
				{
					UE_LOG(LogATR_Health, Log, TEXT("[%s] Wound %d closed (stage %d)"),
						*GetOwner()->GetName(), W.WoundId, static_cast<int32>(W.Stage));
				}
			}
			break;
		}

		default:
			break;
		}
	}

	// ── Systemic infection & fever ──
	const float SystemicFight = Survival.ImmuneStrength01 * Settings->ImmuneFightRate * (1.f + 3.f * SubstanceTotals.InfectionSuppression);
	Vitals.Infection01 = Saturate(Vitals.Infection01 + InfectedWoundLoad * 0.0002f * Dt - SystemicFight * Dt);

	if (Vitals.Infection01 >= Settings->SepsisThreshold)
	{
		SetCondition(EATR_ConditionType::Sepsis, Vitals.Infection01);
	}
	else
	{
		RemoveCondition(EATR_ConditionType::Sepsis);
	}

	Vitals.Fever01 = Saturate(Vitals.Infection01 * 0.8f + DiseaseFever
		+ Blood.TransfusionReactionSeverity01 * 0.6f - SubstanceTotals.FeverReduction);

	// ── Substances ──
	for (FATR_ActiveSubstance& S : Substances)
	{
		S.State.TimeActiveSeconds += Dt;
		S.State.Severity01 = S.State.EvaluateStrength();
	}
	Substances.RemoveAll([](const FATR_ActiveSubstance& S) { return S.State.IsExpired(); });
	RecalculateSubstanceTotals();

	// ── Transfusion reaction & toxin clearance ──
	if (Blood.TransfusionReactionSeverity01 > 0.f)
	{
		Vitals.Toxicity01 = Saturate(Vitals.Toxicity01 + Blood.TransfusionReactionSeverity01 * 0.00005f * Dt);
		Blood.TransfusionReactionSeverity01 = Saturate(Blood.TransfusionReactionSeverity01 - 0.0001f * Dt);
	}
	Blood.RecentTransfusionVolume = FMath::Max(0.f, Blood.RecentTransfusionVolume - 0.0001f * Dt);
	Vitals.Toxicity01 = Saturate(Vitals.Toxicity01 - 0.00002f * (0.5f + 0.5f * Survival.Hydration01) * Dt);

	// ── Organ recovery (brain does NOT regenerate — first pass rule) ──
	const float OrganHeal = Settings->BaseHealRatePerSecond * HealFactor * 0.5f * Dt;
	Vitals.HeartFunction01 = Saturate(Vitals.HeartFunction01 + OrganHeal);

	float LungCap = 1.f;
	for (const FATR_PermanentImpairment& Imp : Impairments)
	{
		if (Imp.Type == EATR_ImpairmentType::ReducedLungCapacity)
		{
			LungCap = FMath::Min(LungCap, 1.f - 0.4f * Imp.Severity01);
		}
	}
	Vitals.LeftLungFunction01 = FMath::Min(LungCap, Vitals.LeftLungFunction01 + OrganHeal);
	Vitals.RightLungFunction01 = FMath::Min(LungCap, Vitals.RightLungFunction01 + OrganHeal);

	// ── Timed conditions (RemainingDuration < 0 means indefinite) ──
	for (FATR_Condition& C : Conditions)
	{
		C.Severity01 = Saturate(C.Severity01 + C.ProgressionRate * Dt);
		if (C.RemainingDuration > 0.f)
		{
			C.RemainingDuration = FMath::Max(0.f, C.RemainingDuration - Dt);
		}
	}
	Conditions.RemoveAll([this](const FATR_Condition& C)
	{
		const bool bExpired = C.RemainingDuration == 0.f;
		if (bExpired)
		{
			OnConditionChanged.Broadcast(C.Type, 0.f, false);
		}
		return bExpired;
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Treatment / medical API
// ─────────────────────────────────────────────────────────────────────────────

bool UATR_HumanHealthComponent::ApplyTreatmentToWound(const int32 WoundId, const UATR_TreatmentDefinition* Treatment, const float ApplierSkill01)
{
	if (!Treatment || !GetOwner() || !GetOwner()->HasAuthority())
	{
		return false;
	}

	FATR_Wound* W = Wounds.FindByPredicate([WoundId](const FATR_Wound& X) { return X.WoundId == WoundId; });
	if (!W || !W->IsActive() || W->bInternal)
	{
		return false; // internal bleeding can't be reached from outside
	}

	const float Quality = Saturate(Treatment->BaseQuality01 * (0.5f + 0.5f * Saturate(ApplierSkill01)));

	switch (Treatment->Type)
	{
	case EATR_TreatmentType::Bandage:
		W->Treatment.bBandaged = true;
		W->Treatment.Cleanliness01 = 1.f;
		W->BleedRate *= 1.f - Treatment->BleedReduction01 * Quality;
		break;

	case EATR_TreatmentType::Tourniquet:
		if (!ATR_Health::IsLimbRegion(W->Region)) { return false; }
		W->Treatment.bTourniquet = true;
		W->Treatment.TourniquetSeconds = 0.f;
		break;

	case EATR_TreatmentType::Disinfectant:
		W->Treatment.bDisinfected = true;
		W->Contamination01 *= 1.f - Treatment->ContaminationReduction01 * Quality;
		W->InfectionRisk01 *= 1.f - Treatment->ContaminationReduction01 * Quality;
		break;

	case EATR_TreatmentType::Suture:
		if (W->Stage == EATR_WoundStage::Infected) { return false; } // never close an infected wound
		W->Treatment.bSutured = true;
		W->BleedRate *= 1.f - 0.8f * Quality;
		break;

	case EATR_TreatmentType::Splint:
		W->Treatment.bSplinted = true;
		break;
	}

	W->Treatment.TreatmentQuality01 = FMath::Max(W->Treatment.TreatmentQuality01, Quality);

	UE_LOG(LogATR_Health, Log, TEXT("[%s] Treatment %d (q=%.2f) applied to wound %d"),
		*GetOwner()->GetName(), static_cast<int32>(Treatment->Type), Quality, WoundId);

	RecalculateDerivedStats();
	return true;
}

bool UATR_HumanHealthComponent::RemoveTourniquet(const int32 WoundId)
{
	FATR_Wound* W = Wounds.FindByPredicate([WoundId](const FATR_Wound& X) { return X.WoundId == WoundId; });
	if (!W || !W->Treatment.bTourniquet)
	{
		return false;
	}
	// Bleeding resumes at whatever rate clotting has brought it down to.
	W->Treatment.bTourniquet = false;
	W->Treatment.TourniquetSeconds = 0.f;
	return true;
}

void UATR_HumanHealthComponent::AdministerSubstance(const UATR_SubstanceDefinition* Substance, const float Dose)
{
	if (!Substance || !GetOwner() || !GetOwner()->HasAuthority() || !IsAlive())
	{
		return;
	}

	FATR_ActiveSubstance Active;
	Active.Definition = Substance;
	Active.State.SubstanceId = Substance->SubstanceId;
	Active.State.Dose = FMath::Max(Dose, 0.f);
	Active.State.OnsetTime = Substance->OnsetTime;
	Active.State.PeakTime = Substance->PeakTime;
	Active.State.Duration = Substance->Duration;
	// Dehydration slows metabolism (drugs linger in a dry body).
	Active.State.MetabolismRate = 0.5f + 0.5f * Survival.Hydration01;
	Substances.Add(Active);

	// Overdose path: every dose adds toxin load immediately.
	Vitals.Toxicity01 = Saturate(Vitals.Toxicity01 + Substance->ToxicityPerDose * Dose);

	RecalculateSubstanceTotals();

	UE_LOG(LogATR_Health, Log, TEXT("[%s] Administered %s x%.1f (toxicity now %.2f)"),
		*GetOwner()->GetName(), *Substance->SubstanceId.ToString(), Dose, Vitals.Toxicity01);
}

void UATR_HumanHealthComponent::ApplyTransfusion(const EATR_BloodType DonorType, const float Volume01)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !IsAlive() || Volume01 <= 0.f)
	{
		return;
	}

	Blood.RecentTransfusionVolume += Volume01;

	if (ATR_Health::IsBloodCompatible(DonorType, Blood.BloodType))
	{
		Vitals.BloodVolume01 = Saturate(Vitals.BloodVolume01 + Volume01);
		UE_LOG(LogATR_Health, Log, TEXT("[%s] Transfusion +%.2f (compatible)"), *GetOwner()->GetName(), Volume01);
	}
	else
	{
		// Hemolytic reaction: fever, shock, organ stress — possibly death via
		// the toxicity/shock paths. Volume still enters, but it is hostile.
		Vitals.BloodVolume01 = Saturate(Vitals.BloodVolume01 + Volume01 * 0.5f);
		Blood.TransfusionReactionSeverity01 = Saturate(Blood.TransfusionReactionSeverity01 + 0.4f + 0.4f * Volume01);
		Vitals.Shock01 = Saturate(Vitals.Shock01 + 0.3f);
		UE_LOG(LogATR_Health, Warning, TEXT("[%s] INCOMPATIBLE transfusion (%.2f) — reaction %.2f"),
			*GetOwner()->GetName(), Volume01, Blood.TransfusionReactionSeverity01);
	}
}

void UATR_HumanHealthComponent::ContractDisease(const UATR_DiseaseDefinition* Disease)
{
	if (!Disease || !GetOwner() || !GetOwner()->HasAuthority() || !IsAlive())
	{
		return;
	}
	if (Diseases.ContainsByPredicate([Disease](const FATR_ActiveDisease& D) { return D.State.DiseaseId == Disease->DiseaseId; }))
	{
		return; // already fighting it
	}

	FATR_ActiveDisease Active;
	Active.Definition = Disease;
	Active.State.DiseaseId = Disease->DiseaseId;
	Active.State.IncubationRemaining = Disease->IncubationSeconds;
	Active.State.TreatmentModifier01 = 1.f;
	Diseases.Add(Active);

	UE_LOG(LogATR_Health, Log, TEXT("[%s] Contracted disease %s (incubating %.0fs)"),
		*GetOwner()->GetName(), *Disease->DiseaseId.ToString(), Disease->IncubationSeconds);
}

void UATR_HumanHealthComponent::Consume(const float Hydration01, const float Calories01, const float Protein01, const float Quality01)
{
	Survival.Hydration01 = Saturate(Survival.Hydration01 + Hydration01);
	Survival.Calories01 = Saturate(Survival.Calories01 + Calories01);
	Survival.ProteinRepairReserve01 = Saturate(Survival.ProteinRepairReserve01 + Protein01);

	// Quality is a trend: meals pull the average toward their quality.
	const float Weight = Saturate(Calories01 * 2.f);
	Survival.NutritionQuality01 = FMath::Lerp(Survival.NutritionQuality01, Saturate(Quality01), Weight);
}

void UATR_HumanHealthComponent::SetSleeping(const bool bAsleep) { bSleeping = bAsleep; }
void UATR_HumanHealthComponent::SetEnvironment(const FATR_EnvironmentState& NewEnvironment) { Environment = NewEnvironment; }
void UATR_HumanHealthComponent::AddWetness(const float Amount01) { Survival.Wetness01 = Saturate(Survival.Wetness01 + Amount01); }
void UATR_HumanHealthComponent::NotifyExertion(const float Intensity01) { Exertion01 = FMath::Max(Exertion01, Saturate(Intensity01)); }

// ─────────────────────────────────────────────────────────────────────────────
// Conditions
// ─────────────────────────────────────────────────────────────────────────────

FATR_Condition* UATR_HumanHealthComponent::FindCondition(const EATR_ConditionType Type, const EATR_BodyRegion Region)
{
	return Conditions.FindByPredicate([Type, Region](const FATR_Condition& C)
	{
		return C.Type == Type && (Region == EATR_BodyRegion::None || C.Region == Region);
	});
}

bool UATR_HumanHealthComponent::HasCondition(const EATR_ConditionType Type) const
{
	return Conditions.ContainsByPredicate([Type](const FATR_Condition& C) { return C.Type == Type; });
}

void UATR_HumanHealthComponent::SetCondition(const EATR_ConditionType Type, const float Severity01, const EATR_BodyRegion Region,
                                             const int32 SourceWoundId, const float ProgressionRate, const float Duration)
{
	if (FATR_Condition* Existing = FindCondition(Type, Region))
	{
		Existing->Severity01 = FMath::Max(Existing->Severity01, Saturate(Severity01));
		return;
	}

	FATR_Condition C;
	C.Type = Type;
	C.Region = Region;
	C.Severity01 = Saturate(Severity01);
	C.ProgressionRate = ProgressionRate;
	C.RemainingDuration = Duration;
	C.SourceWoundId = SourceWoundId;
	Conditions.Add(C);

	OnConditionChanged.Broadcast(Type, C.Severity01, true);

	if (Settings->bLogConditionChanges)
	{
		UE_LOG(LogATR_Health, Log, TEXT("[%s] +Condition %d (sev %.2f, region %d)"),
			*GetOwner()->GetName(), static_cast<int32>(Type), C.Severity01, static_cast<int32>(Region));
	}
}

void UATR_HumanHealthComponent::RemoveCondition(const EATR_ConditionType Type, const EATR_BodyRegion Region)
{
	const int32 Removed = Conditions.RemoveAll([Type, Region](const FATR_Condition& C)
	{
		return C.Type == Type && (Region == EATR_BodyRegion::None || C.Region == Region);
	});

	if (Removed > 0)
	{
		OnConditionChanged.Broadcast(Type, 0.f, false);
		if (Settings->bLogConditionChanges)
		{
			UE_LOG(LogATR_Health, Log, TEXT("[%s] -Condition %d"), *GetOwner()->GetName(), static_cast<int32>(Type));
		}
	}
}

void UATR_HumanHealthComponent::UpdateStatusMarkers()
{
	// Threshold mirrors of vitals/stats — UI/AI visibility only, no penalties.
	const auto Marker = [this](const EATR_ConditionType Type, const bool bActive, const float Severity)
	{
		if (bActive) { SetCondition(Type, Severity); }
		else { RemoveCondition(Type); }
	};

	Marker(EATR_ConditionType::Pain, Vitals.Pain01 > MarkerPain, Vitals.Pain01);
	Marker(EATR_ConditionType::Fever, Vitals.Fever01 > MarkerFever, Vitals.Fever01);
	Marker(EATR_ConditionType::Shock, Vitals.Shock01 > MarkerShock, Vitals.Shock01);
	Marker(EATR_ConditionType::Hypoxia, Vitals.Oxygenation01 < MarkerHypoxiaOx, 1.f - Vitals.Oxygenation01);
	Marker(EATR_ConditionType::Dehydration, Survival.Hydration01 < MarkerDehydration, 1.f - Survival.Hydration01);
	Marker(EATR_ConditionType::Starvation, Survival.Calories01 < MarkerStarvation, 1.f - Survival.Calories01);
	Marker(EATR_ConditionType::SleepDeprivation, Survival.SleepDebt01 > MarkerSleepDebt, Survival.SleepDebt01);
	Marker(EATR_ConditionType::Hypothermia, Vitals.CoreTemperatureC < Settings->HypothermiaOnsetC,
		(Settings->HypothermiaOnsetC - Vitals.CoreTemperatureC) / FMath::Max(Settings->HypothermiaOnsetC - Settings->FatalCoreTempLowC, 1.f));
	Marker(EATR_ConditionType::Hyperthermia, Vitals.CoreTemperatureC > Settings->HyperthermiaOnsetC,
		(Vitals.CoreTemperatureC - Settings->HyperthermiaOnsetC) / FMath::Max(Settings->FatalCoreTempHighC - Settings->HyperthermiaOnsetC, 1.f));
	Marker(EATR_ConditionType::Unconsciousness, bUnconscious, 1.f - Vitals.Consciousness01);
	Marker(EATR_ConditionType::WeakGrip, FMath::Min(Derived.GripStrengthLeft01, Derived.GripStrengthRight01) < MarkerWeakGrip,
		1.f - FMath::Min(Derived.GripStrengthLeft01, Derived.GripStrengthRight01));
	Marker(EATR_ConditionType::Limping, Derived.bIsLimping, 1.f - Derived.MoveSpeedMult);

	const bool bEyeGone = HasImpairment(EATR_ImpairmentType::EyeDestroyed, EATR_BodyRegion::Head);
	const FATR_Condition* Concussion = Conditions.FindByPredicate([](const FATR_Condition& C) { return C.Type == EATR_ConditionType::Concussion; });
	Marker(EATR_ConditionType::VisionImpaired, bEyeGone || (Concussion && Concussion->Severity01 > 0.5f),
		bEyeGone ? 0.5f : (Concussion ? Concussion->Severity01 : 0.f));
	Marker(EATR_ConditionType::HearingImpaired,
		HasImpairment(EATR_ImpairmentType::HearingLoss, EATR_BodyRegion::Head) || (Concussion && Concussion->Severity01 > 0.6f),
		Concussion ? Concussion->Severity01 : 0.5f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Derived stats — recalculated from inputs, never manually maintained
// ─────────────────────────────────────────────────────────────────────────────

void UATR_HumanHealthComponent::RecalculateDerivedStats()
{
	FATR_DerivedCombatStats S;

	const float PainFactor = 1.f - Saturate(Vitals.Pain01 * (1.f - 0.5f * BaseStats.PainTolerance));
	const float FatigueFactor = 1.f - 0.5f * Survival.Fatigue01;
	const float SleepFactor = 1.f - Saturate(0.6f * FMath::Max(0.f, Survival.SleepDebt01 - SubstanceTotals.Alertness));
	const float ConsciousFactor = bUnconscious ? 0.f : (0.3f + 0.7f * Vitals.Consciousness01);

	float DiseaseWeakness = 0.f;
	for (const FATR_ActiveDisease& D : Diseases)
	{
		if (D.Definition) { DiseaseWeakness += D.Definition->Weakness * D.State.Severity01; }
	}
	const float WeaknessFactor = 1.f - Saturate(DiseaseWeakness);

	// Per-side limb integrity from wounds, fractures, impairments.
	const auto SideArmFactor = [this](const bool bLeft) -> float
	{
		const EATR_BodyRegion Upper = bLeft ? EATR_BodyRegion::LeftUpperArm : EATR_BodyRegion::RightUpperArm;
		const EATR_BodyRegion Fore = bLeft ? EATR_BodyRegion::LeftForearm : EATR_BodyRegion::RightForearm;
		float Injury = FMath::Max(WorstWoundSeverity(Upper), WorstWoundSeverity(Fore));
		for (const FATR_Condition& C : Conditions)
		{
			if (C.Type == EATR_ConditionType::Fracture && (C.Region == Upper || C.Region == Fore))
			{
				Injury = FMath::Max(Injury, C.Severity01);
			}
		}
		return 1.f - Saturate(Injury);
	};

	const auto SideHandFactor = [this](const bool bLeft) -> float
	{
		const EATR_BodyRegion Hand = bLeft ? EATR_BodyRegion::LeftHand : EATR_BodyRegion::RightHand;
		if (HasImpairment(EATR_ImpairmentType::LostHand, Hand)) { return 0.f; }

		float Factor = 1.f - Saturate(WorstWoundSeverity(Hand));
		int32 LostFingers = 0;
		for (const FATR_PermanentImpairment& Imp : Impairments)
		{
			if (Imp.Type == EATR_ImpairmentType::LostFinger && Imp.Region == Hand) { ++LostFingers; }
			if (Imp.Type == EATR_ImpairmentType::ReducedGrip && Imp.Region == Hand) { Factor *= 1.f - 0.5f * Imp.Severity01; }
		}
		Factor *= 1.f - 0.15f * LostFingers; // each finger matters, thumb most of all
		return Saturate(Factor);
	};

	const float LeftArm = SideArmFactor(true), RightArm = SideArmFactor(false);
	const float LeftHand = SideHandFactor(true), RightHand = SideHandFactor(false);

	S.GripStrengthLeft01 = Saturate((0.5f + BaseStats.Strength) * LeftArm * LeftHand * PainFactor * WeaknessFactor);
	S.GripStrengthRight01 = Saturate((0.5f + BaseStats.Strength) * RightArm * RightHand * PainFactor * WeaknessFactor);

	const float BestArm = FMath::Max(LeftArm * LeftHand, RightArm * RightHand);
	const float BothArms = FMath::Min(LeftArm * LeftHand, RightArm * RightHand);

	S.MeleePower01 = Saturate((0.5f + BaseStats.Strength) * BestArm * PainFactor * FatigueFactor * WeaknessFactor * ConsciousFactor);
	S.WeaponControl01 = Saturate((0.5f + BaseStats.Dexterity) * BestArm * PainFactor * SleepFactor * ConsciousFactor
		* (1.f - SubstanceTotals.ReactionPenalty));
	S.bCanUseTwoHandedWeapons = BothArms > 0.25f;

	S.AimStability01 = Saturate((0.5f + 0.5f * (BaseStats.Dexterity + BaseStats.Reaction)) * BestArm
		* PainFactor * SleepFactor * FatigueFactor * ConsciousFactor
		* (bShivering ? 0.6f : 1.f)
		* (1.f - SubstanceTotals.ReactionPenalty) * (1.f - SubstanceTotals.BalancePenalty * 0.5f));

	S.ReloadSpeedMult = FMath::Max(0.2f, FMath::Min(S.GripStrengthLeft01, S.GripStrengthRight01) * 0.5f + 0.5f * S.WeaponControl01);

	// Legs: worst leg decides the limp; both decide the speed.
	const auto SideLegFactor = [this](const bool bLeft) -> float
	{
		const EATR_BodyRegion Thigh = bLeft ? EATR_BodyRegion::LeftThigh : EATR_BodyRegion::RightThigh;
		const EATR_BodyRegion Shin = bLeft ? EATR_BodyRegion::LeftShin : EATR_BodyRegion::RightShin;
		const EATR_BodyRegion Foot = bLeft ? EATR_BodyRegion::LeftFoot : EATR_BodyRegion::RightFoot;

		float Injury = FMath::Max3(WorstWoundSeverity(Thigh), WorstWoundSeverity(Shin), 0.7f * WorstWoundSeverity(Foot));
		for (const FATR_Condition& C : Conditions)
		{
			if (C.Type == EATR_ConditionType::Fracture && (C.Region == Thigh || C.Region == Shin || C.Region == Foot))
			{
				// Splinted fractures partially restore function — find the wound.
				const FATR_Wound* W = Wounds.FindByPredicate([&C](const FATR_Wound& X) { return X.WoundId == C.SourceWoundId; });
				const float Eff = (W && W->Treatment.bSplinted) ? C.Severity01 * 0.4f : C.Severity01;
				Injury = FMath::Max(Injury, Eff);
			}
		}
		for (const FATR_PermanentImpairment& Imp : Impairments)
		{
			if (Imp.Type == EATR_ImpairmentType::ReducedMobility && (Imp.Region == Thigh || Imp.Region == Shin || Imp.Region == Foot))
			{
				Injury = FMath::Max(Injury, 0.5f * Imp.Severity01);
			}
			if ((Imp.Type == EATR_ImpairmentType::LostFoot || Imp.Type == EATR_ImpairmentType::LostLimb) && Imp.Region == Foot)
			{
				Injury = 1.f;
			}
		}
		return 1.f - Saturate(Injury);
	};

	const float LeftLeg = SideLegFactor(true), RightLeg = SideLegFactor(false);
	const float WorstLeg = FMath::Min(LeftLeg, RightLeg);

	S.bIsLimping = WorstLeg < 0.6f;
	S.MoveSpeedMult = FMath::Max(0.15f, (0.4f + 0.6f * WorstLeg) * ConsciousFactor * (1.f - SubstanceTotals.BalancePenalty * 0.3f));
	S.bCanSprint = WorstLeg > 0.4f && !bUnconscious && Vitals.Oxygenation01 > 0.5f;

	// Chest/lungs drive endurance.
	const float ChestFactor = 1.f - 0.5f * Saturate(WorstWoundSeverity(EATR_BodyRegion::Chest));
	const float LungFactor = 0.5f * (Vitals.LeftLungFunction01 + Vitals.RightLungFunction01);
	S.SprintDurationMult = FMath::Max(0.1f, (0.5f + BaseStats.Endurance) * ChestFactor * LungFactor * FatigueFactor * (0.5f + 0.5f * Survival.Calories01));
	S.StaminaRecoveryMult = FMath::Max(0.1f, (0.5f + BaseStats.Endurance) * LungFactor * (0.5f + 0.5f * Survival.Hydration01) * SleepFactor);

	S.NoiseMult = S.bIsLimping ? 1.5f : (1.f - 0.3f * BaseStats.Dexterity * WorstLeg);

	S.ClimbAbility01 = Saturate(BothArms * WorstLeg * (0.5f + BaseStats.Strength) * ConsciousFactor);
	S.bCanClimb = S.ClimbAbility01 > 0.2f;
	S.ShovePower01 = S.MeleePower01;
	S.GrappleResistance01 = Saturate((0.5f + BaseStats.Strength) * (0.5f + 0.5f * BaseStats.Balance) * WorstLeg * PainFactor * WeaknessFactor);

	const bool bChanged = FMemory::Memcmp(&S, &Derived, sizeof(FATR_DerivedCombatStats)) != 0;
	Derived = S;
	if (bChanged)
	{
		OnDerivedStatsChanged.Broadcast();
	}
}

void UATR_HumanHealthComponent::RecalculateSubstanceTotals()
{
	FSubstanceTotals T;
	for (const FATR_ActiveSubstance& S : Substances)
	{
		if (!S.Definition) { continue; }
		const float Strength = S.State.Severity01 * S.State.Dose;
		T.PainRelief += S.Definition->PainRelief * Strength;
		T.FeverReduction += S.Definition->FeverReduction * Strength;
		T.InfectionSuppression += S.Definition->InfectionSuppression * Strength;
		T.Alertness += S.Definition->Alertness * Strength;
		T.Sedation += S.Definition->Sedation * Strength;
		T.ReactionPenalty += S.Definition->ReactionPenalty * Strength;
		T.PerceptionPenalty += S.Definition->PerceptionPenalty * Strength;
		T.BalancePenalty += S.Definition->BalancePenalty * Strength;
		T.HydrationDrainPerSecond += S.Definition->HydrationDrain * Strength;
	}
	T.PainRelief = Saturate(T.PainRelief);
	T.FeverReduction = Saturate(T.FeverReduction);
	T.InfectionSuppression = Saturate(T.InfectionSuppression);
	T.Alertness = Saturate(T.Alertness);
	T.Sedation = Saturate(T.Sedation);
	T.ReactionPenalty = Saturate(T.ReactionPenalty);
	T.PerceptionPenalty = Saturate(T.PerceptionPenalty);
	T.BalancePenalty = Saturate(T.BalancePenalty);
	SubstanceTotals = T;
}

// ─────────────────────────────────────────────────────────────────────────────
// Death & helpers
// ─────────────────────────────────────────────────────────────────────────────

void UATR_HumanHealthComponent::Die(const EATR_DeathCause Cause)
{
	if (!IsAlive())
	{
		return;
	}

	DeathCause = Cause; // preserved for UI/debugging
	Vitals.Consciousness01 = 0.f;
	bUnconscious = true;
	SetComponentTickEnabled(false);

	UE_LOG(LogATR_Health, Log, TEXT("[%s] DIED — cause %d. Last damage: %s"),
		*GetOwner()->GetName(), static_cast<int32>(Cause), *LastDamageDebug);

	OnDeath.Broadcast(Cause);
}

FATR_BodyRegionRow UATR_HumanHealthComponent::GetRegionRow(const EATR_BodyRegion Region) const
{
	if (RegionDef)
	{
		if (const FATR_BodyRegionRow* Row = RegionDef->FindRow(Region))
		{
			return *Row;
		}
	}

	// Code defaults so the system runs content-free.
	FATR_BodyRegionRow R;
	R.Region = Region;
	switch (Region)
	{
	case EATR_BodyRegion::Head:
		R.BleedScale = 1.2f; R.PainScale = 1.2f; R.BrainDamageScale = 0.9f; R.CatastrophicSeverityThreshold = 0.95f; break;
	case EATR_BodyRegion::Neck:
		R.BleedScale = 2.5f; R.PainScale = 1.f; R.BrainDamageScale = 0.2f; R.CatastrophicSeverityThreshold = 0.9f; break;
	case EATR_BodyRegion::Chest:
		R.BleedScale = 1.f; R.PainScale = 1.f; R.HeartDamageScale = 0.5f; R.LungDamageScale = 0.8f; break;
	case EATR_BodyRegion::Abdomen:
		R.BleedScale = 1.2f; R.PainScale = 1.1f; break;
	case EATR_BodyRegion::Pelvis:
		R.BleedScale = 0.9f; R.PainScale = 1.f; break;
	case EATR_BodyRegion::LeftHand: case EATR_BodyRegion::RightHand:
	case EATR_BodyRegion::LeftFoot: case EATR_BodyRegion::RightFoot:
		R.BleedScale = 0.5f; R.PainScale = 1.3f; break;
	case EATR_BodyRegion::LeftThigh: case EATR_BodyRegion::RightThigh:
		R.BleedScale = 1.1f; R.PainScale = 0.9f; break; // femoral
	default:
		R.BleedScale = 0.7f; R.PainScale = 0.9f; break;
	}
	return R;
}

FATR_DamageTypeRow UATR_HumanHealthComponent::GetDamageTypeRow(const EATR_DamageType Type) const
{
	if (DamageTypeDef)
	{
		if (const FATR_DamageTypeRow* Row = DamageTypeDef->FindRow(Type))
		{
			return *Row;
		}
	}

	// Code defaults implementing the design doc's damage-behavior table.
	FATR_DamageTypeRow R;
	R.DamageType = Type;
	switch (Type)
	{
	case EATR_DamageType::Blunt:
		R.BleedFactor = 0.f; R.InternalBleedFactor = 0.4f; R.PainFactor = 0.8f; R.StructuralFactor = 0.8f; R.DepthFactor = 0.1f; R.InfectionFactor = 0.1f; break;
	case EATR_DamageType::Slash:
		R.BleedFactor = 0.005f; R.PainFactor = 1.f; R.StructuralFactor = 0.2f; R.DepthFactor = 0.2f; R.InfectionFactor = 0.5f; break;
	case EATR_DamageType::Puncture:
		R.BleedFactor = 0.002f; R.InternalBleedFactor = 0.3f; R.PainFactor = 0.8f; R.StructuralFactor = 0.3f; R.DepthFactor = 1.f; R.InfectionFactor = 0.7f; break;
	case EATR_DamageType::Crush:
		R.BleedFactor = 0.0005f; R.InternalBleedFactor = 0.5f; R.PainFactor = 1.1f; R.StructuralFactor = 1.2f; R.DepthFactor = 0.2f; R.InfectionFactor = 0.2f; break;
	case EATR_DamageType::Bite:
		R.BleedFactor = 0.003f; R.PainFactor = 1.f; R.StructuralFactor = 0.4f; R.DepthFactor = 0.3f; R.InfectionFactor = 1.5f; break;
	case EATR_DamageType::Burn:
		R.BleedFactor = 0.f; R.PainFactor = 1.5f; R.StructuralFactor = 0.1f; R.DepthFactor = 0.1f; R.InfectionFactor = 0.8f; R.FluidLossFactor = 0.1f; break;
	case EATR_DamageType::Ballistic:
		R.BleedFactor = 0.006f; R.InternalBleedFactor = 0.4f; R.PainFactor = 1.f; R.StructuralFactor = 0.6f; R.DepthFactor = 1.2f; R.InfectionFactor = 0.4f; break;
	case EATR_DamageType::Fall:
		R.BleedFactor = 0.0002f; R.InternalBleedFactor = 0.4f; R.PainFactor = 0.9f; R.StructuralFactor = 1.f; R.DepthFactor = 0.1f; R.InfectionFactor = 0.1f; break;
	case EATR_DamageType::Explosion:
		R.BleedFactor = 0.004f; R.InternalBleedFactor = 0.6f; R.PainFactor = 1.2f; R.StructuralFactor = 1.f; R.DepthFactor = 0.5f; R.InfectionFactor = 0.6f; break;
	default:
		R.PainFactor = 0.3f; break; // Toxic/Disease never reach wound creation
	}
	return R;
}

float UATR_HumanHealthComponent::WorstWoundSeverity(const EATR_BodyRegion Region) const
{
	float Worst = 0.f;
	for (const FATR_Wound& W : Wounds)
	{
		if (W.Region == Region && W.IsActive() && !W.bInternal)
		{
			Worst = FMath::Max(Worst, W.Severity01);
		}
	}
	return Worst;
}

bool UATR_HumanHealthComponent::HasImpairment(const EATR_ImpairmentType Type, const EATR_BodyRegion Region) const
{
	return Impairments.ContainsByPredicate([Type, Region](const FATR_PermanentImpairment& I)
	{
		return I.Type == Type && (Region == EATR_BodyRegion::None || I.Region == Region);
	});
}

// ─────────────────────────────────────────────────────────────────────────────
// Debug
// ─────────────────────────────────────────────────────────────────────────────

FString UATR_HumanHealthComponent::GetDebugString() const
{
	TStringBuilder<2048> Sb;

	Sb.Appendf(TEXT("== %s ==\n"), GetOwner() ? *GetOwner()->GetName() : TEXT("?"));
	Sb.Appendf(TEXT("Alive=%d Cause=%d Conscious=%d Sleeping=%d\n"),
		IsAlive() ? 1 : 0, static_cast<int32>(DeathCause), IsConscious() ? 1 : 0, bSleeping ? 1 : 0);
	Sb.Appendf(TEXT("Vitals: Vol=%.2f BP=%.2f Ox=%.2f Brain=%.2f Heart=%.2f Lungs=%.2f/%.2f\n"),
		Vitals.BloodVolume01, Vitals.BloodPressure01, Vitals.Oxygenation01, Vitals.BrainFunction01,
		Vitals.HeartFunction01, Vitals.LeftLungFunction01, Vitals.RightLungFunction01);
	Sb.Appendf(TEXT("        Con=%.2f Pain=%.2f Shock=%.2f Temp=%.1fC Inf=%.2f Fever=%.2f Tox=%.2f\n"),
		Vitals.Consciousness01, Vitals.Pain01, Vitals.Shock01, Vitals.CoreTemperatureC,
		Vitals.Infection01, Vitals.Fever01, Vitals.Toxicity01);
	Sb.Appendf(TEXT("Survival: Hyd=%.2f Cal=%.2f Prot=%.2f Qual=%.2f Sleep=%.2f Fat=%.2f Wet=%.2f Imm=%.2f\n"),
		Survival.Hydration01, Survival.Calories01, Survival.ProteinRepairReserve01, Survival.NutritionQuality01,
		Survival.SleepDebt01, Survival.Fatigue01, Survival.Wetness01, Survival.ImmuneStrength01);
	Sb.Appendf(TEXT("Blood: type=%d reaction=%.2f\n"), static_cast<int32>(Blood.BloodType), Blood.TransfusionReactionSeverity01);

	Sb.Appendf(TEXT("Wounds (%d):\n"), Wounds.Num());
	for (const FATR_Wound& W : Wounds)
	{
		Sb.Appendf(TEXT("  #%d r=%d t=%d sev=%.2f bleed=%.4f risk=%.2f stage=%d%s%s%s%s%s%s\n"),
			W.WoundId, static_cast<int32>(W.Region), static_cast<int32>(W.DamageType), W.Severity01, W.BleedRate,
			W.InfectionRisk01, static_cast<int32>(W.Stage),
			W.bInternal ? TEXT(" INTERNAL") : TEXT(""),
			W.Treatment.bBandaged ? TEXT(" bandaged") : TEXT(""),
			W.Treatment.bTourniquet ? TEXT(" tourniquet") : TEXT(""),
			W.Treatment.bDisinfected ? TEXT(" disinfected") : TEXT(""),
			W.Treatment.bSutured ? TEXT(" sutured") : TEXT(""),
			W.Treatment.bSplinted ? TEXT(" splinted") : TEXT(""));
	}

	Sb.Appendf(TEXT("Conditions (%d):"), Conditions.Num());
	for (const FATR_Condition& C : Conditions)
	{
		Sb.Appendf(TEXT(" [%d sev=%.2f]"), static_cast<int32>(C.Type), C.Severity01);
	}
	Sb.Append(TEXT("\n"));

	Sb.Appendf(TEXT("Diseases (%d) Substances (%d) Impairments (%d)\n"), Diseases.Num(), Substances.Num(), Impairments.Num());
	Sb.Appendf(TEXT("Derived: gripL=%.2f gripR=%.2f melee=%.2f aim=%.2f move=%.2f sprint=%d limp=%d 2h=%d\n"),
		Derived.GripStrengthLeft01, Derived.GripStrengthRight01, Derived.MeleePower01, Derived.AimStability01,
		Derived.MoveSpeedMult, Derived.bCanSprint ? 1 : 0, Derived.bIsLimping ? 1 : 0, Derived.bCanUseTwoHandedWeapons ? 1 : 0);
	Sb.Appendf(TEXT("LastDamage: %s\n"), *LastDamageDeb