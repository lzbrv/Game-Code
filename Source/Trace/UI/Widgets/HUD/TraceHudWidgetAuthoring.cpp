// Trace — the three primitives Scripts/generate-hud-widgets.py cannot get from Python. Spec v17 §4.

#include "UI/Widgets/HUD/TraceHudWidgetAuthoring.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Trace.h"                       // LogTraceGame
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

/** Named after the file, per the build contract's jumbo rule (spec v17 §0). */
namespace TraceHudWidgetAuthoringFile
{
	/** The engine's own name for the property. If UE ever renames it, this is the one line to change. */
	static const TCHAR* WidgetTreePropertyName = TEXT("WidgetTree");
}

UWidgetTree* UTraceHudWidgetAuthoring::GetWidgetTree(UObject* WidgetBlueprintAsset)
{
	if (WidgetBlueprintAsset == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[HudAuthoring] GetWidgetTree: null asset."));
		return nullptr;
	}

	FObjectProperty* TreeProperty = FindFProperty<FObjectProperty>(
		WidgetBlueprintAsset->GetClass(), TraceHudWidgetAuthoringFile::WidgetTreePropertyName);

	if (TreeProperty == nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[HudAuthoring] %s (class %s) has no object property named '%s'. Either this is not a "
			     "Widget Blueprint, or the engine renamed the property - see TraceHudWidgetAuthoring.h."),
			*WidgetBlueprintAsset->GetName(), *WidgetBlueprintAsset->GetClass()->GetName(),
			TraceHudWidgetAuthoringFile::WidgetTreePropertyName);
		return nullptr;
	}

	UObject* TreeObject = TreeProperty->GetObjectPropertyValue_InContainer(WidgetBlueprintAsset);
	UWidgetTree* Tree = Cast<UWidgetTree>(TreeObject);

	if (Tree == nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[HudAuthoring] %s's WidgetTree is %s, not a UWidgetTree."),
			*WidgetBlueprintAsset->GetName(), *GetNameSafe(TreeObject));
	}
	return Tree;
}

UWidget* UTraceHudWidgetAuthoring::AddWidget(UObject* WidgetBlueprintAsset,
	TSubclassOf<UWidget> WidgetClass, FName WidgetName, UWidget* ParentWidget)
{
	UWidgetTree* Tree = GetWidgetTree(WidgetBlueprintAsset);
	if (Tree == nullptr || WidgetClass == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[HudAuthoring] AddWidget('%s'): no tree, or no class."),
			*WidgetName.ToString());
		return nullptr;
	}

	// A name already taken inside this package makes the construct below fail or silently rename, and
	// a renamed widget is a BindWidget that resolves to null. Caught here rather than diagnosed later.
	if (StaticFindObject(nullptr, Tree, *WidgetName.ToString()) != nullptr)
	{
		UE_LOG(LogTraceGame, Error,
			TEXT("[HudAuthoring] AddWidget: '%s' already exists in this tree. Call ClearWidgetTree "
			     "first - a regenerate must be a rewrite, not a merge."),
			*WidgetName.ToString());
		return nullptr;
	}

	UWidget* NewWidget = NewObject<UWidget>(Tree, WidgetClass, WidgetName, RF_Transactional);
	if (NewWidget == nullptr)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[HudAuthoring] AddWidget: NewObject failed for '%s' (%s)."),
			*WidgetName.ToString(), *WidgetClass->GetName());
		return nullptr;
	}

	// What puts it in the Blueprint's variable list, and what the Widget Blueprint compiler expects of
	// anything a BindWidget property is going to bind to.
	NewWidget->bIsVariable = true;

	if (ParentWidget == nullptr)
	{
		Tree->RootWidget = NewWidget;
	}
	else
	{
		UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (ParentPanel == nullptr)
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[HudAuthoring] AddWidget: '%s' cannot be parented to '%s', which is not a panel."),
				*WidgetName.ToString(), *ParentWidget->GetName());
			return nullptr;
		}

		if (ParentPanel->AddChild(NewWidget) == nullptr)
		{
			// A Border, a SizeBox and every other UContentWidget hold exactly ONE child, so this is the
			// error a layout bug produces first.
			UE_LOG(LogTraceGame, Error,
				TEXT("[HudAuthoring] AddWidget: '%s' refused the child '%s' (a content widget already "
				     "holding one, or a panel that takes none)."),
				*ParentPanel->GetName(), *WidgetName.ToString());
			return nullptr;
		}
	}

	Tree->Modify();
	return NewWidget;
}

bool UTraceHudWidgetAuthoring::ClearWidgetTree(UObject* WidgetBlueprintAsset)
{
	UWidgetTree* Tree = GetWidgetTree(WidgetBlueprintAsset);
	if (Tree == nullptr)
	{
		return false;
	}

	Tree->Modify();
	Tree->RootWidget = nullptr;

	// Everything ever constructed into this tree, not just what is currently reachable from the root:
	// a previous run that failed halfway leaves orphans, and an orphan holding a name is exactly as
	// fatal to the next NewObject as a live widget holding it.
	TArray<UObject*> Residents;
	GetObjectsWithOuter(Tree, Residents, /*bIncludeNestedObjects=*/true);

	int32 Evicted = 0;
	for (UObject* Resident : Residents)
	{
		if (Resident == nullptr || Resident->IsA<UWidgetTree>())
		{
			continue;
		}

		// Renamed OUT of the package rather than left for the garbage collector: the name has to be
		// free again before this same run reuses it, and GC will not have run by then.
		//
		// Deliberately NOT passing REN_ForceNoResetLoaders: 5.8 deprecates it with "Rename will no
		// longer call ResetLoaders making this flag no longer needed ... otherwise your project will no
		// longer compile", and the contract asks us to avoid anything deprecated on the way past.
		Resident->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_DoNotDirty);
		++Evicted;
	}

	UE_LOG(LogTraceGame, Display, TEXT("[HudAuthoring] cleared %s: %d object(s) evicted from the tree."),
		*WidgetBlueprintAsset->GetName(), Evicted);
	return true;
}

UWidget* UTraceHudWidgetAuthoring::FindWidget(UObject* WidgetBlueprintAsset, FName WidgetName)
{
	UWidgetTree* Tree = GetWidgetTree(WidgetBlueprintAsset);
	return (Tree != nullptr) ? Tree->FindWidget(WidgetName) : nullptr;
}
