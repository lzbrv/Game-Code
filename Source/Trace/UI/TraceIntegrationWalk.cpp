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
#include "Core/TraceCharacter.h"   // UsesPackHands() — the walk names its frames after the rig it sees

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
	 * THE THREE WEAPON STATES, PRESSED ON THE KEYBOARD — spec v31 §§1, 5 and 6, integration pass.
	 *
	 * *** RE-AUTHORED FOR v31. THE KEYS MOVED AND THE HANDS ARRIVED. ***
	 *
	 * It used to press Two / Three / One under the v30 layout (1 = STOW GUNS, 2 = PISTOL, 3 = SMG)
	 * and file the frames as `41_key2_pistol`, `42_key3_smg`, `48_key1_neither`. Spec v31 §1 flipped
	 * UTraceMeleeSettings::bDualWieldKnife OFF, deleted the stow state and re-keyed all three slots to
	 * **1 = PISTOL, 2 = SMG, 3 = KNIFE**, so every one of those step names named the wrong weapon and
	 * the middle of the walk — Trace.Smg.Hold, the R reload, the 4.6 s burst — was running with the
	 * KNIFE out, where none of it means anything. The frames were still honest pictures of the game;
	 * they were filed under lies. This walk now presses the v31 keys in the v31 order.
	 *
	 * WHY THIS IS A WALK AND NOT THREE LAUNCHES. The deliverable is a TRANSITION: press 3 and the
	 * picture must change. One editor launch per state cannot photograph a transition — only three
	 * separate first frames, each of which could have been produced by a game that never changed.
	 *
	 * NOTHING HERE CALLS AN EQUIP FUNCTION. Trace.ViewModel.Equip exists and would be easier, but it
	 * goes in behind the keybinding, so a walk built on it would still pass if the 3 key were unbound,
	 * stolen by another action, or eaten by a UI focus rule. That is a real failure mode in this
	 * project — Trace.Keys.LegacySteal exists because a bind was stolen once — so the integration pass
	 * presses keys. Every press below is the SHIPPED DEFAULT of a rebindable row in
	 * TraceInputActions::All(): One/Two/Three (PISTOL/SMG/KNIFE), LeftMouseButton (FIRE), R (RELOAD),
	 * RightMouseButton (MELEE, spec v28 §10), F (INSPECT, spec v31 §5) and SpaceBar (JUMP).
	 *
	 * WHAT v31 ADDS BEYOND THE KEYS: every state is sampled with `Trace.Hands.Probe`, which reads the
	 * rig kind, the loadout, the clip actually loaded on the component and its playhead OFF THE
	 * COMPONENT rather than off this harness's belief about it. A frame that shows hands is one
	 * claim; a frame taken 0.1 s after a log line naming `A_Hands_Reload_Smg t=0.31/0.80s` is two
	 * independent ones. The knife states are sampled with `Trace.Knife.PackStatus` for the same
	 * reason — it names SK_TraceKnife or the v27 cube blade, the live clip and the emissive.
	 *
	 * THE GAPS ARE DELIBERATE, in this order:
	 *   - 0.70 s after a slot key before the frame: the selector replicates, the pullout runs (0.200 s
	 *     for a gun, 0.130 s for the knife — spec v31 §1's KnifeSwapMultiplier) and the rig is shown
	 *     or hidden in Tick, so a frame taken on the press frame shows the state BEFORE it.
	 *   - the SMG burst is 0.50 s = five rounds at 600 RPM, enough that the clip visibly drops.
	 *   - the reload frame is ~0.45 s after R: the 0.8 s authored A_Hands_Reload_* is time-stretched
	 *     to the gameplay reload, so the cell is fully out around 0.42 s in.
	 *   - the stab frame is 0.18 s after right mouse: A_Knife_Stab and A_Hands_Stab_Knife are both
	 *     0.300 s, so 0.18 s is past the wind-up and inside the thrust. It is a REAL swing, not a
	 *     pinned pose — Trace.Hands.Hold photographs the pinned version in the companion run, and the
	 *     two together are what separate "the clip exists" from "a click plays it".
	 *   - the flourish is photographed TWICE, 1.4 s apart, because A_Knife_Inspect is 3.20 s of
	 *     balisong: two frames of a moving clip differ, two frames of a stuck one do not.
	 */
	static TArray<FStep> GunsWalk()
	{
		TArray<FStep> Steps;

		Steps.Add(Shoot(0.60f, TEXT("40_match_start")));
		Steps.Add(Say(0.90f, TEXT("Trace.Hands.Probe")));

		// --- 1: THE PISTOL --------------------------------------------------------------------
		Steps.Add(Press(1.20f, TEXT("One")));
		Steps.Add(Say(1.90f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Say(2.00f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(2.30f, TEXT("41_key1_pistol_idle")));

		// SHOOT IT. A_Hands_Shoot_Pistol is 0.1667 s, so the frame is taken while the burst is still
		// held rather than after it — a shot frame caught after the release is an idle frame.
		Steps.Add(Play(3.20f, TEXT("LeftMouseButton"), 0.30f));
		Steps.Add(Say(3.32f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(3.42f, TEXT("42_pistol_firing")));

		// RELOAD IT.
		Steps.Add(Play(4.60f, TEXT("R"), 0.12f));
		Steps.Add(Say(5.00f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(5.10f, TEXT("43_pistol_reload")));

		// --- 2: THE SMG -----------------------------------------------------------------------
		Steps.Add(Press(6.60f, TEXT("Two")));
		Steps.Add(Say(7.30f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Say(7.40f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(7.70f, TEXT("44_key2_smg_idle")));

		// SHOOT IT — a burst, so the muzzle, the recoil and the ammo counter all move.
		Steps.Add(Play(8.60f, TEXT("LeftMouseButton"), 0.50f));
		Steps.Add(Say(8.82f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(8.92f, TEXT("45_smg_firing")));
		Steps.Add(Say(9.40f, TEXT("Trace.ViewModel.Guns")));

		// RELOAD IT.
		Steps.Add(Play(10.20f, TEXT("R"), 0.12f));
		Steps.Add(Say(10.60f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(10.70f, TEXT("46_smg_reload")));

		// The full spec-typed report, once, with the SMG still up.
		Steps.Add(Say(11.40f, TEXT("Trace.Smg.Probe")));

		// --- 3: THE KNIFE ---------------------------------------------------------------------
		//
		// The knife is a WEAPON SLOT again (spec v31 §1), so this key takes the guns off the screen
		// rather than adding a blade beside one. Trace.Knife.DualWeaponTest is the harness that
		// asserts the two are never drawn together; this walk photographs it.
		Steps.Add(Press(12.20f, TEXT("Three")));
		Steps.Add(Say(12.90f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Say(12.98f, TEXT("Trace.Knife.PackStatus")));
		Steps.Add(Say(13.06f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(13.30f, TEXT("47_key3_knife_idle")));

		// STAB IT — right mouse, the spec v28 §10 melee bind, which with dual wield OFF swings only
		// because the blade is the weapon in hand.
		Steps.Add(Play(14.20f, TEXT("RightMouseButton"), 0.12f));
		Steps.Add(Say(14.32f, TEXT("Trace.Knife.PackStatus")));
		Steps.Add(Shoot(14.38f, TEXT("48_knife_stab")));

		// INSPECT IT — F, the new spec v31 §5 bind, on the key the Core pull vacated for it.
		Steps.Add(Play(15.60f, TEXT("F"), 0.12f));
		Steps.Add(Say(15.90f, TEXT("Trace.Knife.PackStatus")));
		Steps.Add(Say(15.98f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(16.10f, TEXT("49_knife_inspect_early")));
		Steps.Add(Say(17.40f, TEXT("Trace.Knife.PackStatus")));
		Steps.Add(Shoot(17.50f, TEXT("50_knife_inspect_late")));

		// --- JUMP, WITH THE BLADE OUT ---------------------------------------------------------
		//
		// A_Hands_Jump_Knife is 0.700 s. The frame is 0.25 s in, near the top of the tuck.
		Steps.Add(Play(19.20f, TEXT("SpaceBar"), 0.10f));
		Steps.Add(Say(19.42f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(19.50f, TEXT("51_jump_knife")));

		// --- AND BACK TO 1, so the last frame proves the walk can LEAVE the knife --------------
		Steps.Add(Press(21.00f, TEXT("One")));
		Steps.Add(Say(21.70f, TEXT("Trace.ViewModel.Guns")));
		Steps.Add(Say(21.80f, TEXT("Trace.Hands.Probe")));
		Steps.Add(Shoot(22.00f, TEXT("52_key1_pistol_again")));

		return Steps;
	}

	/**
	 * *** THE OWNER'S CORE-THROW BUG, THROWN IN PLAY — spec v31 §2, integration pass. ***
	 *
	 * Verbatim: "when a player is going down the core just drops instead of going forward when
	 * thrown. I still want the core to carry the player's velocity but I don't want this drop to
	 * happen, it completely feels like a bug."
	 *
	 * WHY THIS EXISTS ALONGSIDE Trace.ModeB.MomentumTestNow. That command is the better MEASUREMENT —
	 * it prints the pre-v31 and post-v31 launch of the same throw across five thrower states — but it
	 * ASSIGNS the thrower's velocity and movement mode before each throw. Everything downstream of
	 * that assignment is the real code path, and nothing upstream of it is: nobody jumped, nobody
	 * charged, nobody let go. This walk is the other half. It takes the Core, runs forward, JUMPS,
	 * and releases a charged throw 0.5 s AFTER THE APEX — the pawn is genuinely descending under its
	 * own gravity, on its own velocity, through the same left-mouse route the HUD teaches ("LMB -
	 * THROW"). What it proves is weaker per throw and stronger in kind: the bug the owner reported is
	 * the one being photographed.
	 *
	 * THE TIMING IS ARITHMETIC, NOT TASTE. JumpZVelocity is 640 uu/s against ~980 uu/s of gravity, so
	 * the apex is ~0.65 s after the press. The throw is pressed 0.30 s into the jump and released
	 * 0.85 s later — 1.15 s after take-off, i.e. half a second into the DESCENT, with the thrower
	 * carrying roughly -490 uu/s of vertical velocity. That is the state the note is about. The hold
	 * is also the charge: v13 §6's throw charges while the button is down.
	 *
	 * Trace.ModeB.FlightLog is switched on around the throw so this machine logs the loose Core's own
	 * position and speed at 10 Hz, and Trace.ModeB.ThrowMomentum prints the launch split into impulse,
	 * inherited term and the v31 §2 before/after Z. (Trace.ModeB.**Tally** is the goal tally and was
	 * the first thing this step called by mistake — it printed "0 goals" beside a throw and said
	 * nothing about it. Named here so the next reader does not repeat it.) A frame showing the Core out in front of the
	 * player is the picture; those two are the numbers under it.
	 */
	static TArray<FStep> CoreWalk()
	{
		TArray<FStep> Steps;

		Steps.Add(Say(0.60f, TEXT("Trace.ModeB.FlightLog 1")));
		Steps.Add(Say(0.80f, TEXT("Trace.DebugTakeCore")));
		Steps.Add(Shoot(1.60f, TEXT("60_core_taken")));

		// Run, so the throw has a horizontal term to carry — the half of the inheritance the owner
		// explicitly wants KEPT. A standing falling throw would prove only half the note.
		Steps.Add(Play(2.40f, TEXT("W"), 2.60f));

		// JUMP, then release the charged throw half a second past the apex.
		Steps.Add(Play(3.60f, TEXT("SpaceBar"), 0.10f));
		Steps.Add(Play(3.90f, TEXT("LeftMouseButton"), 0.85f));
		Steps.Add(Shoot(4.85f, TEXT("61_thrown_while_falling")));
		Steps.Add(Say(5.00f, TEXT("Trace.ModeB.ThrowMomentum")));

		// The flight itself. Two frames a second apart: a Core that dropped at the thrower's feet and
		// a Core that went forward look identical in one frame and nothing alike in two.
		Steps.Add(Shoot(5.60f, TEXT("62_core_in_flight")));
		Steps.Add(Shoot(6.60f, TEXT("63_core_in_flight_later")));
		Steps.Add(Say(7.40f, TEXT("Trace.ModeB.CoreProbe")));
		Steps.Add(Shoot(7.60f, TEXT("64_core_settled")));
		Steps.Add(Say(7.80f, TEXT("Trace.ModeB.FlightLog 0")));

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
		const bool bCore   = Which.Equals(TEXT("Core"),   ESearchCase::IgnoreCase);
		const TArray<FStep> Steps = bMatch ? MatchWalk()
			: (bSelect ? SelectWalk()
			: (bPlay ? PlayWalk()
			: (bGuns ? GunsWalk() : (bCore ? CoreWalk() : MenuWalk()))));

		UE_LOG(LogTraceGame, Display,
			TEXT("[IntegWalk] === %s WALK: %d steps, %.1fs, viewport %dx%d ==="),
			bMatch ? TEXT("MATCH")
				: (bSelect ? TEXT("SELECT")
				: (bPlay ? TEXT("PLAY")
				: (bGuns ? TEXT("GUNS") : (bCore ? TEXT("CORE") : TEXT("MENU"))))), Steps.Num(),
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

	static FAutoConsoleCommandWithWorld GWalkCoreCommand(
		TEXT("Trace.Integ.WalkCore"),
		TEXT("Spec v31 §2: takes the Core, runs, JUMPS and releases a charged throw half a second "
		     "after the apex — the owner's \"thrown while going down\" case, through the real left-"
		     "mouse route — then photographs the flight. Mode B only. No arguments."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World)
			{
				GTag = TEXT("v31core");
				Start(World, TEXT("Core"));
			}));

	static FAutoConsoleCommandWithWorld GWalkGunsCommand(
		TEXT("Trace.Integ.WalkGuns"),
		TEXT("Spec v31 §§1/5/6: presses 1, 2 and 3 on the real binds in one match — PISTOL, SMG, "
		     "KNIFE — fires and reloads both guns, stabs, presses F to inspect the blade and jumps, "
		     "sampling Trace.Hands.Probe at every state and photographing each. No arguments."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World)
			{
				// Its own tag, so a v31 frame can never be quoted as a v30, v23 or v22 one. The v22
				// walk's header explains at length why that matters; the same argument applies to a
				// pass whose whole deliverable is "the picture changed". THE TAG MOVES WITH THE
				// LAYOUT: v30's frames are still on disk under v30integ_*, photographing the OLD key
				// layout, and a reader who found v30integ_41_key2_pistol next to this pass's frames
				// would have no way to tell which walk produced which.
				//
				// *** AND IT MOVES WITH THE RIG, WHICH IS NOT DECORATION — IT IS A BUG THIS WALK HAS
				// ALREADY CAUSED. *** The v31 integration pass ran this walk twice in one sitting:
				// once normally, and once with Content/Trace/Art moved aside to prove the no-`git lfs
				// pull` fallback. Both runs wrote v31integ_41_key1_pistol_idle.png, so the second
				// silently overwrote the first and the surviving file showed the PROCEDURAL CUBE rig
				// under a filename the report was about to quote as the pack hands. The frames were
				// honest; the names were not, which is exactly the failure the header above warns
				// about and exactly the failure a fixed literal cannot prevent.
				//
				// So the tag is READ OFF THE PAWN instead of asserted. A frame taken with the fallback
				// rig up can no longer land on a pack-rig filename however the fallback was reached —
				// the switch, a missing asset, or a future third route nobody has thought of yet.
				const ATraceCharacter* LocalChar = (World != nullptr)
					? Cast<ATraceCharacter>(World->GetFirstPlayerController() != nullptr
						? World->GetFirstPlayerController()->GetPawn() : nullptr)
					: nullptr;
				const bool bPackRig = (LocalChar != nullptr) && LocalChar->UsesPackHands();
				GTag = bPackRig ? TEXT("v31integ") : TEXT("v31fallback");
				UE_LOG(LogTraceGame, Display,
					TEXT("[IntegWalk] hand rig is %s, so these frames are filed as %s_*."),
					bPackRig ? TEXT("THE PACK (SK_TraceHands)") : TEXT("THE PROCEDURAL CUBE FALLBACK"),
					*GTag);
				Start(World, TEXT("Guns"));
			}));
}

#endif // !UE_BUILD_SHIPPING
