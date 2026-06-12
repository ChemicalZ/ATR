// Fill out your copyright notice in the Description page of Project Settings.

#include "ATR_EchoDebugMapSubsystem.h"
#include "SATR_EchoDebugMapWidget.h"
#include "../ATR_EchoSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "HAL/IConsoleManager.h"

namespace
{
	const TCHAR* GEchoDebugMapCommand = TEXT("ATR.EchoDebugMap");
}

// ── Global F8 listener. Returns false so the key still propagates normally. ──
class FATR_EchoDebugMapInputProcessor : public IInputProcessor
{
public:
	explicit FATR_EchoDebugMapInputProcessor(UATR_EchoDebugMapSubsystem* InOwner) : Owner(InOwner) {}

	virtual void Tick(const float /*DeltaTime*/, FSlateApplication& /*SlateApp*/, TSharedRef<ICursor> /*Cursor*/) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& /*SlateApp*/, const FKeyEvent& InKeyEvent) override
	{
		if (InKeyEvent.GetKey() == EKeys::F8 && Owner.IsValid())
		{
			Owner->ToggleMap();
		}
		return false; // never consume
	}

	virtual const TCHAR* GetDebugName() const override { return TEXT("ATR_EchoDebugMap"); }

private:
	TWeakObjectPtr<UATR_EchoDebugMapSubsystem> Owner;
};

bool UATR_EchoDebugMapSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false; // debug-only tool
#else
	return Super::ShouldCreateSubsystem(Outer);
#endif
}

void UATR_EchoDebugMapSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Register the console command once (the first game instance to init wins; PIE-with-multiple
	// instances won't double-register and assert). Routed back to the invoking world's instance.
	if (!IConsoleManager::Get().FindConsoleObject(GEchoDebugMapCommand))
	{
		ToggleCommand = IConsoleManager::Get().RegisterConsoleCommand(
			GEchoDebugMapCommand,
			TEXT("Toggle the Echo debug map overlay (echo positions + horde agitation/pressure field)."),
			FConsoleCommandWithWorldDelegate::CreateStatic(&UATR_EchoDebugMapSubsystem::ToggleFromWorld),
			ECVF_Cheat);
	}

	// Global F8 toggle.
	if (FSlateApplication::IsInitialized())
	{
		InputProcessor = MakeShared<FATR_EchoDebugMapInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
	}
}

void UATR_EchoDebugMapSubsystem::Deinitialize()
{
	CloseMap();

	if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
	InputProcessor.Reset();

	if (ToggleCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ToggleCommand);
		ToggleCommand = nullptr;
	}

	Super::Deinitialize();
}

void UATR_EchoDebugMapSubsystem::ToggleFromWorld(UWorld* World)
{
	if (!World) return;
	if (UGameInstance* GI = World->GetGameInstance())
	{
		if (UATR_EchoDebugMapSubsystem* Self = GI->GetSubsystem<UATR_EchoDebugMapSubsystem>())
		{
			Self->ToggleMap();
		}
	}
}

void UATR_EchoDebugMapSubsystem::ToggleMap()
{
	if (IsMapOpen()) CloseMap();
	else             OpenMap();
}

void UATR_EchoDebugMapSubsystem::OpenMap()
{
	if (MapWidget.IsValid())
		return;

	UGameInstance* GI = GetGameInstance();
	UGameViewportClient* Viewport = GI ? GI->GetGameViewportClient() : nullptr;
	if (!Viewport)
		return;

	UWorld* World = GI->GetWorld();
	UATR_EchoSubsystem* EchoSub = World ? World->GetSubsystem<UATR_EchoSubsystem>() : nullptr;

	SAssignNew(MapWidget, SATR_EchoDebugMapWidget)
		.OnCloseRequested(FSimpleDelegate::CreateUObject(this, &UATR_EchoDebugMapSubsystem::CloseMap));
	MapWidget->SetSubsystem(EchoSub);

	Viewport->AddViewportWidgetContent(MapWidget.ToSharedRef(), /*ZOrder*/ 1000);

	// Show the cursor and let the overlay receive mouse/keys while gameplay keeps running.
	if (APlayerController* PC = GI->GetFirstLocalPlayerController())
	{
		PC->SetShowMouseCursor(true);
		FInputModeGameAndUI Mode;
		Mode.SetWidgetToFocus(MapWidget);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PC->SetInputMode(Mode);
	}
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().SetKeyboardFocus(MapWidget, EFocusCause::SetDirectly);
	}
}

void UATR_EchoDebugMapSubsystem::CloseMap()
{
	if (!MapWidget.IsValid())
		return;

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UGameViewportClient* Viewport = GI->GetGameViewportClient())
		{
			Viewport->RemoveViewportWidgetContent(MapWidget.ToSharedRef());
		}
		if (APlayerController* PC = GI->GetFirstLocalPlayerController())
		{
			FInputModeGameOnly Mode;
			PC->SetInputMode(Mode);
			PC->SetShowMouseCursor(false);
		}
	}

	MapWidget.Reset();
}
