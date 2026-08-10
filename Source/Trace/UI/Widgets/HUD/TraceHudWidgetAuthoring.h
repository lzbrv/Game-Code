// Trace — the three primitives Scripts/generate-hud-widgets.py cannot get from Python. Spec v17 §4.
//
// ---------------------------------------------------------------------------------------------
// WHY THIS FILE EXISTS AT ALL — MEASURED, NOT ASSUMED
// ---------------------------------------------------------------------------------------------
// Spec v17 §4 says "generate the WBP_ assets from Python". UE 5.8's Python API can create a Widget
// Blueprint (`unreal.WidgetBlueprintFactory` exists and works), can set every property on every
// widget and every slot, and can compile and save the asset. It CANNOT reach the widget TREE:
//
//     unreal.WidgetBlueprint.get_editor_property('widget_tree')
//         -> Exception: Failed to find property 'widget_tree' ... on 'WidgetBlueprint'
//     dir(unreal.WidgetTree)
//         -> no 'root_widget', no 'find_widget', no properties at all
//
// Both checked headlessly against this engine install, not inferred from documentation.
// `UWidgetBlueprint::WidgetTree` and `UWidgetTree::RootWidget` are bare `UPROPERTY()`s with no
// editor or Blueprint visibility, and the Python bindings only surface properties that have one.
// So there is no way, from Python alone, to put a single widget into a new asset.
//
// These three functions are that missing reach, and nothing more. They are deliberately DUMB —
// construct, parent, clear — so that WHAT the tree contains stays in the generator script where a
// human can read it as a layout, rather than migrating back into C++ where spec v17 §4 does not
// want it.
//
// ---------------------------------------------------------------------------------------------
// WHY IT IS REFLECTION AND NOT AN #include
// ---------------------------------------------------------------------------------------------
// `UWidgetBlueprint` lives in the UMGEditor module, which this module does not depend on and MUST
// NOT: the user added UMG/Slate/SlateCore to Trace.Build.cs and that file is explicitly off limits
// this pass. So the blueprint arrives here as a plain UObject* and its WidgetTree is found with
// FindFProperty. UWidgetTree and every UWidget subclass are in the UMG RUNTIME module, which is a
// dependency, so everything after that first hop is ordinary typed C++.
//
// The cost of reflection is that a renamed engine property becomes a runtime failure instead of a
// compile error — so every function here reports what it could not find, by name, and the generator
// aborts loudly rather than writing half an asset.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Templates/SubclassOf.h"

#include "TraceHudWidgetAuthoring.generated.h"

class UWidget;
class UWidgetTree;

UCLASS()
class TRACE_API UTraceHudWidgetAuthoring : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * The UWidgetTree inside @p WidgetBlueprintAsset, or null with an explanation in the log.
	 *
	 * @param WidgetBlueprintAsset a UWidgetBlueprint. Typed as UObject because UMGEditor is not, and
	 *                             must not become, a dependency of this module.
	 */
	UFUNCTION(BlueprintCallable, Category = "Trace|HUD Authoring")
	static UWidgetTree* GetWidgetTree(UObject* WidgetBlueprintAsset);

	/**
	 * Constructs one widget INTO the blueprint's tree and parents it.
	 *
	 * @param WidgetName   *** THE BINDING CONTRACT. *** UUserWidget resolves a BindWidget property by
	 *                     looking up this exact name in the tree, so a typo here is a widget that
	 *                     compiles and then binds to nothing. The generator's manifest and the C++
	 *                     RequiredWidgetNames() lists are two independent statements of these names.
	 * @param ParentWidget null makes this the tree's ROOT.
	 *
	 * Every widget it creates is marked `bIsVariable`, which is what puts it in the Blueprint's
	 * variable list and is what the Widget Blueprint compiler expects of a bound widget.
	 */
	UFUNCTION(BlueprintCallable, Category = "Trace|HUD Authoring")
	static UWidget* AddWidget(UObject* WidgetBlueprintAsset, TSubclassOf<UWidget> WidgetClass,
		FName WidgetName, UWidget* ParentWidget);

	/**
	 * Empties the tree so a regenerate is a full REWRITE and not a merge.
	 *
	 * The old widgets are renamed out into the transient package rather than merely dropped, and that
	 * is load-bearing: a dead widget still holding the name "CountText" inside the same package makes
	 * the very next NewObject of that name fail, so a second run of the generator would silently
	 * produce differently-named widgets and every BindWidget would come back null.
	 */
	UFUNCTION(BlueprintCallable, Category = "Trace|HUD Authoring")
	static bool ClearWidgetTree(UObject* WidgetBlueprintAsset);

	/**
	 * The widget named @p WidgetName in the blueprint's tree, or null.
	 *
	 * *** IT IS THE SAME LOOKUP BindWidget ITSELF PERFORMS. *** The generator verifies its own output
	 * with this rather than by walking the tree it just built, so the check asks the question the
	 * runtime asks — "is there a widget called CountText in here" — instead of the question the
	 * generator already knows the answer to.
	 */
	UFUNCTION(BlueprintCallable, Category = "Trace|HUD Authoring")
	static UWidget* FindWidget(UObject* WidgetBlueprintAsset, FName WidgetName);
};
