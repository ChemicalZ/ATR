# Health Linking Pass — Echoes ⇄ Players, Authority & Replication

The health systems (`Source/AllThatRemains/Health/`) are now wired into the live game.
Everything mutates on the **server only**; clients receive replicated mirrors.

---

## What was linked

### Echo structural health → Echo subsystem
- `UATR_EchoSubsystem` owns one `FATR_EchoHealthModel` (rows index-parallel to the SoA,
  allocated once in `Initialize`). `RemoveEcho` mirrors its swap-remove into the model
  (`HandleSwapRemove`) so rows never drift; reused rows are re-announced to clients.
- **Damage entry point:** `UATR_EchoSubsystem::ApplyDamageToEcho(SoAIndex, FATR_DamageEvent)`
  (BlueprintCallable, server-only). Region mapping:
  - Head → brain erosion (`EchoBrainDamageScale`) + sever roll (head off = dead).
  - Chest/Abdomen/Pelvis → spine destruction on deep Ballistic/Crush/Explosion hits
    (`EchoSpineDestroySeverity`).
  - Neck/limbs → sever roll (`DismemberChance × severity × EchoDismemberScale`); distal parts
    follow.
  - Every hit accrues cosmetic `AccumulatedDamage` (gore presentation seam).
- **Death:** brain destruction only. Pending deltas are flushed to clients, then
  `ForceDestroyEcho` removes the Echo (clients also see the snapshot despawn).
- **Capability consequences:** `UpdateEchoIntent` only selects `Attack` when the Echo can
  attack standing/crawling; `AATR_ActiveEcho::ApplyStructuralStateToMovement` applies
  limp (`LimpSpeedScale`), crawl (`CrawlSpeed`), or immobility on promotion and after damage.

### Echo health replication
- `FATR_EchoHealthDelta`s drain each server tick (cap: `MaxStructuralDeltasPerUpdate`) and
  broadcast via `UATR_EchoReplicationComponent::Client_EchoHealthDeltas` (**reliable** — rare,
  small, order-sensitive). Clients apply into their local model mirror, sequence-gated per Echo.
- Newly-relevant echoes that already carry damage get a `FullStructuralRefresh` when they first
  enter a client's snapshot stream (`BuildAndSendBand`).

### Echo melee → player damage
- `AATR_ActiveEcho::TryGrabTarget/TryBiteTarget/PullTarget` native implementations now resolve
  real combat (server-side) through the health model + `UATR_EchoSettings` (Echo|Combat):
  - Grab: capability (no arms = `NoGrip`) → facing cone → miss roll (a whiff can scratch).
    Grip is Strong/Weak by finger count (`MinFingersForStrongGrab`). `CurrentGrip` replicates.
  - Bite: requires jaw+neck. Tier roll (scratch / deep scratch / **laceration**) weighted by
    grip; applied to the victim's `UATR_HumanHealthComponent` as a contaminated Bite/Slash
    `FATR_DamageEvent` (or via the optional `BiteDamageProfile` asset, EventScale per tier).
  - Pull: per-second velocity drag toward the echo (`MeleePullSpeed`, `WeakGripPullScale`).
- Resolution is logged structurally to `LogATR_EchoCombat`.

### Human health replication
- `UATR_HumanHealthComponent` now replicates observable state: vitals, survival, derived
  stats, blood, wounds, conditions, impairments, plus `DeathCause`/`bUnconscious` with
  RepNotify (client-side `OnDeath`/`OnConsciousnessChanged` fire). Diseases/substances stay
  server-only. All mutation APIs remain authority-gated; simulation ticks only with authority.

### Player
- `AATR_Player` owns a `UATR_HumanHealthComponent` ("Health"). Echo bites tick bleeding,
  pain, shock, infection, etc. through the existing fast/medium/slow buckets.
- **Debug melee:** `DebugAttackAction` → client sends only its camera ray →
  `Server_DebugMeleeAttack` traces (ECC_Pawn, `DebugMeleeRange` from the pawn), resolves the
  hit body region (`ATR_Health::RegionFromHitLocation` — coarse height/lateral zones), and
  routes damage: Echo → `ApplyDamageToEcho`; human → `ApplyDamageEvent`. Tunables and an
  optional weapon profile live in Health & Survival → **Debug|Melee**; debug draw included.

---

## Editor setup required
1. Create an Input Action `IA_DebugAttack` (bool), add it to the default mapping context
   (e.g. LMB), and assign it to **DebugAttackAction** on the player Blueprint.
2. Optional: author `UATR_WeaponDamageProfile` assets and assign **Debug|Melee → DebugMeleeProfile**
   and **Echo|Combat → BiteDamageProfile** (system runs content-free without them).

## Quick test plan
- PIE (listen server + client). Let an echo reach a player: expect grab logs, pull drag, bite
  wounds in `LogATR_EchoCombat` / `LogATR_Health`; victim wounds/vitals visible on both
  machines (`Health->GetDebugString()`).
- Debug-attack an echo's legs/arms: expect sever rolls, limp/crawl speed changes replicated
  via capability deltas. Head hits: brain erosion, then death + despawn on all machines.
- Bleed out from stacked lacerations: `CirculatoryCollapse` death path; client fires `OnDeath`.
