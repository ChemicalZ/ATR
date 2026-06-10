// Fill out your copyright notice in the Description page of Project Settings.

#include "SATR_EchoDebugMapView.h"
#include "../ATR_EchoSubsystem.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Fonts/SlateFontInfo.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

// Approx. footprint used to scale echo/player markers with zoom (cm).
static constexpr float GEchoAgentDiameterCm = 110.f;
static constexpr float GPlayerSizeCm        = 240.f;

void SATR_EchoDebugMapView::Construct(const FArguments& InArgs)
{
	SetCanTick(false);
}

UATR_EchoSubsystem* SATR_EchoDebugMapView::GetSubsystem() const
{
	return WeakSubsystem;
}

FVector2D SATR_EchoDebugMapView::WorldToScreen(const FVector2D& World, const FVector2D& LocalSize) const
{
	const FVector2D Rel = (World - ViewCenterWorld) * PixelsPerCm;
	return FVector2D(LocalSize.X * 0.5 + Rel.X, LocalSize.Y * 0.5 - Rel.Y); // +Y world = up
}

FVector2D SATR_EchoDebugMapView::ScreenToWorld(const FVector2D& Screen, const FVector2D& LocalSize) const
{
	const FVector2D D(Screen.X - LocalSize.X * 0.5, Screen.Y - LocalSize.Y * 0.5);
	const float Inv = (PixelsPerCm > KINDA_SMALL_NUMBER) ? 1.f / PixelsPerCm : 0.f;
	return ViewCenterWorld + FVector2D(D.X, -D.Y) * Inv;
}

void SATR_EchoDebugMapView::EnsureInitialized(const FVector2D& LocalSize) const
{
	if (bViewInitialized || LocalSize.X < 1.f || LocalSize.Y < 1.f)
		return;

	const UATR_EchoSubsystem* Sub = GetSubsystem();
	const float FullExtent = (Sub ? Sub->WorldHalfExtent : 50000.f) * 2.f;
	PixelsPerCm      = FMath::Min(LocalSize.X, LocalSize.Y) / FMath::Max(1.f, FullExtent);
	ViewCenterWorld  = FVector2D::ZeroVector;
	bViewInitialized = true;
}

void SATR_EchoDebugMapView::ZoomBy(float Factor)
{
	PixelsPerCm      = FMath::Clamp(PixelsPerCm * Factor, 1e-5f, 2.f);
	bViewInitialized = true;
}

void SATR_EchoDebugMapView::ResetView()
{
	bViewInitialized = false; // reframed to full world extent on next paint
}

void SATR_EchoDebugMapView::FocusOnEchoes()
{
	const UATR_EchoSubsystem* Sub = GetSubsystem();
	if (!Sub) return;

	const int32 N = FMath::Min(Sub->ActiveEntities, Sub->Positions.Num());
	if (N <= 0) return;

	FVector2D Min(Sub->Positions[0].X, Sub->Positions[0].Y);
	FVector2D Max = Min;
	for (int32 i = 1; i < N; ++i)
	{
		const FVector3f& P = Sub->Positions[i];
		Min.X = FMath::Min<double>(Min.X, P.X); Min.Y = FMath::Min<double>(Min.Y, P.Y);
		Max.X = FMath::Max<double>(Max.X, P.X); Max.Y = FMath::Max<double>(Max.Y, P.Y);
	}

	ViewCenterWorld = (Min + Max) * 0.5;
	const FVector2D Span = (Max - Min) + FVector2D(2000.f, 2000.f); // padding
	const FVector2D LS   = GetTickSpaceGeometry().GetLocalSize();
	if (LS.X > 1.f && LS.Y > 1.f && Span.X > 1.f && Span.Y > 1.f)
		PixelsPerCm = FMath::Min(LS.X / Span.X, LS.Y / Span.Y);
	bViewInitialized = true;
}

FLinearColor SATR_EchoDebugMapView::IntentColor(uint8 Intent)
{
	switch (Intent)
	{
		case 0:  return FLinearColor(0.25f, 0.25f, 0.28f);          // Dormant
		case 1:  return FLinearColor(0.55f, 0.55f, 0.55f);          // Idle
		case 2:  return FLinearColor(0.40f, 0.60f, 1.00f);          // Wander
		case 3:  return FLinearColor(0.30f, 0.90f, 0.90f);          // TurnTowardStimulus
		case 4:  return FLinearColor(1.00f, 0.90f, 0.20f);          // InvestigateLocation
		case 5:  return FLinearColor(1.00f, 0.22f, 0.12f);          // ChaseVisibleActor
		case 6:  return FLinearColor(1.00f, 0.50f, 0.12f);          // ChaseLastSeenLocation
		case 7:  return FLinearColor(1.00f, 0.66f, 0.22f);          // SearchProjectedDirection
		case 8:  return FLinearColor(0.90f, 0.30f, 0.90f);          // FanSearchArea
		case 9:  return FLinearColor(0.20f, 1.00f, 0.30f);          // JoinHordePressure
		case 10: return FLinearColor(1.00f, 0.00f, 0.00f);          // Attack
		case 11: return FLinearColor(0.60f, 0.30f, 1.00f);          // HandleObstacle
		case 12: return FLinearColor(0.50f, 0.50f, 0.50f);          // ReturnToIdle
		default: return FLinearColor::White;
	}
}

FLinearColor SATR_EchoDebugMapView::HeatColor(float T)
{
	T = FMath::Clamp(T, 0.f, 1.f);
	if (T < 0.33f) return FMath::Lerp(FLinearColor(0.10f, 0.20f, 1.00f), FLinearColor(0.10f, 1.00f, 0.30f), T / 0.33f);
	if (T < 0.66f) return FMath::Lerp(FLinearColor(0.10f, 1.00f, 0.30f), FLinearColor(1.00f, 0.95f, 0.10f), (T - 0.33f) / 0.33f);
	return FMath::Lerp(FLinearColor(1.00f, 0.95f, 0.10f), FLinearColor(1.00f, 0.10f, 0.05f), (T - 0.66f) / 0.34f);
}

int32 SATR_EchoDebugMapView::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D LS = AllottedGeometry.GetLocalSize();
	EnsureInitialized(LS);

	// Layer assignment (low → high draw order).
	const int32 LBg        = LayerId;
	const int32 LAbstract  = LayerId + 1;
	const int32 LAgitation = LayerId + 2;
	const int32 LTarget    = LayerId + 3;
	const int32 LEcho      = LayerId + 4;
	const int32 LArrow     = LayerId + 5;
	const int32 LPlayer    = LayerId + 6;
	const int32 LText      = LayerId + 7;

	const FSlateFontInfo Font8  = FCoreStyle::GetDefaultFontStyle("Regular", 8);
	const FSlateFontInfo Font10 = FCoreStyle::GetDefaultFontStyle("Regular", 10);

	auto Box = [&](int32 L, const FVector2D& Pos, const FVector2D& Size, const FLinearColor& C)
	{
		FSlateDrawElement::MakeBox(
			OutDrawElements, L,
			AllottedGeometry.ToPaintGeometry(FVector2f((float)Size.X, (float)Size.Y),
				FSlateLayoutTransform(FVector2f((float)Pos.X, (float)Pos.Y))),
			&FillBrush, ESlateDrawEffect::None, C);
	};
	auto Line = [&](int32 L, const FVector2D& A, const FVector2D& B, const FLinearColor& C, float Th)
	{
		TArray<FVector2D> Pts; Pts.Add(A); Pts.Add(B);
		FSlateDrawElement::MakeLines(OutDrawElements, L, AllottedGeometry.ToPaintGeometry(), Pts, ESlateDrawEffect::None, C, true, Th);
	};
	// Line with an arrowhead at B.
	auto Arrow = [&](int32 L, const FVector2D& A, const FVector2D& B, const FLinearColor& C, float Th)
	{
		Line(L, A, B, C, Th);
		FVector2D D = B - A;
		const float Len = D.Size();
		if (Len < 1.0) return;
		D /= Len;
		const FVector2D Perp(-D.Y, D.X);
		const float Head = FMath::Clamp(Len * 0.35f, 5.f, 12.f);
		const FVector2D Base = B - D * Head;
		Line(L, B, Base + Perp * (Head * 0.5f), C, Th);
		Line(L, B, Base - Perp * (Head * 0.5f), C, Th);
	};
	auto Text = [&](int32 L, const FVector2D& Pos, const FString& S, const FSlateFontInfo& F, const FLinearColor& C)
	{
		FSlateDrawElement::MakeText(
			OutDrawElements, L,
			AllottedGeometry.ToPaintGeometry(FVector2f(600.f, 18.f), FSlateLayoutTransform(FVector2f((float)Pos.X, (float)Pos.Y))),
			S, F, ESlateDrawEffect::None, C);
	};

	// Backdrop.
	Box(LBg, FVector2D::ZeroVector, LS, FLinearColor(0.04f, 0.04f, 0.06f, 0.88f));

	const UATR_EchoSubsystem* Sub = GetSubsystem();
	if (!Sub)
	{
		Text(LText, FVector2D(10, 10), TEXT("Echo subsystem not available in this world."), Font10, FLinearColor::White);
		return LText;
	}

	// Visible world AABB for culling.
	const FVector2D W0 = ScreenToWorld(FVector2D(0, 0), LS);
	const FVector2D W1 = ScreenToWorld(LS, LS);
	const FVector2D VisMin(FMath::Min(W0.X, W1.X), FMath::Min(W0.Y, W1.Y));
	const FVector2D VisMax(FMath::Max(W0.X, W1.X), FMath::Max(W0.Y, W1.Y));
	auto Overlaps = [&](const FVector2D& Mn, const FVector2D& Mx)
	{
		return !(Mx.X < VisMin.X || Mn.X > VisMax.X || Mx.Y < VisMin.Y || Mn.Y > VisMax.Y);
	};
	auto InView = [&](const FVector2D& W)
	{
		return !(W.X < VisMin.X || W.X > VisMax.X || W.Y < VisMin.Y || W.Y > VisMax.Y);
	};

	// World extent boundary + origin crosshair.
	{
		const float HE = Sub->WorldHalfExtent;
		const FVector2D TL = WorldToScreen(FVector2D(-HE,  HE), LS);
		const FVector2D TR = WorldToScreen(FVector2D( HE,  HE), LS);
		const FVector2D BR = WorldToScreen(FVector2D( HE, -HE), LS);
		const FVector2D BL = WorldToScreen(FVector2D(-HE, -HE), LS);
		const FLinearColor Edge(0.30f, 0.30f, 0.40f, 0.8f);
		Line(LAbstract, TL, TR, Edge, 1.f); Line(LAbstract, TR, BR, Edge, 1.f);
		Line(LAbstract, BR, BL, Edge, 1.f); Line(LAbstract, BL, TL, Edge, 1.f);

		const FVector2D O = WorldToScreen(FVector2D::ZeroVector, LS);
		const FLinearColor Axis(0.45f, 0.45f, 0.55f, 0.6f);
		Line(LAbstract, O - FVector2D(8, 0), O + FVector2D(8, 0), Axis, 1.f);
		Line(LAbstract, O - FVector2D(0, 8), O + FVector2D(0, 8), Axis, 1.f);
	}

	// ── Abstract population cells (coarse) ──
	if (bShowAbstractCells)
	{
		const float CS = FMath::Max(1.f, Sub->CoarseGridCellSize);
		for (const TPair<FIntPoint, FATR_AbstractCell>& Pair : Sub->AbstractCells)
		{
			const FIntPoint K = Pair.Key;
			const FATR_AbstractCell& Cell = Pair.Value;
			const FVector2D Mn(K.X * CS, K.Y * CS);
			const FVector2D Mx = Mn + FVector2D(CS, CS);
			if (!Overlaps(Mn, Mx)) continue;

			const FVector2D STL = WorldToScreen(FVector2D(Mn.X, Mx.Y), LS);
			const FVector2D STR = WorldToScreen(FVector2D(Mx.X, Mx.Y), LS);
			const FVector2D SBR = WorldToScreen(FVector2D(Mx.X, Mn.Y), LS);
			const FVector2D SBL = WorldToScreen(FVector2D(Mn.X, Mn.Y), LS);

			const float A = FMath::Clamp(Cell.Agitation, 0.f, 1.f);
			if (A > 0.001f) { FLinearColor C = HeatColor(A); C.A = 0.12f; Box(LAbstract, STL, SBR - STL, C); }

			const FLinearColor Out(0.45f, 0.45f, 0.60f, 0.7f);
			Line(LAbstract, STL, STR, Out, 1.f); Line(LAbstract, STR, SBR, Out, 1.f);
			Line(LAbstract, SBR, SBL, Out, 1.f); Line(LAbstract, SBL, STL, Out, 1.f);

			if (Cell.Population > 0)
				Text(LText, STL + FVector2D(4, 2), FString::Printf(TEXT("Pop %d"), Cell.Population), Font8, FLinearColor(0.85f, 0.85f, 0.95f));

			if (bShowArrows && !Cell.PressureDirection.IsNearlyZero())
			{
				const FVector2D Dir = Cell.PressureDirection.GetSafeNormal();
				const FVector2D CW = (Mn + Mx) * 0.5;
				const FVector2D CSc = WorldToScreen(CW, LS);
				const FVector2D Tip = WorldToScreen(CW + Dir * (CS * 0.4f), LS);
				Arrow(LArrow, CSc, Tip, FLinearColor(0.2f, 1.f, 0.2f, 0.9f), 2.f);
			}
		}
	}

	// ── Momentum field (fine) ──
	if (bShowAgitation)
	{
		const float CS = FMath::Max(1.f, Sub->MomentumCellSize);
		for (const TPair<FIntPoint, FATR_MomentumCell>& Pair : Sub->MomentumField)
		{
			const FIntPoint K = Pair.Key;
			const FATR_MomentumCell& Cell = Pair.Value;
			const float A = FMath::Clamp(Cell.Strength, 0.f, 1.f);
			if (A <= 0.001f) continue;

			const FVector2D Mn(K.X * CS, K.Y * CS);
			const FVector2D Mx = Mn + FVector2D(CS, CS);
			if (!Overlaps(Mn, Mx)) continue;

			const FVector2D STL = WorldToScreen(FVector2D(Mn.X, Mx.Y), LS);
			const FVector2D SBR = WorldToScreen(FVector2D(Mx.X, Mn.Y), LS);
			FLinearColor C = HeatColor(A); C.A = 0.22f + 0.5f * A;
			Box(LAgitation, STL, SBR - STL, C);

			if (bShowArrows)
			{
				// Sample the ACTUAL flow echoes follow (gradient of the diffused field), not the raw
				// deposited direction — so the arrows match how the horde moves.
				const FVector2D CW = (Mn + Mx) * 0.5;
				float FlowAgit = 0.f; FVector FlowDir = FVector::ZeroVector;
				Sub->SampleMomentumField(FVector(CW.X, CW.Y, 0.0), FlowAgit, FlowDir);
				if (!FlowDir.IsNearlyZero())
				{
					const FVector2D CSc = WorldToScreen(CW, LS);
					const FVector2D Tip = WorldToScreen(CW + FVector2D(FlowDir.X, FlowDir.Y) * (CS * 0.4f), LS);
					Arrow(LArrow, CSc, Tip, FLinearColor(0.5f, 1.f, 1.f, 0.95f), 1.6f);
				}
			}
		}
	}

	// ── Dense gradient flow field (independent sampling grid) ──
	// Samples the field's gradient on a regular grid across the visible area, so you see the smooth
	// flow echoes actually follow — including diffused regions with no deposit of their own.
	if (bShowFlowField)
	{
		const float CS = FMath::Max(1.f, Sub->MomentumCellSize);
		float Step = FMath::Max(CS, 28.f / FMath::Max(PixelsPerCm, 1e-5f)); // >= 1 cell, >= ~28px apart
		int32 NX = FMath::CeilToInt((VisMax.X - VisMin.X) / Step) + 1;
		int32 NY = FMath::CeilToInt((VisMax.Y - VisMin.Y) / Step) + 1;
		while ((int64)NX * NY > 2500 && Step < 1e7f) // cap total samples
		{
			Step *= 1.5f;
			NX = FMath::CeilToInt((VisMax.X - VisMin.X) / Step) + 1;
			NY = FMath::CeilToInt((VisMax.Y - VisMin.Y) / Step) + 1;
		}

		const float ArrowPx = FMath::Clamp(Step * PixelsPerCm * 0.42f, 6.f, 22.f);
		const float StartX  = FMath::FloorToFloat(VisMin.X / Step) * Step;
		const float StartY  = FMath::FloorToFloat(VisMin.Y / Step) * Step;

		for (float WY = StartY; WY <= VisMax.Y; WY += Step)
		for (float WX = StartX; WX <= VisMax.X; WX += Step)
		{
			float Agit = 0.f; FVector Dir = FVector::ZeroVector;
			Sub->SampleMomentumField(FVector(WX, WY, 0.0), Agit, Dir);
			if (Agit <= 0.02f || Dir.IsNearlyZero()) continue;

			const FVector2D S = WorldToScreen(FVector2D(WX, WY), LS);
			const FVector2D DirScreen = FVector2D(Dir.X, -Dir.Y).GetSafeNormal();
			FLinearColor C = HeatColor(FMath::Clamp(Agit, 0.f, 1.f));
			C.A = 0.5f + 0.5f * Agit;
			Arrow(LArrow, S, S + DirScreen * ArrowPx, C, 1.3f);
		}
	}

	// Marker radius scales with zoom (so dots grow when you zoom in), clamped to stay visible.
	const float DotR    = FMath::Clamp(GEchoAgentDiameterCm * 0.5f * PixelsPerCm, 2.0f, 14.0f);
	const float MoveLen = FMath::Max(DotR * 3.0f, 14.0f);

	// ── Echo target lines (drawn under the dots) ──
	if (bShowTargetLines)
	{
		const int32 N = FMath::Min(Sub->ActiveEntities, Sub->Positions.Num());
		for (int32 i = 0; i < N; ++i)
		{
			const FVector3f& P = Sub->Positions[i];
			const FVector2D W(P.X, P.Y);
			if (!InView(W) || !Sub->RuntimeStates.IsValidIndex(i)) continue;

			const FATR_EchoRuntimeState& RS = Sub->RuntimeStates[i];
			const FATR_EchoMoveRequest& MR = RS.Movement.Request;

			FVector2D TgtW; bool bHasTarget = false; bool bSees = false;
			if (MR.Type == EATR_EchoMoveTargetType::Actor)
			{
				if (const AActor* A = MR.Actor.Get())
				{
					const FVector L = A->GetActorLocation();
					TgtW = FVector2D(L.X, L.Y); bHasTarget = true;
					bSees = RS.Awareness.bHasCurrentLineOfSight;
				}
			}
			else if (MR.Type == EATR_EchoMoveTargetType::Location)
			{
				TgtW = FVector2D(MR.Location.X, MR.Location.Y); bHasTarget = true;
			}
			if (!bHasTarget) continue;

			const FVector2D S  = WorldToScreen(W, LS);
			const FVector2D Ts = WorldToScreen(TgtW, LS);
			const FLinearColor C = bSees ? FLinearColor(1.0f, 0.22f, 0.18f, 0.85f)   // sees player → red
			                             : FLinearColor(1.0f, 0.62f, 0.12f, 0.65f);  // remembered/heard loc → orange
			Line(LTarget, S, Ts, C, 1.2f);
			Box(LTarget, Ts - FVector2D(2, 2), FVector2D(4, 4), C); // small target marker
		}
	}

	// ── Echoes (dots, optional movement-direction arrows) ──
	int32 DrawnEchoes = 0;
	if (bShowEchoes)
	{
		const int32 N = FMath::Min(Sub->ActiveEntities, Sub->Positions.Num());
		for (int32 i = 0; i < N; ++i)
		{
			const FVector3f& P = Sub->Positions[i];
			const FVector2D W(P.X, P.Y);
			if (!InView(W)) continue;

			FLinearColor C = FLinearColor::White;
			if (bShowIntentColors && Sub->RuntimeStates.IsValidIndex(i))
				C = IntentColor((uint8)Sub->RuntimeStates[i].Intent);

			const FVector2D S = WorldToScreen(W, LS);
			Box(LEcho, S - FVector2D(DotR, DotR), FVector2D(DotR * 2.f, DotR * 2.f), C);
			++DrawnEchoes;

			if (bShowMoveArrows && Sub->Velocities.IsValidIndex(i))
			{
				// Only draw when genuinely moving (speed > ~5 cm/s). No yaw/facing fallback —
				// a stationary echo (idle / no target) shows no arrow instead of pointing at its
				// default facing direction.
				const FVector3f& V = Sub->Velocities[i];
				if (V.X * V.X + V.Y * V.Y > 25.f)
				{
					const FVector2D Dir = FVector2D(V.X, V.Y).GetSafeNormal();
					const FVector2D DirScreen(Dir.X, -Dir.Y); // +Y world = up
					Arrow(LArrow, S, S + DirScreen * MoveLen, FLinearColor(0.95f, 0.95f, 1.0f, 0.9f), 1.5f);
				}
			}
		}
	}

	// ── Players (distinct cyan triangles, oriented by facing) ──
	if (bShowPlayers)
	{
		if (UWorld* World = Sub->GetWorld())
		{
			const float PR = FMath::Clamp(GPlayerSizeCm * 0.5f * PixelsPerCm, 9.f, 26.f);
			auto Marker = [&](const FVector2D& C, const FVector2D& Facing, float Size, const FLinearColor& Col, float Th)
			{
				FVector2D F = Facing.GetSafeNormal();
				if (F.IsNearlyZero()) F = FVector2D(0, -1);
				const FVector2D Perp(-F.Y, F.X);
				const FVector2D Tip = C + F * Size;
				const FVector2D B1  = C - F * (Size * 0.7f) + Perp * (Size * 0.75f);
				const FVector2D B2  = C - F * (Size * 0.7f) - Perp * (Size * 0.75f);
				Line(LPlayer, Tip, B1, Col, Th);
				Line(LPlayer, B1, B2, Col, Th);
				Line(LPlayer, B2, Tip, Col, Th);
			};

			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* PC = It->Get();
				const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
				if (!Pawn) continue;

				const FVector L = Pawn->GetActorLocation();
				const FVector2D W(L.X, L.Y);
				const FVector2D S = WorldToScreen(W, LS);
				const float Yr = FMath::DegreesToRadians((float)Pawn->GetActorRotation().Yaw);
				const FVector2D FacingScreen(FMath::Cos(Yr), -FMath::Sin(Yr));

				Marker(S, FacingScreen, PR + 1.5f, FLinearColor(0.02f, 0.02f, 0.04f, 0.95f), 4.0f); // dark backing
				Marker(S, FacingScreen, PR,        FLinearColor(0.20f, 0.95f, 1.00f, 1.00f), 2.5f); // bright cyan
			}
		}
	}

	// ── Sound emission pulses (click-to-emit feedback) ──
	{
		const double NowT = FPlatformTime::Seconds();
		const double Life = 1.6;
		for (int32 i = RecentEmits.Num() - 1; i >= 0; --i)
		{
			const double Age = NowT - RecentEmits[i].Time;
			if (Age > Life) { RecentEmits.RemoveAt(i); continue; }
			const float Tt  = (float)(Age / Life);
			const FVector2D Cs = WorldToScreen(RecentEmits[i].World, LS);
			const float Rpx = RecentEmits[i].Radius * Tt * PixelsPerCm;
			const FLinearColor C(1.f, 0.8f, 0.15f, 1.f - Tt);
			const int32 Seg = 28;
			TArray<FVector2D> Pts; Pts.Reserve(Seg + 1);
			for (int32 s = 0; s <= Seg; ++s) { const float A = (float)s / Seg * 2.f * PI; Pts.Add(Cs + FVector2D(FMath::Cos(A), FMath::Sin(A)) * Rpx); }
			FSlateDrawElement::MakeLines(OutDrawElements, LPlayer, AllottedGeometry.ToPaintGeometry(), Pts, ESlateDrawEffect::None, C, true, 2.f);
			Box(LPlayer, Cs - FVector2D(3.f, 3.f), FVector2D(6.f, 6.f), FLinearColor(1.f, 0.8f, 0.15f, 1.f - Tt));
		}
	}

	if (bEmitSoundMode)
		Text(LText, FVector2D(10, LS.Y - 40),
			FString::Printf(TEXT("EMIT SOUND: click map to drop  [type %d  strength %.2f  radius %.0f]"),
				(int32)SoundType, SoundStrength, SoundRadius),
			Font10, FLinearColor(1.f, 0.85f, 0.25f));

	// ── HUD stat line ──
	{
		const float CmPerPixel = (PixelsPerCm > KINDA_SMALL_NUMBER) ? 1.f / PixelsPerCm : 0.f;
		Text(LText, FVector2D(10, LS.Y - 24),
			FString::Printf(TEXT("Zoom: 1px = %.0f cm   |   Echoes drawn: %d / %d   |   Momentum cells: %d   |   Abstract cells: %d   |   Wheel = zoom, Drag = pan"),
				CmPerPixel, DrawnEchoes, Sub->ActiveEntities, Sub->MomentumField.Num(), Sub->AbstractCells.Num()),
			Font10, FLinearColor(0.85f, 0.85f, 0.9f));
	}

	// ── Legend panel (top-right) ──
	{
		const float PanelW = 232.f;
		float X = LS.X - PanelW - 10.f;
		float Y = 10.f;
		const float Pad = 8.f;
		const float Row = 18.f;
		const float SwatchSize = 11.f;

		// Count rows to size the backing panel.
		int32 Rows = 1; // title
		if (bShowAgitation || bShowAbstractCells) Rows += 2;          // heat ramp + label
		if (bShowArrows || bShowFlowField)        Rows += 1;          // flow arrows note
		if (bShowTargetLines)                     Rows += 2;          // sees / remembered
		if (bShowPlayers)                         Rows += 1;
		if (bShowMoveArrows)                      Rows += 1;
		if (bShowIntentColors)                    Rows += 7;          // intent swatches
		const float PanelH = Pad * 2.f + Rows * Row;

		Box(LText, FVector2D(X, Y), FVector2D(PanelW, PanelH), FLinearColor(0.03f, 0.03f, 0.05f, 0.85f));
		Line(LText, FVector2D(X, Y), FVector2D(X + PanelW, Y), FLinearColor(0.4f, 0.4f, 0.5f, 0.8f), 1.f);

		float Cx = X + Pad;
		float Cy = Y + Pad;
		auto Swatch = [&](const FLinearColor& Col, const FString& Label)
		{
			Box(LText, FVector2D(Cx, Cy + 2.f), FVector2D(SwatchSize, SwatchSize), Col);
			Text(LText, FVector2D(Cx + SwatchSize + 6.f, Cy), Label, Font8, FLinearColor(0.88f, 0.88f, 0.94f));
			Cy += Row;
		};

		Text(LText, FVector2D(Cx, Cy), TEXT("LEGEND"), Font10, FLinearColor(0.95f, 0.95f, 1.0f));
		Cy += Row;

		if (bShowAgitation || bShowAbstractCells)
		{
			// Heat ramp bar: blue (low pressure) → red (high pressure).
			const float BarW = PanelW - Pad * 2.f - 4.f;
			const int32 Segs = 32;
			const float Sw = BarW / Segs;
			for (int32 k = 0; k < Segs; ++k)
				Box(LText, FVector2D(Cx + k * Sw, Cy + 2.f), FVector2D(Sw + 1.f, SwatchSize), HeatColor((float)k / (Segs - 1)));
			Cy += Row;
			Text(LText, FVector2D(Cx, Cy), TEXT("momentum:  weak  ->  strong"), Font8, FLinearColor(0.88f, 0.88f, 0.94f));
			Cy += Row;
		}

		if (bShowArrows || bShowFlowField)
			Swatch(FLinearColor(0.5f, 1.0f, 1.0f), TEXT("flow: horde momentum"));

		if (bShowTargetLines)
		{
			Swatch(FLinearColor(1.0f, 0.22f, 0.18f), TEXT("target line: sees player"));
			Swatch(FLinearColor(1.0f, 0.62f, 0.12f), TEXT("target line: remembered loc"));
		}
		if (bShowPlayers)   Swatch(FLinearColor(0.20f, 0.95f, 1.00f), TEXT("player"));
		if (bShowMoveArrows) Swatch(FLinearColor(0.95f, 0.95f, 1.00f), TEXT("echo move direction"));

		if (bShowIntentColors)
		{
			Swatch(IntentColor(1),  TEXT("Idle / ReturnToIdle"));
			Swatch(IntentColor(2),  TEXT("Wander"));
			Swatch(IntentColor(4),  TEXT("Investigate / heard"));
			Swatch(IntentColor(5),  TEXT("Chase (sees)"));
			Swatch(IntentColor(6),  TEXT("Chase last-seen / search"));
			Swatch(IntentColor(9),  TEXT("Join horde pressure"));
			Swatch(IntentColor(11), TEXT("Handle obstacle"));
		}
	}

	return LText;
}

FReply SATR_EchoDebugMapView::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const FVector2D LS    = MyGeometry.GetLocalSize();
	const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const FVector2D WorldUnder = ScreenToWorld(Local, LS);

	const float Factor = (MouseEvent.GetWheelDelta() > 0.f) ? 1.15f : (1.f / 1.15f);
	PixelsPerCm = FMath::Clamp(PixelsPerCm * Factor, 1e-5f, 2.f);
	bViewInitialized = true;

	// Keep the world point under the cursor pinned in place after zooming.
	ViewCenterWorld.X = WorldUnder.X - (Local.X - LS.X * 0.5) / PixelsPerCm;
	ViewCenterWorld.Y = WorldUnder.Y + (Local.Y - LS.Y * 0.5) / PixelsPerCm;
	return FReply::Handled();
}

void SATR_EchoDebugMapView::EmitSoundAtWorld(const FVector2D& WorldXY)
{
	UATR_EchoSubsystem* Sub = GetSubsystem();
	if (!Sub) return;

	UWorld* W = Sub->GetWorld();

	// Place the stimulus at the local player's Z so 3D distance falloff matches the echoes' height.
	double Z = 0.0;
	if (W)
		if (APlayerController* PC = W->GetFirstPlayerController())
			if (APawn* P = PC->GetPawn())
				Z = P->GetActorLocation().Z;

	FATR_StimulusEvent E;
	E.Type        = static_cast<EATR_StimulusType>(SoundType);
	E.Location    = FVector(WorldXY.X, WorldXY.Y, Z);
	E.Strength    = SoundStrength;
	E.Radius      = SoundRadius;
	E.Direction   = FVector::ZeroVector;
	E.TimeSeconds = W ? W->GetTimeSeconds() : 0.f;

	Sub->EmitWorldStimulus(E);
	RecentEmits.Add({ WorldXY, FPlatformTime::Seconds(), SoundRadius });
}

FReply SATR_EchoDebugMapView::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	// Emit-sound mode: left-click drops a stimulus at the clicked world point (no pan).
	if (bEmitSoundMode && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		const FVector2D LS    = MyGeometry.GetLocalSize();
		const FVector2D Local = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
		EmitSoundAtWorld(ScreenToWorld(Local, LS));
		return FReply::Handled();
	}

	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton || MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		bPanning = true;
		LastPanScreenPos = MouseEvent.GetScreenSpacePosition();
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}
	return FReply::Unhandled();
}

FReply SATR_EchoDebugMapView::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bPanning)
	{
		bPanning = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FReply SATR_EchoDebugMapView::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (!bPanning)
		return FReply::Unhandled();

	const FVector2D Cur = MouseEvent.GetScreenSpacePosition();
	const FVector2D DeltaPx = Cur - LastPanScreenPos;
	LastPanScreenPos = Cur;

	const float Inv = (PixelsPerCm > KINDA_SMALL_NUMBER) ? 1.f / PixelsPerCm : 0.f;
	ViewCenterWorld.X -= DeltaPx.X * Inv;
	ViewCenterWorld.Y += DeltaPx.Y * Inv; // screen-down drag reveals world below
	return FReply::Handled();
}
  