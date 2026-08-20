// Trace — the integration walk (spec v22, integration pass).
//
// =================================================================================================
// WHY THIS EXISTS
// =================================================================================================
// The integration brief for spec v22 is "walk every screen yourself at 1920x1080, with no
// command-line flags, as a player would: title -> settings -> back -> JOIN prompt -> start a match
// -> character select -> in match -> escape -> settings. Look at every frame."
//
// The harness this project already has cannot do that. `-TraceExec` fires ALL of its commands in
// one timer callback, in the same frame (UI/TraceAutoShot.cpp, "one timer, not one per command"), so
// it can open a screen but it cannot open one, photograph it, leave it and photograph the next. The
// alternative — one editor launch per screen — cannot photograph a TRANSITION at all, and "does the
// renderer change when the modal opens" (spec §A3) is a question about transitions.
//
// So this is a scripted walk on a timer: a list of (when, what) steps that presses REAL KEYS through
// Trace.SimInput — the same injection path the ability harnesses use — and requests a screenshot at
// each stop. Nothing here calls a menu handler directly. If a key is unbound, or the modal eats the
// escape, or a row moved, the walk photographs that instead of hiding it.
//
// =================================================================================================
// USING IT
// =================================================================================================
//     Trace.Integ.Walk Menu     the title screen tour: title, settings, back, JOIN, back
//     Trace.Integ.Walk Match    in match: play, escape, the in-match settings page, back
//
// Frames land in Saved/Screenshots as v22integ_<NN>_<what>.png, so the file name says which step of
// which walk produced it and no frame has to be identified by its timestamp. The log carries the
// same names, which is what lets a frame be attributed to a run when more than one editor is up.

#include "CoreMinimal.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Containers/Ticker.h"   // FTSTicker — real time, so a paused world cannot strand the walk
#include "TimerManager.h"
#include "UnrealClient.h"

#include "Trace.h"   // LogTraceGame

#if !UE_BUILD_SHIPPING

// Named after the file, never anonymous — this module is compiled as a unity build and
// Scripts/check-jumbo-build-collisions.py gates on it. "FStep" is exactly the kind of name a second
// harness file would also want.
namespace TraceIntegrationWalkFile
{
	/** One stop on the walk. Either a key press or a photograph; never both, so the log reads in order. */
	struct FStep
	{
		/** Seconds after the walk starts. */
		float At = 0.f;

		/** A key to press through the real input pipeline, or empty for a photograph. */
		FString Key;

		/** What the frame is called, when this step is a photograph. */
		FString Shot;

		/**
		 * A console command to run at this stop, when the step is neither a key nor a photograph.
		 *
		 * ADDED FOR THE v30 GUN WALK, and the reason is that a photograph of a first-person weapon is
		 * ambiguous evidence on its own: two guns drawn at the same place in the frame look similar at
		 * a glance, and "the cyan is brighter" is not a number. The reports this fires (Trace.
		 * ViewModel.Guns, Trace.Smg.Probe) print the rig, the slots and the live EmissiveIntensity
		 * into the same log, timestamped a fraction of a second from the frame they explain — so a
		 * frame can be attributed to a measured state rather than to a description of one.
		 */
		FString Exec;

		/**
		 * How long the key is held, in seconds, and WHICH route it takes.
		 *
		 * The menu walks all press through "viewport" for 0.12 s, which is where the menu's key
		 * handling lives. Gameplay is a different pipeline and a different rhythm: a dash is a tap,
		 * a burst of fire is a third of a second, and moving anywhere needs a hold of over a second.
		 * "controller" is APlayerController::InputKey, the entry point Enhanced Input's gameplay
		 * mappings are bound behind — the same one the ability harnesses in this project use.
		 */
		float Hold = 0.12f;
		FString Route = TEXT("viewport");
	};

	static FStep Press(float At, const TCHAR* Key)
	{
		FStep Step;
		Step.At = At;
		Step.Key = Key;
		return Step;
	}

	/** A gameplay press: held for as long as the action needs, through the controller route. */
	static FStep Play(float At, const TCHAR* Key, float Hold)
	{
		FStep Step;
		Step.At = At;
		Step.Key = Key;
		Step.Hold = Hold;
		Step.Route = TEXT("controller");
		return Step;
	}

	static FStep Shoot(float At, const TCHAR* Name)
	{
		FStep Step;
		Step.At = At;
		Step.Shot = Name;
		return Step;
	}

	/** A console report, scheduled so it lands beside the frame it explains. */
	static FStep Say(float At, const TCHAR* Command)
	{
		FStep Step;
		Step.At = At;
		Step.Exec = Command;
		return Step;
	}

	/**
	 * THE TITLE SCREEN TOUR.
	 *
	 * The row order is ETraceMenuRow's: PLAY, JOIN, PRACTICE, DIFFICULTY, SCORING MODE, SETTINGS,
	 * QUIT. Five Downs from the top lands on SETTINGS and four Ups from there lands on JOIN, so the
	 * walk never types a row NUMBER — it moves the way a player moves and photographs where it got
	 * to. If a row is ever inserted, the frames show the wrong screen rather than silently passing.
	 *
	 * The gaps are 0.45 s, comfortably more than one frame at any rate this project runs at, because
	 * the menu reads its keys once per frame and two presses inside one frame are one press.
	 */
	static TArray<FStep> MenuWalk()
	{
		TArray<FStep> Steps;
		Steps.Add(Shoot(0.60f, TEXT("01_title")));

		float T = 1.40f;
		for (int32 I = 0; I < 5; ++I, T += 0.45f)
		{
			Steps.Add(Press(T, TEXT("Down")));
		}
		Steps.Add(Shoot(T + 0.50f, TEXT("02_settings_row")));

		T += 1.10f;
		Steps.Add(Press(T, TEXT("Enter")));
		Steps.Add(Shoot(T + 1.40f, TEXT("03_settings_open")));

		T += 2.60f;
		Steps.Add(Press(T, TEXT("Escape")));
		Steps.Add(Shoot(T + 1.40f, TEXT("04_back_at_title")));

		T += 2.60f;
		for (int32 I = 0; I < 4; ++I, T += 0.45f)
		{
			Steps.Add(Press(T, TEXT("Up")));
		}
		Steps.Add(Shoot(T + 0.50f, TEXT("05_join_row")));

		T += 1.10f;
		Steps.Add(Press(T, TEXT("Enter")));
		Steps.Add(Shoot(T + 1.40f, TEXT("06_join_prompt")));

		T += 2.60f;
		Steps.Add(Press(T, TEXT("Escape")));
		Steps.Add(Shoot(T + 1.40f, TEXT("07_title_again")));

		// SPEC v23 §A1 — THE GRID MUST STILL BE MOVING WHEN THE WALK COMES BACK.
		//
		// A second title frame taken seconds after 07 is what turns "the backdrop has a grid on it"
		// into "the grid scrolls", from the same run, on the same screen the player is looking at.
		// Two frames of an animated floor differ; two frames of a wallpaper do not. It is deliberately
		// taken AFTER the settings and JOIN round trip rather than next to 01, so it also answers the
		// second question — whether the grid survives a modal opening and closing over it.
		T += 4.00f;
		Steps.Add(Shoot(T, TEXT("08_title_grid_later")));

		return Steps;
	}

	/**
	 * THE CHARACTER SELECT SCREEN — spec v23 §A3's deliverable, and the one screen the v22 walk
	 * never visited.
	 *
	 * The owner asked for the character NAMES to stay bold while everything else went extra light,
	 * so the select screen is where the two weights appear side by side: the tile names and the
	 * identity panel's name are Bold, and the ability prose under them is Light. Both frames are
	 * needed — a tile grid alone does not show the identity panel, and it is the panel that sets the
	 * two weights one line apart.
	 *
	 * Driven with real Right/Enter presses like every other walk, so a moved row or an eaten key
	 * photographs itself instead of passing quietly.
	 */
	static TArray<FStep> SelectWalk()
	{
		TArray<FStep> Steps;
		Steps.Add(Shoot(0.60f, TEXT("20_select_open")));

		float T = 1.60f;
		for (int32 I = 0; I < 3; ++I, T += 0.45f)
		{
			Steps.Add(Press(T, TEXT("Right")));
		}
		Steps.Add(Shoot(T + 0.60f, TEXT("21_select_moved")));

		// Eight Rights from ROCCO lands on MORTIMER, and that tile is chosen rather than counted to.
		// Demo 20 item 2 changed his dash from a quarter to two fifths and slowed his cooldown 25%,
		// so his card is the one string on this screen that had to be rewritten to stay true
		// (Core/TraceCharacterRoster.cpp). Photographing the tile the change is about is what turns
		// "the text was edited" into "the player reads the right number".
		T += 1.60f;
		for (int32 I = 0; I < 5; ++I, T += 0.45f)
		{
			Steps.Add(Press(T, TEXT("Right")));
		}
		Steps.Add(Shoot(T + 0.60f, TEXT("22_select_mortimer_card")));

		return Steps;
	}

	/**
	 * IN MATCH: the game, then the pause/settings page over it, then back to the game.
	 *
	 * The last frame is not decoration. Spec §A3's complaint is that a modal changes the RENDERER
	 * under it; the way that shows up is a screen that does not come back the way it went in, so the
	 * walk photographs the return as well as the departure.
	 */
	static TArray<FStep> MatchWalk()
	{
		TArray<FStep> Steps;
		Steps.Add(Shoot(0.60f, TEXT("01_in_match")));

		Steps.Add(Press(2.20f, TEXT("Escape")));
		Steps.Add(Shoot(3.60f, TEXT("02_match_escape")));

		Steps.Add(Press(5.00f, TEXT("Down")));
		Steps.Add(Press(5.45f, TEXT("Down")));
		Steps.Add(Shoot(6.20f, TEXT("03_match_escape_moved")));

		Steps.Add(Press(7.20f, TEXT("Escape")));
		Steps.Add(Shoot(8.60f, TEXT("04_back_in_match")));

		return Steps;
	}

	/**
	 * The tag every frame of this walk is filed under. It is a variable rather than a literal
	 * because the frames of two different integration passes must not share a filename: v22's
	 * frames are still in Saved/Screenshots and "01_title" from the wrong pass is exactly the kind
	 * of evidence that gets quoted as this pass's. Set from the console command's second argument.
	 */
	static FString GTag = TEXT("v23integ");

	/**
	 * ACTUALLY PLAYING — spec v23's integration brief asks for a real match with bots, not just a
	 * screenshot of one: "goals, shooting, dashing, abilities".
	 *
	 * Every press below is a REAL bind read off the settings screen (MOVE FORWARD W, DASH LEFT
	 * SHIFT, FIRE LEFT MOUSE BUTTON, ABILITY E, JUMP SPACE BAR, PASS CORE RIGHT MOUSE BUTTON) going
	 * through the controller route, so a broken binding, a suppressed input or a refused ability
	 * shows up as a frame where nothing happened rather than as a harness that quietly asserted its
	 * own success. Nothing here calls an ability directly.
	 *
	 * The bots do the rest: they carry, shoot and score on their own, so the goal evidence comes out
	 * of the match log rather than out of a scripted goal.
	 */
	static TArray<FStep> PlayWalk()
	{
		TArray<FStep> Steps;
		Steps.Add(Shoot(0.60f, TEXT("30_in_match")));

		// Move, so the dash has somewhere to go and the trail has a direction.
		Steps.Add(Play(1.60f, TEXT("W"), 1.60f));
		Steps.Add(Play(2.60f, TEXT("LeftShift"), 0.10f));
		Steps.Add(Shoot(3.10f, TEXT("31_dash")));

		// Fire: a burst rather than a tap, so the muzzle, the recoil and the ammo counter all move.
		Steps.Add(Play(4.20f, TEXT("LeftMouseButton"), 0.60f));
		Steps.Add(Shoot(4.70f, TEXT("32_firing")));

		// The character's own ability. Whether it is refused is itself the answer.
		Steps.Add(Play(6.00f, TEXT("E"), 0.15f));
		Steps.Add(Shoot(6.60f, TEXT("33_ability")));

		Steps.Add(Play(7.60f, TEXT("SpaceBar"), 0.10f));
		Steps.Add(Play(8.00f, TEXT("W"), 1.20f));
		Steps.Add(Shoot(9.60f, TEXT("34_after_play")));

		// The scoreboard is a whole screen of type and it is the one place kill-feed names, scores
		// and the clock are all on screen together.
		Steps.Add(Play(10.60f, TEXT("Tab"), 1.80f));
		Steps.Add(Shoot(11.60f, TEXT("35_scoreboard")));

		return Steps;
	}

	/**
	 * THE THREE WEAPON STATES, PRESSED ON THE KEYBOARD — spec v30 §§2-4, integration pass.
	 *
	 * WHY THIS IS A WALK AND NOT THREE LAUNCHES. Spec v30's opening complaint is that nothing on
	 * screen told the player which gun they hold, so the deliverable is a TRANSITION: press 3 and the
	 * picture must change. One editor launch per state cannot photograph a transition — it can only
	 * photograph three separate first frames, each of which could have been produced by a gun that
	 * never changed. This presses 2, then 3, then 1 in ONE match, on the real binds (StowGuns=1,
	 * EquipPistol=2, EquipSmg=3 — Settings/TraceUserSettings.cpp), and photographs each.
	 *
	 * NOTHING HERE CALLS AN EQUIP FUNCTION. Trace.ViewModel.Equip exists and would be easier, but it
	 * goes in behind the keybinding, so a walk built on it would still pass if the 3 key were unbound,
	 * stolen by another action, or eaten by a UI focus rule. That is a real failure mode in this
	 * project — Trace.Keys.LegacySteal exists because a bind was stolen once — so the integration pass
	 * presses keys.
	 *
	 * THE GAPS ARE DELIBERATE, in this order:
	 *   - 0.60 s after a key before the frame: the selector replicates and the rig is shown or hidden
	 *     in Tick, so a frame taken in the same frame as the press shows the state BEFORE it.
	 *   - the burst is 0.50 s = five rounds at 600 RPM, enough that the clip visibly drops.
	 *   - the shot-frame pose is PINNED with Trace.Smg.Hold rather than caught: the fire cycle is
	 *     0.100 s and no unattended screenshot can land inside it reliably. The walls thrown apart and
	 *     the cyan at its peak are photographed from the pinned pose, and Trace.ViewModel.Guns prints
	 *     the spread and the intensity from the same moment.
	 *   - the reload frame is 0.45 s after R: the 0.8 s authored motion is time-stretched to the 1.3 s
	 *     gameplay reload, so the cell is fully out around 0.42 s in.
	 *   - the last burst is 4.60 s, longer than the 4.00 s it takes to empty a 40-round magazine at
	 *     600 RPM, so the amber is read at the bottom of its range and not part way down.
	 */
	static TArray<FStep> GunsWalk()
	{
		TArray<FStep> Steps;

		Steps.Add(Shoot(0.60f, TEXT("40_match_start")));

		// --- 2: THE PISTOL --------------------------------------------------------------------
		Steps.Add(Press(1.20f, TEXT("Two")));
		Steps.Add(Say(1.80f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Shoot(2.10f, TEXT("41_key2_pistol")));

		// --- 3: THE SMG -----------------------------------------------------------------------
		Steps.Add(Press(3.20f, TEXT("Three")));
		Steps.Add(Say(3.90f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Shoot(4.20f, TEXT("42_key3_smg")));

		// --- FIRING: the tracer leaves ITS barrel, and the clip drops --------------------------
		Steps.Add(Play(5.20f, TEXT("LeftMouseButton"), 0.50f));
		Steps.Add(Shoot(5.45f, TEXT("43_smg_firing")));
		Steps.Add(Say(5.90f, TEXT("Trace.ViewModel.Guns")));

		// --- THE SHOT FRAME, PINNED: walls apart, cyan at 4.8x --------------------------------
		Steps.Add(Say(6.60f, TEXT("Trace.Smg.Hold 0 -1 2.5")));
		Steps.Add(Say(7.10f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Shoot(7.40f, TEXT("44_smg_shotframe")));
		Steps.Add(Say(9.20f, TEXT("Trace.Smg.Hold -1 -1 0")));

		// --- RELOAD: the magazine drops -------------------------------------------------------
		Steps.Add(Play(10.00f, TEXT("R"), 0.12f));
		Steps.Add(Shoot(10.45f, TEXT("45_smg_reload_magout")));
		Steps.Add(Say(10.55f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Shoot(10.90f, TEXT("46_smg_reload_riding")));

		// --- EMPTY IT: the amber has to be at the bottom of its range -------------------------
		Steps.Add(Play(12.60f, TEXT("LeftMouseButton"), 4.60f));
		Steps.Add(Say(16.90f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Shoot(17.10f, TEXT("47_smg_empty")));

		// The full spec-typed report, once, with the SMG still up.
		Steps.Add(Say(18.20f, TEXT("Trace.Smg.Probe")));

		// --- 1: NEITHER GUN -------------------------------------------------------------------
		Steps.Add(Press(21.00f, TEXT("One")));
		Steps.Add(Say(21.70f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Shoot(22.00f, TEXT("48_key1_neither")));

		// --- AND BACK TO 2, so the last frame proves the walk can leave state 1 ----------------
		Steps.Add(Press(23.00f, TEXT("Two")));
		Steps.Add(Say(23.70f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Shoot(24.00f, TEXT("49_key2_pistol_again")));

		return Steps;
	}

	/**
	 * @param Tag  SNAPSHOTTED WHEN THE WALK STARTED, never read from GTag here.
	 *
	 * GTag is process-wide and the steps fire seconds later off the core ticker, so reading the
	 * global at photograph time means the LAST walk to start decides what every in-flight frame is
	 * called. That became reachable the moment a second alias set the tag (WalkGuns, spec v30):
	 * WalkGuns then WalkMatch in one process would have filed the match walk's frames under
	 * v30integ_*, which is precisely the misattribution this file's header warns about. Passing the
	 * tag down makes each walk's frames immune to what any later walk does.
	 */
	static void TakeShot(const FString& Tag, const FString& Name)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Screenshots") / (Tag + TEXT("_") + Name + TEXT(".png")));

		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*FPaths::GetPath(Path));

		// bShowUI = true for the same reason UI/TraceAutoShot.cpp gives at length: false does not
		// filter debug text, it drops the ENTIRE Slate layer — and the title screen, the row widgets
		// and every atlas label on them ARE Slate. A walk that photographed the menu with its menu
		// missing would be evidence for a conclusion that is not true.
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI=*/true, /*bAddFilenameSuffix=*/false);
		UE_LOG(LogTraceGame, Display, TEXT("[IntegWalk] SHOT %s -> %s"), *Name, *Path);
	}

	/** Runs one step. Kept off the timer lambda so the two paths log identically. */
	static void RunStep(APlayerController* PC, const FStep& Step, const FString& Tag)
	{
		if (!Step.Shot.IsEmpty())
		{
			TakeShot(Tag, Step.Shot);
			return;
		}

		if (!Step.Exec.IsEmpty())
		{
			// Through the player controller for the same reason TraceAutoShot's deferred exec does it:
			// several Trace.* commands resolve "the local pawn" from the executing controller, and
			// GEngine->Exec with a null player hands them nothing to work with. bWriteToLog so the
			// command itself is in the log above the report it produced.
			UE_LOG(LogTraceGame, Display, TEXT("[IntegWalk] EXEC %s"), *Step.Exec);
			if (PC != nullptr)
			{
				PC->ConsoleCommand(Step.Exec, /*bWriteToLog=*/true);
			}
			else
			{
				UE_LOG(LogTraceGame, Warning, TEXT("[IntegWalk] EXEC %s skipped — no local controller."), *Step.Exec);
			}
			return;
		}

		if (PC == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[IntegWalk] KEY %s skipped — no local controller."), *Step.Key);
			return;
		}

		UE_LOG(LogTraceGame, Display, TEXT("[IntegWalk] KEY %s (%.2fs, %s)"), *Step.Key, Step.Hold, *Step.Route);

		// The default is 0.12 s through the viewport path, which is where the menu's key handling
		// lives. A zero-length press can be delivered and released inside one frame and then never
		// seen by a screen that polls once per frame.
		PC->ConsoleCommand(FString::Printf(TEXT("Trace.SimInput %s %.2f %s"), *Step.Key, Step.Hold, *Step.Route),
			/*bWriteToLog=*/false);
	}

	static void Start(UWorld* World, const FString& Which)
	{
		if (World == nullptr)
		{
			UE_LOG(LogTraceGame, Warning, TEXT("[IntegWalk] no world."));
			return;
		}

		APlayerController* PC = World->GetFirstPlayerController();

		const bool bMatch  = Which.Equals(TEXT("Match"),  ESearchCase::IgnoreCase);
		const bool bSelect = Which.Equals(TEXT("Select"), ESearchCase::IgnoreCase);
		const bool bPlay   = Which.Equals(TEXT("Play"),   ESearchCase::IgnoreCase);
		const bool bGuns   = Which.Equals(TEXT("Guns"),   ESearchCase::IgnoreCase);
		const TArray<FStep> Steps = bMatch ? MatchWalk()
			: (bSelect ? SelectWalk() : (bPlay ? PlayWalk() : (bGuns ? GunsWalk() : MenuWalk())));

		UE_LOG(LogTraceGame, Display,
			TEXT("[IntegWalk] === %s WALK: %d steps, %.1fs, viewport %dx%d ==="),
			bMatch ? TEXT("MATCH")
				: (bSelect ? TEXT("SELECT") : (bPlay ? TEXT("PLAY") : (bGuns ? TEXT("GUNS") : TEXT("MENU")))), Steps.Num(),
			Steps.Num() > 0 ? Steps.Last().At : 0.f,
			(PC != nullptr && PC->GetLocalPlayer() != nullptr && PC->GetLocalPlayer()->ViewportClient != nullptr
				&& PC->GetLocalPlayer()->ViewportClient->Viewport != nullptr)
				? PC->GetLocalPlayer()->ViewportClient->Viewport->GetSizeXY().X : 0,
			(PC != nullptr && PC->GetLocalPlayer() != nullptr && PC->GetLocalPlayer()->ViewportClient != nullptr
				&& PC->GetLocalPlayer()->ViewportClient->Viewport != nullptr)
				? PC->GetLocalPlayer()->ViewportClient->Viewport->GetSizeXY().Y : 0);

		// ---- SCHEDULED ON THE CORE TICKER, NOT THE WORLD'S TIMER MANAGER ---------------------------
		//
		// This is not a preference, it is the bug the first version of this walk had. The match walk's
		// second step presses ESCAPE, which raises the pause menu, which PAUSES THE WORLD — and a
		// world timer does not tick in a paused world. The walk fired step 1, fired step 2, and then
		// stopped forever: the run produced one frame of the match and no frame of the pause menu at
		// all, which is precisely the screen it had been written to photograph.
		//
		// FTSTicker runs in real time and does not care about pause, time dilation or the world at
		// all. Returning false unregisters the delegate, so each step fires exactly once.
		//
		// One ticker per step rather than one walking a list: the steps are independent and a per-step
		// registration means a step that fails cannot strand the ones after it.
		TWeakObjectPtr<APlayerController> WeakPC(PC);
		const FString TagAtStart = GTag;   // see TakeShot: the steps outlive whatever GTag becomes next
		for (int32 Index = 0; Index < Steps.Num(); ++Index)
		{
			const FStep Step = Steps[Index];
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakPC, Step, TagAtStart](float) -> bool
			{
				RunStep(WeakPC.Get(), Step, TagAtStart);
				return false;
			}), FMath::Max(0.01f, Step.At));
		}
	}

	static FAutoConsoleCommandWithWorldAndArgs GWalkCommand(
		TEXT("Trace.Integ.Walk"),
		TEXT("Trace.Integ.Walk Menu|Match|Select [tag] — walks the screens with real key presses and ")
		TEXT("photographs each stop to Saved/Screenshots/<tag>_*.png (default tag v23integ)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (Args.Num() > 1 && !Args[1].IsEmpty())
				{
					GTag = Args[1];
				}
				Start(World, Args.Num() > 0 ? Args[0] : FString(TEXT("Menu")));
			}));

	// ---------------------------------------------------------------------------------------------
	// THREE ARGUMENT-FREE ALIASES, AND THE REASON THEY EXIST
	//
	// The only way to fire a command at a live screen headlessly is -TraceExec=, and its value goes
	// through FParse::Value, which stops at the first SPACE unless the value is quoted. Quoting it
	// does not survive: on macOS the launcher stub re-execs
	// UnrealEditor.app/Contents/MacOS/UnrealEditor ("Running incorrect executable for target ...
	// Launching ... instead") and the rebuilt command line arrives DOUBLED —
	//     -TraceExec=""Trace.Integ.Walk Menu v23integ""
	// — which parses to nothing and the walk silently never runs. Measured, not guessed: a full
	// launch produced no [IntegWalk] line at all and no frames.
	//
	// A command that takes NO arguments cannot hit that, so each walk gets its own name. The command
	// above still exists for a human typing into the console, where the arguments are useful.
	// ---------------------------------------------------------------------------------------------
	static FAutoConsoleCommandWithWorld GWalkMenuCommand(
		TEXT("Trace.Integ.WalkMenu"),
		TEXT("The title-screen walk, no arguments — safe to pass to -TraceExec="),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World) { Start(World, TEXT("Menu")); }));

	static FAutoConsoleCommandWithWorld GWalkMatchCommand(
		TEXT("Trace.Integ.WalkMatch"),
		TEXT("The in-match walk, no arguments — safe to pass to -TraceExec="),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World) { Start(World, TEXT("Match")); }));

	static FAutoConsoleCommandWithWorld GWalkSelectCommand(
		TEXT("Trace.Integ.WalkSelect"),
		TEXT("The character-select walk, no arguments — safe to pass to -TraceExec="),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World) { Start(World, TEXT("Select")); }));

	static FAutoConsoleCommandWithWorld GWalkPlayCommand(
		TEXT("Trace.Integ.WalkPlay"),
		TEXT("Plays: move, dash, fire, ability, jump, scoreboard — real binds, no arguments."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World) { Start(World, TEXT("Play")); }));

	static FAutoConsoleCommandWithWorld GWalkGunsCommand(
		TEXT("Trace.Integ.WalkGuns"),
		TEXT("Spec v30 §§2-4: presses 2, 3 and 1 on the real binds in one match, fires and reloads the "
		     "SMG, empties its magazine, and photographs every state — no arguments."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World)
			{
				// Its own tag, so a v30 frame can never be quoted as a v22 or v23 one. The v22 walk's
				// header explains at length why that matters; the same argument applies to a pass whose
				// whole deliverable is "the picture changed".
				GTag = TEXT("v30integ");
				Start(World, TEXT("Guns"));
			}));
}

#endif // !UE_BUILD_SHIPPING
