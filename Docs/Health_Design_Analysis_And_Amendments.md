# ATR Health / Stats / Combat / Survival — Design Analysis & Amendments

Analysis of `ATR_Health_Stats_Combat_Survival_Design.md` against itself and the existing
codebase, plus the amendments applied in the implementation. The design is sound overall:
no monolithic HP, cause-based death, cheap structural Echoes, data/event-driven, tiered
fidelity. The issues below are duplication and missing linkage, not direction.

---

## Issues Found and Amendments Applied

### 1. Body temperature tracked twice (contradiction)
`FHumanVitals.BodyTemperatureC` and `FHumanSurvivalStats.CoreTemperatureC` are the same
physical quantity in two structs. Two copies will drift and double-apply penalties.

**Amendment:** One canonical `CoreTemperatureC` lives in vitals. Survival stats keep
`Wetness01` (an *input* to temperature), not a second temperature.

### 2. Blood volume tracked twice (contradiction)
`FHumanVitals.BloodVolume01` and `FBloodState.BloodVolume01` duplicate each other.

**Amendment:** Vitals own blood volume. `FATR_BloodState` keeps only type and
transfusion-reaction state.

### 3. Pain / Shock / Fever exist as both vitals and conditions (ambiguity)
The doc lists Pain, Shock, and Fever both as vital scalars and as condition types.
Unclarified, this double-applies penalties.

**Amendment:** The vital scalar is the single aggregated truth (e.g. `Pain01` sums wound
pain, condition pain, disease pain, minus painkillers). The condition entry is a discrete
threshold marker derived *from* the vital, used for UI/AI queries only — it never applies
penalties itself. Same rule for derived-status conditions (`WeakGrip`, `Limping`,
`VisionImpaired`, `HearingImpaired`): markers only; penalties flow exclusively through
derived stats so nothing is counted twice.

### 4. Wounds and treatment state had no linkage (missing data)
`FWoundTreatmentState` is defined with no way to associate it with a wound, and
`FHumanWound` has no identity, so treatment ("bandage *this* wound") is unimplementable
as written.

**Amendment:** Wounds get a stable `WoundId`; treatment state is embedded per wound.

### 5. Substance effects can't be ticked as specified (missing data)
`FTimedSubstanceEffect` has onset/peak/duration but no elapsed-time field, so the effect
curve has no position.

**Amendment:** Added `TimeActiveSeconds`; strength is computed from it
(ramp to onset→peak, hold, decay over duration scaled by metabolism).

### 6. Fingers/toes as body regions vs. sparse detail (ambiguity)
The doc says don't tick unused fine-detail parts, but wounds only carry `EBodyRegion`.

**Amendment:** Wounds carry an optional `DetailKind` + `DetailIndex`
(finger/toe/eye/ear/jaw) on top of the parent region. No per-digit arrays exist unless a
wound references one — matching the "spawn detail only when damaged" rule.

### 7. Echo `HeadPresent` vs `BrainIntegrity` overlap (clarification)
A detached head with nonzero brain integrity is contradictory.

**Amendment (rule):** clearing `HeadPresent` forces `BrainIntegrity = 0` → dead
(per the doc's own recommendation, no severed-head gameplay in first pass).

### 8. Naming
Doc names (`FHumanVitals`, `FEchoHealthSoA`, …) don't follow project conventions.
Implemented as `FATR_*`, `EATR_*`, `UATR_*` to match the existing Echo system.

### 9. `DA_EchoBodyDefinition` / `DA_EchoCapabilityRules` folded into settings
Echo capability rules are a handful of thresholds and booleans, not designer content.
First pass puts them in `UATR_HealthSettings` (Echo section) next to the replication
limits; they can be promoted to DataAssets later if per-archetype variation is needed.
All *content-like* data (weapons, conditions, diseases, substances, treatments, armor
materials, body regions) is in DataAssets as the doc requires.

---

## Scope Notes (unchanged from doc, restated as commitments)

- Implemented standalone under `Source/AllThatRemains/Health/`. **No existing system was
  modified** — no edits to the Echo subsystem, replication component, manager, player, or
  Build.cs (module deps already suffice). Linking (combat hits → `ApplyDamageEvent`, Echo
  subsystem → `FATR_EchoHealthModel`, clothing → mitigation interface) happens after review.
- Human sim is server-authoritative; human-state replication is deliberately deferred to
  the linking pass. Echo deltas are produced as compact structs ready for the existing
  replication component to carry.
- Kept lightweight per doc: no vitamin chemistry, no addiction sim, no surgery,
  no prosthetics, blood compatibility = standard type matrix only.

---

## Where Things Live

| Concern | File |
|---|---|
| Shared enums, damage event, mitigation interface | `Health/ATR_HealthTypes.h` |
| Human vitals/survival/wounds/conditions/disease/substances/blood/impairments | `Health/Human/ATR_HumanHealthTypes.h` |
| Base + derived stats | `Health/Human/ATR_HumanStats.h` |
| Human simulation component (combat pipeline, tick buckets, treatment, healing, death) | `Health/Human/ATR_HumanHealthComponent.h/.cpp` |
| Tunables (rates, fatal thresholds, tick Hz, Echo rules, debug toggles) | `Health/ATR_HealthSettings.h/.cpp` |
| Content DataAssets | `Health/Data/ATR_*Definition.h`, `ATR_WeaponDamageProfile.h` |
| Echo SoA, masks, capability flags, deltas | `Health/Echo/ATR_EchoHealthTypes.h` |
| Echo structural model (damage, recompute, delta queue) | `Health/Echo/ATR_EchoHealthModel.h/.cpp` |
