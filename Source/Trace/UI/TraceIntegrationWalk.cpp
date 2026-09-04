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
// The argument-free aliases (safe to pass to -TraceExec=, see the alias block at the bottom):
//     Trace.Integ.WalkMenu / WalkMatch / WalkSelect / WalkPlay / WalkGuns
//     Trace.Integ.WalkCore        spec v31 §2 / v32 §7e — the Core thrown while descending. GATED on
//                                 the measured take-off; it RE-STAGES the jump when the arena's
//                                 geometry cuts the flight short, and it FAILS rather than mislabels.
//     Trace.Integ.WalkCoreRedArm  the same walk with every throw aimed AFTER the landing. Must FAIL.
//     Trace.Integ.WalkCoreRetryArm  the same walk with only the FIRST throw aimed after the landing.
//                                 Must MISS attempt 1 and then PASS on attempt 2.
//     Trace.Integ.Bloom           spec v32 §6 — prints the live post-process bloom and says whether
//                                 the FX doc's 1.5x base emissive crosses the threshold. Measures
//                                 only; changes nothing.
//
// Frames land in Saved/Screenshots as v22integ_<NN>_<what>.png, so the file name says which step of
// which walk produced it and no frame has to be identified by its timestamp. The log carries the
// same names, which is what lets a frame be attributed to a run when more than one editor is up.

#include "CoreMinimal.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"   // TActorIterator — the bloom probe walks the live post-process volumes
#include "Engine/PostProcessVolume.h"
#include "GameFramework/CharacterMovementComponent.h"   // IsFalling()/GetGravityZ() — the throw gate
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Containers/Ticker.h"   // FTSTicker — real time, so a paused world cannot strand the walk
#include "TimerManager.h"
#include "UnrealClient.h"

#include "Trace.h"   // LogTraceGame
#include "Core/TraceCharacter.h"   // UsesPackHands() — the walk names its frames after the rig it sees
#include "Gameplay/TraceCore.h"    // ATraceCore::LastThrow — the throw's own record of the thrower's state

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

		/**
		 * SPEC v32 §7e. When true this step does not press, photograph or report — it hands the rest
		 * of the walk over to a STATE MACHINE that waits for a measured condition instead of a clock.
		 *
		 * The timer list is the right shape for a menu tour, where every step is "press, wait long
		 * enough for a frame, photograph". It is the WRONG shape for "throw the Core while descending
		 * under gravity", because the thing being photographed is a PHYSICAL STATE and no fixed offset
		 * from a jump press is guaranteed to land in it. See ArmCoreThrowGate.
		 */
		bool bCoreThrowGate = false;
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

	/** SPEC v32 §7e. Hands the walk over to the measured throw gate; see ArmCoreThrowGate. */
	static FStep ThrowGate(float At)
	{
		FStep Step;
		Step.At = At;
		Step.bCoreThrowGate = true;
		return Step;
	}

	/**
	 * THE TITLE SCREEN TOUR.
	 *
	 * The row order is ETraceMenuRow's: PLAY, JOIN, PRACTICE, DIFFICULTY, SETTINGS, QUIT. Four Downs
	 * from the top lands on SETTINGS and three Ups from there lands on JOIN, so the walk never types
	 * a row NUMBER — it moves the way a player moves and photographs where it got to. If a row is
	 * ever inserted or removed, the frames show the wrong screen rather than silently passing.
	 *
	 * THE TWO COUNTS MOVED WITH THE ROW. They were five and four while SCORING MODE sat at index 4;
	 * removing it moved SETTINGS from 5 to 4 and JOIN is still 1, so both counts drop by one.
	 *
	 * The gaps are 0.45 s, comfortably more than one frame at any rate this project runs at, because
	 * the menu reads its keys once per frame and two presses inside one frame are one press.
	 */
	static TArray<FStep> MenuWalk()
	{
		TArray<FStep> Steps;
		Steps.Add(Shoot(0.60f, TEXT("01_title")));

		float T = 1.40f;
		for (int32 I = 0; I < 4; ++I, T += 0.45f)
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
		for (int32 I = 0; I < 3; ++I, T += 0.45f)
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
	 * *** SPEC v32 §7e — THE TIMING USED TO BE ARITHMETIC AND IT WAS NOT ENOUGH. ***
	 *
	 * What was here: "JumpZVelocity is 640 uu/s against ~980 uu/s of gravity, so the apex is ~0.65 s
	 * after the press. The throw is pressed 0.30 s into the jump and released 0.85 s later." The
	 * arithmetic is right and the harness was still non-deterministic about the one state it exists to
	 * prove. A v31 verifier ran it three times from an identical command line and got two throws while
	 * AIRBORNE DESCENDING (-580 and -569 uu/s) and one throw ON THE GROUND — and the ground run still
	 * filed its frame as `61_thrown_while_falling`, which is a harness passing over its own failure.
	 *
	 * WHY OPEN-LOOP ARITHMETIC COULD NOT WORK HERE. The margin is tiny and the schedule is not the
	 * only clock in the room. A 640/980 jump is airborne for ~1.3 s and the old plan released at
	 * take-off + 1.15 s, so the whole tolerance for everything that shifts take-off — a frame or two
	 * of input latency on the SimInput hold, a hitch while a screenshot is encoded, the pawn clipping
	 * a ramp, the run step ending on a slightly different piece of floor — was 0.15 s. Real-time
	 * tickers do not narrow that; they measure it. And take-off is the term the schedule cannot see:
	 * `Trace.SimInput SpaceBar` presses a key, it does not leave the ground.
	 *
	 * SO THE JUMP AND THE THROW ARE NOW A CLOSED LOOP, AND THE VERDICT IS A MEASUREMENT.
	 * `ThrowGate` hands the walk to ArmCoreThrowGate, which
	 *   1. presses jump and WAITS for the pawn's own movement component to report IsFalling(), rather
	 *      than assuming a key press became a take-off;
	 *   2. reads the LIVE vertical velocity and the LIVE gravity on that first airborne frame and
	 *      solves for the apex from them, so the release is placed a fixed FRACTION OF THE WAY BACK
	 *      DOWN rather than a fixed number of seconds past a guessed apex — which is what buys the
	 *      margin back, and which also means a retuned JumpZVelocity moves the schedule with it;
	 *   3. reads ATraceCore::LastThrow AFTER the release — the throw's own record of the thrower's
	 *      state at the instant of launch, written by ThrowFromHolder itself — and names the frame and
	 *      the verdict from THAT. Not from the plan. A throw off the ground is now
	 *      `61_thrown_ON_GROUND_MISS` and a loud FAIL, which is what the v31 run should have printed.
	 *
	 * *** AND, SINCE A v32 VERIFIER MEASURED THE ABOVE AT 7 PASS / 2 FAIL OVER NINE IDENTICAL RUNS,
	 * IT ALSO RE-STAGES. *** Honest is not the same as deterministic. The two misses were not the
	 * gate's clock and not the throw: the pawn ran into the goal structure it was jumping at and came
	 * down on top of it, so its flight was 0.2 s shorter than the symmetric one the take-off predicts
	 * and the release arrived after touchdown. That is the arena, not the game under test, and it is
	 * the harness's job to try again rather than to report the arena as a defect. So a miss now takes
	 * the Core back, waits for the pawn to be standing, runs and jumps again, up to GateMaxAttempts —
	 * each attempt measured on its own, the verdict naming which attempt made the state and how many
	 * missed, and the whole run still FAILING if none of them does. GateMaxAttempts carries the
	 * argument in full, including what keeps a retry loop from becoming a way to hide a failure.
	 *
	 * A throw that never registers at all is also a FAIL: LastThrow.Serial is watched, never its
	 * values, for the reason the member itself gives — two identical throws are byte-identical.
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

		// *** THE HANDOVER, v32 §7e. *** The jump, the charge, the release, the frame and the whole
		// flight tail live inside the gate from here, because every one of them is placed relative to
		// a MEASURED take-off rather than to this list's clock. The "N steps, X s" banner Start()
		// prints therefore understates a Core walk by about four seconds, and by about three more per
		// re-staged attempt; that is the gate running.
		Steps.Add(ThrowGate(3.60f));

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

	// =============================================================================================
	// *** SPEC v32 §7e — THE CORE-THROW GATE. WAIT FOR THE STATE; FAIL IF IT IS MISSED. ***
	// =============================================================================================
	//
	// CoreWalk's header carries the argument for why this exists. This is the machine.

	/**
	 * How long to wait for the jump press to become an actual take-off before calling the run a miss.
	 *
	 * Generous: this is not the precision knob, it is the "something is badly wrong" knob. A jump that
	 * has not left the ground two seconds after the key went down did not happen — the pawn is dead,
	 * pinned, in a menu, or the bind moved — and none of those should be photographed as a throw.
	 */
	static constexpr double GateAirborneTimeoutSeconds = 2.0;

	/**
	 * WHERE IN THE DESCENT THE RELEASE IS AIMED, as a fraction of the fall from the apex back to
	 * take-off height. 0 is the apex (not descending at all); 1 is the landing frame.
	 *
	 * *** AND HERE IS WHY THE OLD FIXED 0.50 s MISSED, MEASURED RATHER THAN REASONED. *** The old
	 * comment did the arithmetic against "JumpZVelocity 640 uu/s against ~980 uu/s of gravity", giving
	 * an apex 0.65 s up and 0.65 s back down — a 1.30 s flight with a release at take-off + 1.15 s,
	 * i.e. 0.15 s of margin. THE LIVE NUMBERS ARE NOT THOSE. This gate prints them every run, and over
	 * five runs the pawn left the ground at +614 to +621 uu/s against 1098 uu/s of gravity: an apex
	 * 0.56 s up and a flight of about 1.12 s. The old plan therefore aimed its release 1.15 s into a
	 * 1.12 s flight — ABOUT THIRTY MILLISECONDS AFTER TOUCHDOWN. It is not that the margin was thin;
	 * there was no margin, and the two v31 runs that did catch the pawn airborne caught it in the last
	 * frames before landing, which is why they read -580 and -569 uu/s instead of the -490 the comment
	 * predicted. A stale constant in a comment became a schedule, and the schedule was wrong.
	 *
	 * SO THIS IS A FRACTION AND NOT A NUMBER OF SECONDS. Because it is solved from the velocity and
	 * gravity read on the take-off frame, a retuned JumpZVelocity, a retuned gravity or a low-gravity
	 * volume moves the release with it instead of silently eating the margin the way the old one did.
	 *
	 * *** AND HERE IS WHY 0.60 WAS STILL TOO LATE. NINE RUNS, ONE COMMAND LINE, MEASURED. ***
	 * A v32 verifier ran this gate nine times from a byte-identical command line and got seven PASSes
	 * and two FAILs. The gate was honest about all nine — that half of §7e works — but 22 % of runs
	 * still could not reach the state the walk exists to photograph, and a harness that cannot reach
	 * its own state one run in five is not yet deterministic. The two misses are not load and not
	 * noise: run 8 measured the SAME take-off as run 3 (+625 uu/s against 1098 uu/s, apex 0.57 s) and
	 * still threw from the floor. What separates them is WHERE THE PAWN CAME DOWN, which is the one
	 * term a ballistic solve off the take-off frame cannot see:
	 *
	 *     run 3 (PASS): threw at Z 284.6, AIRBORNE at -391 uu/s, 524 uu/s horizontal
	 *     run 8 (FAIL): threw at Z 371.0, ON THE GROUND,          25 uu/s horizontal
	 *
	 * 25 uu/s horizontal out of a 976 uu/s run means the pawn hit something; Z 371 against run 3's
	 * 284.6 means it came down on top of it. CoreWalk runs blindly forward for 2.6 s before it jumps,
	 * and what is in front of it is the Blue goal structure at X -16800 and the ramps around it. The
	 * fall is therefore NOT symmetric — the solve assumes the pawn lands at the height it took off
	 * from, and sometimes it lands a hundred uu above it, which shortens the flight by 0.2 s and puts
	 * a release aimed 0.34 s past the apex AFTER TOUCHDOWN.
	 *
	 * Two changes answer that, and only the second one makes the harness deterministic:
	 *   1. 0.60 -> 0.35 buys margin. At the measured take-off the release now lands 0.20 s past the
	 *      apex at about -219 uu/s — still more than twice GateMinDescentUU, so "unambiguously going
	 *      down" is untouched — with 0.37 s of airtime behind it instead of 0.23 s. Run 8's miss
	 *      needed the pawn down about 0.22 s early; this covers that and half again.
	 *   2. THE GATE RETRIES. Margin is a probability, not a guarantee, and a ledge one jump higher
	 *      would eat any fixed fraction. See GateMaxAttempts — that is the actual fix; this is the
	 *      cheap half that makes the retry rarely needed.
	 */
	static constexpr float GateDescendFraction = 0.35f;

	/**
	 * *** THE RED ARM (spec v32 §8's standing rule, applied to this gate). ***
	 *
	 * 1.60 is PAST THE LANDING: the fall from the apex back to take-off height is 1.00 by this
	 * measure, so a release aimed at 1.60 arrives well after the pawn is standing on the floor again.
	 * That is exactly the state the v31 walk hit by accident on one run in three and filed as
	 * `61_thrown_while_falling`. Trace.Integ.WalkCoreRedArm aims for it ON PURPOSE, and the gate must
	 * print `GATE VERDICT: FAIL` and `61_thrown_ON_GROUND_MISS`. A gate that passes both arms is not
	 * measuring anything, which is the whole complaint §7e is about.
	 */
	static constexpr float GateRedArmDescendFraction = 1.60f;

	/**
	 * THE FLOOR UNDER THE LEFT-MOUSE HOLD — and it is a floor now, not the hold itself.
	 *
	 * v13 §6: the hold IS the charge, full at 0.60 s, so a hold shorter than that photographs a
	 * half-charged throw and every flight number under it means something else. 0.70 keeps 0.10 s of
	 * headroom over that edge, which is several frames of input latency.
	 *
	 * *** WHY THE HOLD IS DERIVED AND IS NO LONGER THE FIXED 0.85 s. *** Trace.SimInput presses,
	 * waits and releases on its own ticker, so the hold is chosen AT THE PRESS and cannot be recalled
	 * afterwards — which makes the hold the only handle this gate has on where in the flight the
	 * release lands. A fixed 0.85 s hold therefore pinned the release at take-off + 0.85 s at the
	 * very earliest, because the press cannot happen before the take-off it is measured from. On the
	 * measured numbers (apex 0.57 s) that is 0.49 of the fall, so the old constant made every
	 * GateDescendFraction below ~0.5 UNREACHABLE: asking for 0.35 would silently have been served as
	 * 0.49. Solving the release first and deriving the hold from it
	 * (Hold = ToApex x (1 + Fraction), floored here) is what lets the fraction mean what it says, and
	 * it still leaves the throw fully charged at every fraction this gate can be pointed at.
	 */
	static constexpr float GateMinChargeHoldSeconds = 0.70f;

	/**
	 * *** THE CEILING OVER THE HOLD, AND IT IS A GAMEPLAY RULE, NOT A TASTE. ***
	 *
	 * SPEC v28 §7: a FULL charge may only be sat on for GetThrowAutoReleaseSeconds() before THE SERVER
	 * THROWS IT ITSELF. So the longest hold this gate can actually spend is
	 * GetThrowChargeSeconds() + GetThrowAutoReleaseSeconds() — 0.60 + 0.60 = 1.20 s at the shipped
	 * knobs — and a hold longer than that does not delay the release at all: it hands the release back
	 * to the server's clock, which is precisely the open-loop timing this whole gate exists to remove.
	 * The gate would then be measuring the auto-release and reporting it as its own aim.
	 *
	 * WHO THIS BITES, MEASURED AGAINST THE ARITHMETIC RATHER THAN GUESSED: the shipped arm holds about
	 * 0.77 s and never comes near it. THE RED ARM DOES. Aimed at 1.60 of the fall it wants a 1.48 s
	 * hold, and pressing at take-off would put the server's auto-release at take-off + 1.20 s against
	 * a flight of about 1.14 s — six hundredths of a second of margin, so a jump off a ledge (a longer
	 * flight than the take-off predicts, the mirror image of the failure this pass is fixing) would
	 * have thrown the red arm AIRBORNE AND DESCENDING and printed a PASS over a deliberate failure.
	 * Capping the hold moves the PRESS later and leaves the aimed release where it is, so the red arm
	 * stays red and the aim keeps meaning what it says.
	 *
	 * The margin is subtracted from the live knobs rather than the constant being written out, because
	 * both of them are retunable by name and by CVar; 0 in the auto-release knob is v28 §7 switched
	 * off, and then there is no ceiling at all.
	 */
	static constexpr float GateAutoReleaseMarginSeconds = 0.15f;

	/** Below this, "descending" is a rounding error rather than the state the owner's note is about. */
	static constexpr float GateMinDescentUU = 100.f;

	/** How long past the planned release to keep watching for the throw before calling it a miss. */
	static constexpr double GateThrowTimeoutSeconds = 3.0;

	/**
	 * *** HOW MANY TIMES THE GATE STAGES THE THROW BEFORE IT CALLS THE RUN A FAILURE. ***
	 *
	 * This is the half of §7e that "fail rather than mislabel" did not buy, and the nine-run
	 * measurement quoted on GateDescendFraction is the argument for it. A miss is a STAGING accident
	 * — the pawn came down on top of the goal structure it was running at, so the flight was short
	 * and the release landed after touchdown — and a staging accident is not a fact about the game
	 * this walk exists to photograph. Neither of the verifier's two failures said anything about the
	 * Core, the throw or the owner's bug. They said the pawn was standing on a ledge.
	 *
	 * So a miss RE-ARMS instead of ending the run: take the Core back, wait for the pawn to be
	 * standing, run and jump again. Taking it back cannot deadlock behind the bot who caught the
	 * throw — Trace.DebugTakeCore goes through ATraceCore::TryPickup, which is documented in
	 * TraceCore.cpp as "an unconditional debug grant, and it takes the Core off whoever has it".
	 *
	 * WHAT KEEPS THIS HONEST, because a retry loop is exactly the shape a harness uses to hide a
	 * failure:
	 *   - every attempt is measured and logged on its own, at Warning level, naming what it threw
	 *     from and (where the pawn landed early) why;
	 *   - the verdict states WHICH ATTEMPT made the state and how many missed, so the pass rate stays
	 *     visible in the log instead of being smoothed away by the retry;
	 *   - if every attempt misses, the last one's frame and verdict are exactly what a single miss
	 *     used to produce — which is what keeps Trace.Integ.WalkCoreRedArm red;
	 *   - and Trace.Integ.WalkCoreRetryArm makes attempt 1 miss ON PURPOSE, so the recovery path is
	 *     run rather than described.
	 *
	 * Three, not ten: at 0.35 of the fall a miss needs the pawn to lose a third of its airtime, and
	 * three attempts from three different pieces of floor is already far below the 22 % one attempt
	 * measured. Ten would mostly buy a longer log.
	 */
	static constexpr int32 GateMaxAttempts = 3;

	/**
	 * How long a re-arm waits for the Core to come back and the pawn to be standing on the ground
	 * again before it gives up. The grant itself lands within a frame or two; the wait that can
	 * actually take time is the pawn's own landing after a RISING miss, or a respawn.
	 */
	static constexpr double GateRearmTimeoutSeconds = 6.0;

	/**
	 * The retry's run-up: W is held for GateRearmRunHoldSeconds and the jump goes in
	 * GateRearmRunUpSeconds into it.
	 *
	 * The first attempt gets its horizontal term from the walk's own 2.6 s W step, which has long
	 * since ended by the time a retry starts, so a retry has to make its own — the owner's note is
	 * "I still want the core to carry the player's velocity", and a standing throw would prove only
	 * half of it. 0.45 s is past the acceleration ramp at 976 uu/s of walk speed. And if the pawn is
	 * standing against the wall it just hit, W does nothing and the retry becomes a clean vertical
	 * jump — which is the most deterministic case there is, because a vertical jump lands exactly
	 * where it took off and the ballistic solve is then exact rather than optimistic.
	 */
	static constexpr float GateRearmRunHoldSeconds = 1.60f;
	static constexpr double GateRearmRunUpSeconds = 0.45;

	/** SPEC v32 §7e. Everything one armed gate carries between polls. */
	struct FCoreThrowGate
	{
		TWeakObjectPtr<APlayerController> PC;
		FString Tag;

		enum class EPhase : uint8
		{
			/** RE-ARM ONLY: the Core has been asked for back; waiting to hold it and be standing. */
			WaitCarrier,
			/** RE-ARM ONLY: running forward, so the retry's throw carries a horizontal term too. */
			WaitRunUp,
			/** Jump pressed; waiting for the movement component to say the pawn actually left. */
			WaitAirborne,
			/** Take-off measured, release solved; waiting for the moment to press. */
			WaitPress,
			/** Held; waiting for ATraceCore::LastThrow to record a launch. */
			WaitThrow,
		};
		EPhase Phase = EPhase::WaitAirborne;

		/** All absolute FPlatformTime::Seconds. Real time, for the reason Start() gives about pause. */
		double ArmedAt = 0.0;
		double PressAt = 0.0;
		double ReleaseAt = 0.0;

		/** When the current re-arm began (its timeout) and when its run-up ends in a jump. */
		double RearmAt = 0.0;
		double JumpAt = 0.0;

		/**
		 * DERIVED PER ATTEMPT from the solved release, floored at GateMinChargeHoldSeconds — never a
		 * constant any more. See GateMinChargeHoldSeconds for why a fixed hold silently overrode
		 * GateDescendFraction.
		 */
		float HoldSeconds = GateMinChargeHoldSeconds;

		/** Which staged attempt this is, and how many have missed. See GateMaxAttempts. */
		int32 Attempt = 1;
		int32 Misses = 0;

		/** One clause per missed attempt, so the final verdict can name all of them in one line. */
		FString MissRecord;

		/**
		 * Set when the pawn is seen back on the ground BEFORE the release was due — the mechanism
		 * behind both of the verifier's failures, and worth naming in the log rather than leaving the
		 * reader to infer it from a velocity of zero. Cleared on every re-arm.
		 */
		bool bLandedBeforeRelease = false;
		double LandedEarlyBySeconds = 0.0;

		/**
		 * Trace.Integ.WalkCoreRetryArm: attempt 1 aims past the landing on purpose and the re-arm
		 * puts the aim back where the shipped gate has it. The red arm OF THE RETRY, so the recovery
		 * path is exercised rather than asserted.
		 */
		bool bMissFirstAttemptOnPurpose = false;

		/**
		 * Where in the fall the release is aimed. Shipped value GateDescendFraction; the red arm
		 * moves it past the landing. SNAPSHOTTED INTO THE GATE at arm time rather than read from the
		 * file static on every poll, so two overlapping walks cannot swap arms halfway.
		 */
		float DescendFraction = GateDescendFraction;

		/** SPEC v29 §6: a throw is detected by this moving, never by LastThrow's values changing. */
		int32 SerialBeforeThrow = 0;

		/** The measured take-off, kept so the verdict line can print what the plan was solved from. */
		float TakeoffVelocityZ = 0.f;
		float TimeToApexSeconds = 0.f;
	};

	/** The local pawn, re-resolved every poll: a respawn mid-gate must not be photographed as a throw. */
	static ATraceCharacter* GateLocalPawn(const FCoreThrowGate& Gate)
	{
		APlayerController* const GatePC = Gate.PC.Get();
		return (GatePC != nullptr) ? Cast<ATraceCharacter>(GatePC->GetPawn()) : nullptr;
	}

	/**
	 * One console command from inside the gate, through the local controller and logged the way
	 * RunStep logs its own — a re-arm has to be readable in the same column as the walk that armed it.
	 */
	static void GateExec(const FCoreThrowGate& Gate, const TCHAR* Command)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[IntegWalk] EXEC %s"), Command);
		if (APlayerController* const ExecPC = Gate.PC.Get())
		{
			ExecPC->ConsoleCommand(Command, /*bWriteToLog=*/true);
		}
	}

	/**
	 * One key press from inside the gate, through the SAME Trace.SimInput route every other step of
	 * this walk uses. A re-arm that reached past the input pipeline would be staging the throw by a
	 * different road than the run it is retrying, and the two would stop being comparable.
	 */
	static void GateKey(const FCoreThrowGate& Gate, const TCHAR* Key, float Hold)
	{
		UE_LOG(LogTraceGame, Display, TEXT("[IntegWalk] KEY %s (%.2fs, controller) — gate re-arm."), Key, Hold);
		if (APlayerController* const KeyPC = Gate.PC.Get())
		{
			KeyPC->ConsoleCommand(FString::Printf(TEXT("Trace.SimInput %s %.2f controller"), Key, Hold),
				/*bWriteToLog=*/false);
		}
	}

	/** One deferred stop on the gate's tail, relative to the measured launch rather than to the walk. */
	static void GateAfter(double Delay, TFunction<void()> What)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[What = MoveTemp(What)](float) -> bool
			{
				What();
				return false;
			}), static_cast<float>(FMath::Max(0.01, Delay)));
	}

	/**
	 * The flight tail, anchored on the launch this gate MEASURED.
	 *
	 * The offsets are the ones the timer list used (+0.10 / +0.25 / +0.85 / +1.85 / +2.65 / +2.85 /
	 * +3.05 from the release), so the frames are directly comparable with every v31 run already on
	 * disk. What changed is only what they are anchored to. Two flight frames a second apart, because
	 * a Core that dropped at the thrower's feet and a Core that went forward look identical in one
	 * frame and nothing alike in two.
	 */
	static void GateRunTail(const FCoreThrowGate& Gate, const FString& ThrowFrameName)
	{
		const FString Tag = Gate.Tag;
		TWeakObjectPtr<APlayerController> WeakPC = Gate.PC;

		auto Exec = [WeakPC](const TCHAR* Command)
		{
			UE_LOG(LogTraceGame, Display, TEXT("[IntegWalk] EXEC %s"), Command);
			if (APlayerController* const ExecPC = WeakPC.Get())
			{
				ExecPC->ConsoleCommand(Command, /*bWriteToLog=*/true);
			}
		};

		GateAfter(0.10, [Tag, ThrowFrameName]() { TakeShot(Tag, ThrowFrameName); });
		GateAfter(0.25, [Exec]() { Exec(TEXT("Trace.ModeB.ThrowMomentum")); });
		GateAfter(0.85, [Tag]() { TakeShot(Tag, TEXT("62_core_in_flight")); });
		GateAfter(1.85, [Tag]() { TakeShot(Tag, TEXT("63_core_in_flight_later")); });
		GateAfter(2.65, [Exec]() { Exec(TEXT("Trace.ModeB.CoreProbe")); });
		GateAfter(2.85, [Tag]() { TakeShot(Tag, TEXT("64_core_settled")); });
		GateAfter(3.05, [Exec]() { Exec(TEXT("Trace.ModeB.FlightLog 0")); });
	}

	/**
	 * THE ONE LINE A SCRIPT GREPS FOR. Error level, so a run that misses cannot be read as a run that
	 * passed by anyone skimming Display lines — which is exactly how the v31 miss got past a reader.
	 */
	static void GateLogFail(const FString& Why)
	{
		UE_LOG(LogTraceGame, Error, TEXT("[IntegWalk] GATE VERDICT: FAIL — %s"), *Why);
	}

	/**
	 * The gate gave up BEFORE a throw happened, so there is no flight to photograph and no tail to
	 * run. Says so, takes one frame under a name that ADMITS what it is, and turns the flight log off
	 * so the run does not leave the machine chattering.
	 *
	 * *** THE FRAME IS STILL TAKEN. *** A miss with no picture is a miss nobody can diagnose, and the
	 * v31 failure this replaces was not "no frame" — it was a frame filed under the wrong sentence.
	 */
	static void GateAbort(const FCoreThrowGate& Gate, const FString& FrameName, const FString& Why)
	{
		GateLogFail(Why);
		TakeShot(Gate.Tag, FrameName);
		const TWeakObjectPtr<APlayerController> WeakPC = Gate.PC;
		GateAfter(0.40, [WeakPC]()
		{
			if (APlayerController* const ExecPC = WeakPC.Get())
			{
				ExecPC->ConsoleCommand(TEXT("Trace.ModeB.FlightLog 0"), /*bWriteToLog=*/true);
			}
		});
	}

	/** One poll. Returns true to keep the ticker registered, false when the gate is finished. */
	static bool TickCoreThrowGate(FCoreThrowGate& Gate)
	{
		const double Now = FPlatformTime::Seconds();
		ATraceCharacter* const Pawn = GateLocalPawn(Gate);
		UCharacterMovementComponent* const Move = (Pawn != nullptr) ? Pawn->GetCharacterMovement() : nullptr;

		switch (Gate.Phase)
		{
		case FCoreThrowGate::EPhase::WaitCarrier:
		{
			// THE RE-ARM'S OWN PRECONDITION, ASKED OF THE PAWN. Trace.DebugTakeCore self-schedules on
			// the core ticker until the grant lands, so this waits for the FACT (IsCarrier) rather
			// than for the command that asked for it — and it waits for the pawn to be standing as
			// well, because jumping again out of the tail of the last jump would stage a different
			// throw from the one the first attempt staged.
			if (Pawn != nullptr && Pawn->IsCarrier() && Move != nullptr && !Move->IsFalling())
			{
				GateKey(Gate, TEXT("W"), GateRearmRunHoldSeconds);
				Gate.JumpAt = Now + GateRearmRunUpSeconds;
				Gate.Phase = FCoreThrowGate::EPhase::WaitRunUp;
				return true;
			}

			if (Now - Gate.RearmAt > GateRearmTimeoutSeconds)
			{
				GateAbort(Gate, TEXT("61_gate_FAILED_no_rearm"), FString::Printf(
					TEXT("attempt %d of %d could not be staged: %.1fs after Trace.DebugTakeCore the local ")
					TEXT("pawn is %s and %s. Earlier attempts: %s"),
					Gate.Attempt, GateMaxAttempts, GateRearmTimeoutSeconds,
					(Pawn == nullptr) ? TEXT("gone (dead or respawning)")
						: (Pawn->IsCarrier() ? TEXT("carrying the Core") : TEXT("NOT carrying the Core")),
					(Move != nullptr && Move->IsFalling()) ? TEXT("still in the air") : TEXT("on the ground"),
					Gate.MissRecord.IsEmpty() ? TEXT("none") : *Gate.MissRecord));
				return false;
			}
			return true;
		}

		case FCoreThrowGate::EPhase::WaitRunUp:
		{
			if (Now < Gate.JumpAt)
			{
				return true;
			}

			GateKey(Gate, TEXT("SpaceBar"), 0.10f);
			UE_LOG(LogTraceGame, Display,
				TEXT("[IntegWalk] GATE ATTEMPT %d of %d: Core back in hand, %.2fs of run-up behind it, ")
				TEXT("jumping again. The take-off is measured from scratch — nothing is carried over ")
				TEXT("from the attempt that missed."),
				Gate.Attempt, GateMaxAttempts, GateRearmRunUpSeconds);

			Gate.ArmedAt = Now;
			Gate.Phase = FCoreThrowGate::EPhase::WaitAirborne;
			return true;
		}

		case FCoreThrowGate::EPhase::WaitAirborne:
		{
			// *** THE CONDITION, ASKED OF THE MOVEMENT COMPONENT AND NOT OF THE CLOCK. *** This is the
			// whole §7e fix in one line: the old walk assumed a key press became a take-off 0.30 s ago.
			if (Move != nullptr && Move->IsFalling())
			{
				// Solved from the LIVE numbers on the first airborne frame. GetGravityZ() is negative,
				// so the down-magnitude is its negation; guarded at 1 because a zero-gravity volume
				// would otherwise divide by nothing and place the release at infinity.
				const float VelocityZAtTakeoff = static_cast<float>(Pawn->GetVelocity().Z);
				const float GravityDown = FMath::Max(1.f, -Move->GetGravityZ());
				const float ToApex = FMath::Max(0.f, VelocityZAtTakeoff / GravityDown);

				Gate.TakeoffVelocityZ = VelocityZAtTakeoff;
				Gate.TimeToApexSeconds = ToApex;

				// *** THE RELEASE IS SOLVED FIRST AND THE HOLD IS DERIVED FROM IT. *** The other way
				// round — a constant hold, with the press placed to suit it — is what pinned every
				// release at take-off + 0.85 s and made GateDescendFraction below ~0.5 unreachable.
				// See GateMinChargeHoldSeconds.
				const double ReleaseDelay =
					static_cast<double>(ToApex) * (1.0 + static_cast<double>(Gate.DescendFraction));
				Gate.ReleaseAt = Now + ReleaseDelay;

				// The ceiling is read off the LIVE charge knobs, never written out here — see
				// GateAutoReleaseMarginSeconds. 0 in the auto-release knob is spec v28 §7 switched off,
				// and then a full charge can be sat on forever and there is no ceiling to respect.
				const float AutoReleaseSeconds = ATraceCore::GetThrowAutoReleaseSeconds();
				const float MaxHoldSeconds = (AutoReleaseSeconds > 0.f)
					? FMath::Max(GateMinChargeHoldSeconds,
						ATraceCore::GetThrowChargeSeconds() + AutoReleaseSeconds - GateAutoReleaseMarginSeconds)
					: TNumericLimits<float>::Max();

				Gate.HoldSeconds = FMath::Clamp(static_cast<float>(ReleaseDelay),
					GateMinChargeHoldSeconds, MaxHoldSeconds);
				Gate.PressAt = Gate.ReleaseAt - static_cast<double>(Gate.HoldSeconds);

				if (static_cast<float>(ReleaseDelay) > MaxHoldSeconds)
				{
					// Not a warning: the aimed release is unchanged, only the press moved later. Said
					// out loud anyway, because "the press is not at take-off" is a real difference in
					// what the run staged and the next reader should not have to derive it.
					UE_LOG(LogTraceGame, Display,
						TEXT("[IntegWalk] GATE: the aimed release is %.2fs out, past the %.2fs the server ")
						TEXT("would auto-release a full charge in (v28 §7). Holding %.2fs and pressing ")
						TEXT("%.2fs after take-off instead, so the RELEASE stays where it was aimed."),
						ReleaseDelay, MaxHoldSeconds, Gate.HoldSeconds, Gate.PressAt - Now);
				}

				// DOES THE CHARGE FLOOR ACTUALLY BIND? Asked with a millisecond of slack and asked
				// SEPARATELY from the clamp below, because the two are not the same event and saying
				// so wrongly is a lie in the log. When the aimed release is longer than the floor the
				// press lands ON the take-off frame, and PressAt then differs from Now only by the
				// rounding of one double subtraction — which still trips `PressAt < Now` and would
				// otherwise print "inside the 0.70s charge floor" about a 0.78 s release that is
				// nowhere near it. Only a genuinely short jump binds the floor.
				const bool bChargeFloorBinds =
					ReleaseDelay < static_cast<double>(Gate.HoldSeconds) - 0.001;

				if (Gate.PressAt < Now)
				{
					Gate.PressAt = Now;
					Gate.ReleaseAt = Now + static_cast<double>(Gate.HoldSeconds);
				}

				if (bChargeFloorBinds)
				{
					// The aimed release is sooner than a full charge takes, so the two cannot both be
					// had. The press has just been pulled to now and the full hold kept rather than
					// clipped: a clipped hold is a different (weaker) throw and every flight number
					// under it would mean something else. Said out loud, because it means the plan
					// could not be met — the release slides later in the fall than the fraction asked
					// for, and the verdict below judges where it actually landed, not where it was
					// aimed.
					UE_LOG(LogTraceGame, Warning,
						TEXT("[IntegWalk] GATE: airborne at +%.0f uu/s puts the aimed release %.2fs away, ")
						TEXT("inside the %.2fs charge floor. Pressing immediately and keeping the full hold, ")
						TEXT("so the release lands later in the fall than %.0f%% and may land after ")
						TEXT("touchdown; the verdict will say so rather than assume."),
						VelocityZAtTakeoff, ReleaseDelay, GateMinChargeHoldSeconds,
						100.f * Gate.DescendFraction);
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[IntegWalk] GATE ATTEMPT %d of %d: AIRBORNE measured at +%.0f uu/s against ")
					TEXT("%.0f uu/s of gravity -> apex in %.2fs, so a symmetric flight is %.2fs. Releasing ")
					TEXT("%.0f%% of the way back down (%.2fs past the apex, ~%.0f uu/s descending, %.2fs of ")
					TEXT("airtime behind it), so pressing in %.2fs and holding %.2fs."),
					Gate.Attempt, GateMaxAttempts, VelocityZAtTakeoff, GravityDown, ToApex, 2.f * ToApex,
					100.f * Gate.DescendFraction, ToApex * Gate.DescendFraction,
					-GravityDown * ToApex * Gate.DescendFraction,
					2.f * ToApex - static_cast<float>(Gate.ReleaseAt - Now),
					Gate.PressAt - Now, Gate.HoldSeconds);

				Gate.Phase = FCoreThrowGate::EPhase::WaitPress;
				return true;
			}

			if (Now - Gate.ArmedAt > GateAirborneTimeoutSeconds)
			{
				GateAbort(Gate, TEXT("61_gate_FAILED_never_airborne"), FString::Printf(
					TEXT("the pawn never left the ground within %.1fs of the jump press (pawn %s, ")
					TEXT("movement %s). No throw was made, because a throw made here would be exactly ")
					TEXT("the ON-GROUND frame the v31 run mislabelled as \"thrown_while_falling\"."),
					GateAirborneTimeoutSeconds,
					(Pawn != nullptr) ? *GetNameSafe(Pawn) : TEXT("<none>"),
					(Move != nullptr) ? TEXT("present") : TEXT("<none>")));
				return false;
			}
			return true;
		}

		case FCoreThrowGate::EPhase::WaitPress:
		{
			if (Now < Gate.PressAt)
			{
				return true;
			}

			// SAMPLED BEFORE THE PRESS, never after: the throw can be recorded on the very next frame
			// and a serial read afterwards would already include it.
			Gate.SerialBeforeThrow = ATraceCore::LastThrow.Serial;

			if (APlayerController* const PressPC = Gate.PC.Get())
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[IntegWalk] KEY LeftMouseButton (%.2fs, controller) — gated, release aimed at ")
					TEXT("%.2fs past the measured apex."),
					Gate.HoldSeconds, Gate.TimeToApexSeconds * Gate.DescendFraction);
				PressPC->ConsoleCommand(
					FString::Printf(TEXT("Trace.SimInput LeftMouseButton %.2f controller"), Gate.HoldSeconds),
					/*bWriteToLog=*/false);
			}
			else
			{
				GateAbort(Gate, TEXT("61_gate_FAILED_no_controller"),
					TEXT("the local player controller went away between the jump and the throw."));
				return false;
			}

			Gate.Phase = FCoreThrowGate::EPhase::WaitThrow;
			return true;
		}

		case FCoreThrowGate::EPhase::WaitThrow:
		{
			// *** THE MECHANISM BEHIND EVERY MISS THIS GATE HAS EVER PRODUCED, NAMED AS IT HAPPENS. ***
			//
			// The hold cannot be recalled once Trace.SimInput has scheduled its release, so seeing the
			// pawn back on the floor here does not save the attempt — but it does turn "thrown at 0
			// uu/s vertical" from a symptom into a diagnosis, and it is the line that tells the reader
			// the arena, not the harness's clock, is what moved. Sampled BEFORE the release is due,
			// which is the only window in which it means anything.
			if (!Gate.bLandedBeforeRelease && Now < Gate.ReleaseAt && Move != nullptr && !Move->IsFalling())
			{
				Gate.bLandedBeforeRelease = true;
				Gate.LandedEarlyBySeconds = Gate.ReleaseAt - Now;
				UE_LOG(LogTraceGame, Warning,
					TEXT("[IntegWalk] GATE ATTEMPT %d of %d: THE PAWN IS BACK ON THE GROUND %.2fs BEFORE ")
					TEXT("the release is due. The jump was solved as a %.2fs symmetric flight, so it has ")
					TEXT("come down on something higher than it took off from. This attempt is already ")
					TEXT("lost — the release cannot be recalled — and the gate will re-arm on it."),
					Gate.Attempt, GateMaxAttempts, Gate.LandedEarlyBySeconds, 2.f * Gate.TimeToApexSeconds);
			}

			// *** THE GROUND TRUTH. *** ThrowFromHolder writes the thrower's own velocity and falling
			// bit into LastThrow at the instant of launch. Everything below is read from THAT rather
			// than re-sampled here, because "what was the pawn doing when it let go" is a question
			// about a moment that has already passed by the time any poll can ask it.
			if (ATraceCore::LastThrow.Serial == Gate.SerialBeforeThrow)
			{
				if (Now > Gate.ReleaseAt + GateThrowTimeoutSeconds)
				{
					GateAbort(Gate, TEXT("61_gate_FAILED_no_throw"), FString::Printf(
						TEXT("no throw was recorded within %.1fs of the planned release ")
						TEXT("(ATraceCore::LastThrow.Serial still %d). The Core was probably never taken, ")
						TEXT("or LMB is no longer THROW. Nothing was photographed as a throw."),
						GateThrowTimeoutSeconds, Gate.SerialBeforeThrow));
					return false;
				}
				return true;
			}

			const ATraceCore::FThrowMomentumSample& Sample = ATraceCore::LastThrow;
			const bool bDescending = Sample.bThrowerFalling && (Sample.ThrowerVelocityZ <= -GateMinDescentUU);

			// THE FRAME IS NAMED BY WHAT HAPPENED, NEVER BY WHAT WAS ASKED FOR. That sentence is the
			// §7e fix; the three names below are the only place it can be spent.
			const TCHAR* const FrameName = bDescending
				? TEXT("61_thrown_while_falling")
				: (Sample.bThrowerFalling ? TEXT("61_thrown_while_RISING_MISS") : TEXT("61_thrown_ON_GROUND_MISS"));

			if (bDescending)
			{
				// THE ATTEMPT COUNT IS IN THE PASS LINE, NOT JUST IN THE FAILURES. A gate that retries
				// and then prints a bare "PASS" has hidden its own pass rate, which is the thing §7e is
				// actually about; a reader must be able to see from one line whether this run needed
				// one jump or three.
				UE_LOG(LogTraceGame, Display,
					TEXT("[IntegWalk] GATE VERDICT: PASS on attempt %d of %d (%d missed and re-armed%s%s) ")
					TEXT("— thrown AIRBORNE and DESCENDING at %.0f uu/s (take-off was +%.0f uu/s, apex ")
					TEXT("%.2fs later; held %.2fs -> charge x%.2f). Frame filed as %s."),
					Gate.Attempt, GateMaxAttempts, Gate.Misses,
					Gate.Misses > 0 ? TEXT(": ") : TEXT(""),
					Gate.Misses > 0 ? *Gate.MissRecord : TEXT(""),
					Sample.ThrowerVelocityZ, Gate.TakeoffVelocityZ, Gate.TimeToApexSeconds,
					Sample.HeldSeconds, Sample.ChargeScale, FrameName);
				GateRunTail(Gate, FrameName);
				return false;
			}

			// ---- A MISS. STAGE IT AGAIN IF THERE IS AN ATTEMPT LEFT (see GateMaxAttempts) ----------
			++Gate.Misses;
			// The separator LEADS rather than trails, so the record never ends in a dangling "; " and
			// the verdict line it is pasted into closes its own bracket cleanly.
			Gate.MissRecord += FString::Printf(
				TEXT("%sattempt %d threw %s at %.0f uu/s%s"),
				Gate.MissRecord.IsEmpty() ? TEXT("") : TEXT("; "),
				Gate.Attempt,
				Sample.bThrowerFalling ? TEXT("AIRBORNE RISING") : TEXT("ON THE GROUND"),
				Sample.ThrowerVelocityZ,
				Gate.bLandedBeforeRelease
					? *FString::Printf(TEXT(" (down %.2fs early)"), Gate.LandedEarlyBySeconds)
					: TEXT(""));

			if (Gate.Attempt < GateMaxAttempts)
			{
				UE_LOG(LogTraceGame, Warning,
					TEXT("[IntegWalk] GATE ATTEMPT %d of %d MISSED — the throw was released with the ")
					TEXT("thrower %s at %.0f uu/s vertical, not descending (the gate wanted falling and at ")
					TEXT("least %.0f uu/s down). %s RE-ARMING: taking the Core back and jumping again. ")
					TEXT("This is a STAGING miss, not a verdict — see GateMaxAttempts — and the run still ")
					TEXT("FAILS if every attempt misses."),
					Gate.Attempt, GateMaxAttempts,
					Sample.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("ON THE GROUND"),
					Sample.ThrowerVelocityZ, GateMinDescentUU,
					Gate.bLandedBeforeRelease
						? TEXT("The pawn was seen landing before the release was due, so the flight was ")
						  TEXT("shorter than the take-off predicted — it came down on higher ground.")
						: TEXT("The pawn was never seen landing early, so the release itself, not the ")
						  TEXT("landing, is what fell outside the window."));

				// The retry arm's deliberate miss is spent on attempt 1 and must not be repeated, or
				// the arm would prove only that a gate aimed at the floor keeps hitting the floor.
				// Asked of the attempt NUMBER rather than of the fraction's value: comparing two
				// floats for equality to decide control flow is how a knob at 0.3500001 turns a red
				// arm green with nothing in the log to show for it.
				if (Gate.bMissFirstAttemptOnPurpose && Gate.Attempt == 1)
				{
					Gate.DescendFraction = GateDescendFraction;
					UE_LOG(LogTraceGame, Display,
						TEXT("[IntegWalk] *** RETRY ARM: the deliberate miss is spent. The re-arm aims at ")
						TEXT("the shipped %.2f of the fall, so a PASS from here is the recovery path ")
						TEXT("working and nothing else. ***"), GateDescendFraction);
				}

				++Gate.Attempt;
				Gate.Phase = FCoreThrowGate::EPhase::WaitCarrier;
				Gate.RearmAt = Now;
				Gate.bLandedBeforeRelease = false;
				Gate.LandedEarlyBySeconds = 0.0;

				GateExec(Gate, TEXT("Trace.DebugTakeCore"));
				return true;
			}

			// *** THE TAIL STILL RUNS ON THE LAST MISS, AND THAT IS DELIBERATE. *** A throw DID happen,
			// so there is a flight, and the flight frames are the evidence for what a Core thrown from
			// the wrong state actually does — which is the picture the owner's note is about. The tail
			// takes the frame (under the MISS name) and turns the flight log off.
			GateLogFail(FString::Printf(
				TEXT("ALL %d STAGED ATTEMPTS MISSED. The last throw was released with the thrower %s at ")
				TEXT("%.0f uu/s vertical, not descending (the gate wanted falling and at least %.0f uu/s ")
				TEXT("down). Take-off was +%.0f uu/s with the apex %.2fs later, and the release was aimed ")
				TEXT("%.2fs past it. Every attempt: %s. EVERY STAGED JUMP, FROM A DIFFERENT PIECE OF ")
				TEXT("FLOOR, FAILED TO REACH THE STATE — WHICH IS A FAILURE AND NOT THE v31 ")
				TEXT("NON-DETERMINISM RE-LABELLED."),
				GateMaxAttempts,
				Sample.bThrowerFalling ? TEXT("AIRBORNE") : TEXT("ON THE GROUND"),
				Sample.ThrowerVelocityZ, GateMinDescentUU, Gate.TakeoffVelocityZ,
				Gate.TimeToApexSeconds, Gate.TimeToApexSeconds * Gate.DescendFraction, *Gate.MissRecord));
			GateRunTail(Gate, FrameName);
			return false;
		}

		default:
			return false;
		}
	}

	/**
	 * ONE-SHOT, set by Trace.Integ.WalkCoreRedArm and cleared by the arm it applies to.
	 *
	 * A one-shot rather than a mode: a red arm left latched would quietly turn every later Core walk
	 * in the same process into a failing one, and this file has already been bitten once by a global
	 * that outlived the run that set it (see TakeShot's note on GTag).
	 */
	static bool GGateRedArmOnce = false;

	/**
	 * ONE-SHOT, set by Trace.Integ.WalkCoreRetryArm. THE RED ARM OF THE RETRY, which is a different
	 * claim from the red arm above and needs its own arm to prove it.
	 *
	 * GGateRedArmOnce proves the gate can still FAIL. It cannot prove the retry RECOVERS, because
	 * every one of its attempts is aimed past the landing on purpose. This one makes only ATTEMPT 1
	 * miss and lets the re-arm aim where the shipped gate aims, so the run must print
	 * "GATE ATTEMPT 1 of 3 MISSED" and then "GATE VERDICT: PASS on attempt 2". A first-time PASS means
	 * the deliberate miss did not bite; a FAIL means the recovery path does not work.
	 */
	static bool GGateFirstAttemptMissOnce = false;

	/** SPEC v32 §7e. Presses the jump and hands the rest of the Core walk to TickCoreThrowGate. */
	static void ArmCoreThrowGate(APlayerController* PC, const FString& Tag)
	{
		TSharedRef<FCoreThrowGate> Gate = MakeShared<FCoreThrowGate>();
		Gate->PC = PC;
		Gate->Tag = Tag;
		Gate->ArmedAt = FPlatformTime::Seconds();

		if (GGateRedArmOnce)
		{
			GGateRedArmOnce = false;
			Gate->DescendFraction = GateRedArmDescendFraction;
			UE_LOG(LogTraceGame, Display,
				TEXT("[IntegWalk] *** RED ARM (v32 s7e): the release is aimed at %.2f of the fall, i.e. ")
				TEXT("PAST THE LANDING, instead of the shipped %.2f, on EVERY one of the %d attempts. THE ")
				TEXT("GATE MUST NOW PRINT \"GATE VERDICT: FAIL\" AND FILE THE FRAME AS ")
				TEXT("61_thrown_ON_GROUND_MISS. ***"),
				GateRedArmDescendFraction, GateDescendFraction, GateMaxAttempts);
		}
		else if (GGateFirstAttemptMissOnce)
		{
			GGateFirstAttemptMissOnce = false;
			Gate->bMissFirstAttemptOnPurpose = true;
			Gate->DescendFraction = GateRedArmDescendFraction;
			UE_LOG(LogTraceGame, Display,
				TEXT("[IntegWalk] *** RETRY ARM (v32 s7e): ATTEMPT 1 ONLY is aimed at %.2f of the fall, ")
				TEXT("i.e. PAST THE LANDING, so it must miss; the re-arm then aims at the shipped %.2f. ")
				TEXT("THE GATE MUST NOW PRINT \"GATE ATTEMPT 1 of %d MISSED\" AND THEN \"GATE VERDICT: ")
				TEXT("PASS on attempt 2\". ***"),
				GateRedArmDescendFraction, GateDescendFraction, GateMaxAttempts);
		}

		UE_LOG(LogTraceGame, Display,
			TEXT("[IntegWalk] KEY SpaceBar (0.10s, controller) — GATE ARMED (v32 s7e): the throw is now ")
			TEXT("placed off the MEASURED take-off, and the frame is named off ATraceCore::LastThrow."));

		if (PC != nullptr)
		{
			PC->ConsoleCommand(TEXT("Trace.SimInput SpaceBar 0.10 controller"), /*bWriteToLog=*/false);
		}
		else
		{
			UE_LOG(LogTraceGame, Error,
				TEXT("[IntegWalk] GATE VERDICT: FAIL — no local controller to jump with."));
			return;
		}

		// Interval 0, i.e. once per frame: the release has to be placed inside a window of tens of
		// milliseconds and a coarser poll would quantise it back into the problem this replaces.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Gate](float) -> bool
		{
			return TickCoreThrowGate(Gate.Get());
		}), 0.f);
	}

	/** Runs one step. Kept off the timer lambda so the two paths log identically. */
	static void RunStep(APlayerController* PC, const FStep& Step, const FString& Tag)
	{
		// SPEC v32 §7e. Checked FIRST: a gate step carries no key, no shot and no exec, and falling
		// through to the key branch would log "KEY  skipped" and lose the rest of the walk in silence.
		if (Step.bCoreThrowGate)
		{
			ArmCoreThrowGate(PC, Tag);
			return;
		}

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
		TEXT("Spec v31 §2 / v32 §7e: takes the Core, runs, JUMPS and releases a charged throw a "
		     "measured fraction of the way back down from the apex — the owner's \"thrown while going "
		     "down\" case, through the real left-mouse route — then photographs the flight. Re-stages "
		     "the jump up to three times if the arena cuts a flight short. Mode B only. No arguments."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World)
			{
				GTag = TEXT("v31core");
				Start(World, TEXT("Core"));
			}));

	// SPEC v32 §7e / §8. THE RED ARM OF THE CORE WALK, AS A COMMAND, SO IT IS RUN AND NOT DESCRIBED.
	//
	// Identical to Trace.Integ.WalkCore in every respect except where the release is aimed: past the
	// landing instead of a third of the way down, on every attempt it makes. Same walk, same gate,
	// same verdict code — so if this prints a PASS, the gate is not reading the state it claims to
	// read, and the §7e complaint ("silently mislabelling") is back. IT ALSO PROVES THE RETRY CANNOT
	// MANUFACTURE A PASS: three staged attempts, all aimed at the floor, must still end in one FAIL.
	// Its frames get their own tag for the reason WalkGuns spells out at length: a deliberately-
	// failing frame must never be able to overwrite a real one.
	static FAutoConsoleCommandWithWorld GWalkCoreRedArmCommand(
		TEXT("Trace.Integ.WalkCoreRedArm"),
		TEXT("Spec v32 s7e. The Core walk with every throw deliberately released AFTER the landing. The "
		     "gate MUST print \"GATE VERDICT: FAIL\" and file the frame as 61_thrown_ON_GROUND_MISS. "
		     "If it passes, the gate is vacuous. Mode B only. No arguments."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World)
			{
				GGateRedArmOnce = true;
				GTag = TEXT("v32coreRED");
				Start(World, TEXT("Core"));
			}));

	// SPEC v32 §7e / §8. THE RED ARM OF THE *RETRY*, AS A COMMAND, FOR THE SAME REASON.
	//
	// Trace.Integ.WalkCoreRedArm proves the gate can still fail. It cannot prove the gate RECOVERS,
	// because every attempt it makes is aimed at the floor. This one makes attempt 1 miss on purpose
	// and lets attempt 2 aim properly, so the machinery that turns a 22%-of-runs staging miss into a
	// pass is RUN rather than described. Its frames get their own tag so a deliberately-missed frame
	// can never overwrite a real one — see WalkGuns for the run that taught this project that lesson.
	static FAutoConsoleCommandWithWorld GWalkCoreRetryArmCommand(
		TEXT("Trace.Integ.WalkCoreRetryArm"),
		TEXT("Spec v32 s7e. The Core walk with the FIRST throw deliberately released after the landing "
		     "and every later one aimed normally. The gate MUST print \"GATE ATTEMPT 1 of 3 MISSED\" and "
		     "then \"GATE VERDICT: PASS on attempt 2\". If it passes first time the deliberate miss did "
		     "not bite; if it fails, the re-arm does not recover. Mode B only. No arguments."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World)
			{
				GGateFirstAttemptMissOnce = true;
				GTag = TEXT("v32coreRETRY");
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
				// The tag is bumped by the pass that re-photographs this walk, which is the whole
				// point of the two paragraphs above: v31integ_43_pistol_reload.png is the frame the
				// off-hand shard was FOUND in and it has to stay on disk saying that.
				GTag = bPackRig ? TEXT("v35shardFIX") : TEXT("v35shardFALLBACK");
				UE_LOG(LogTraceGame, Display,
					TEXT("[IntegWalk] hand rig is %s, so these frames are filed as %s_*."),
					bPackRig ? TEXT("THE PACK (SK_TraceHands)") : TEXT("THE PROCEDURAL CUBE FALLBACK"),
					*GTag);
				Start(World, TEXT("Guns"));
			}));

	// =============================================================================================
	// *** SPEC v32 §6 — THE BLOOM MEASUREMENT. "CHECK, DON'T REBUILD." ***
	// =============================================================================================
	//
	// The FX doc's whole bloom section is one sentence: "set the threshold low enough that the 1.5x
	// base intensity already blooms slightly, so the 5x discharge really punches." That is a claim
	// about a number in a RUNNING game, and the only honest way to answer it is to read the number
	// out of the running game — the arena's post-process volume is spawned at runtime by
	// ATraceArenaBuilder::BuildPostProcess, so a header default is a statement of intent and not
	// evidence that the volume in front of the camera carries it.
	//
	// WHY IT LIVES IN THE INTEGRATION-WALK FILE AND NOT IN THE ARENA BUILDER. It measures; it does not
	// build. Everything in this file exists to turn "the picture is right" into a number or a frame,
	// and this is the same job. It also keeps a diagnostic out of a 8500-line builder that has no
	// other reader-facing command in it, and it reads only public engine state — nothing here needs
	// to be a friend of anything.
	//
	// IT DOES NOT TOUCH THE FIDELITY TIERS. Trace.Arena.Fidelity.Bloom is a shipped knob with a
	// documented meaning (0-3, FFT at 3, never off) and is not this section's business.

	/** The FX doc's two materials, at the BASE multipliers it names. See Art/Pack/docs/unreal-fx_README.md. */
	struct FBloomBaseEmissive
	{
		const TCHAR* Slot;
		FColor Colour;
		float BaseMultiplier;
	};

	/**
	 * Does an emissive of this colour at this multiplier get past the bloom threshold?
	 *
	 * UE's bloom threshold is in LINEAR scene-colour units and the FX doc's colours are sRGB hex, so
	 * the conversion has to happen or the answer is wrong by the gamma curve — #25E6FF's green
	 * channel is 0.902 in sRGB and 0.787 in linear, which is the difference between "just crosses"
	 * and "just misses" at any threshold near 1. FColor -> FLinearColor does exactly that conversion.
	 *
	 * The brightest channel is the right quantity: bloom is thresholded per pixel on the scene colour,
	 * and a saturated cyan whose blue channel is 1.0 blooms on the blue channel whatever the red does.
	 */
	static float BloomBrightnessOf(const FBloomBaseEmissive& Emissive)
	{
		const FLinearColor Linear = FLinearColor(Emissive.Colour);
		return FMath::Max3(Linear.R, Linear.G, Linear.B) * Emissive.BaseMultiplier;
	}

	static FAutoConsoleCommandWithWorld GIntegBloomCommand(
		TEXT("Trace.Integ.Bloom"),
		TEXT("Spec v32 s6. Prints the LIVE BloomIntensity/BloomThreshold off every post-process volume "
		     "in this world and states whether the FX doc's 1.5x base emissive crosses the threshold. "
		     "Measures only — it changes nothing. No arguments, so it is safe to pass to -TraceExec=."),
		FConsoleCommandWithWorldDelegate::CreateStatic(
			[](UWorld* World)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("========== [Bloom] SPEC v32 s6 — LIVE POST-PROCESS, MEASURED =========="));

				if (World == nullptr)
				{
					UE_LOG(LogTraceGame, Warning, TEXT("[Bloom] no world."));
					return;
				}

				int32 Volumes = 0;
				bool bHaveEffective = false;
				float EffectiveIntensity = 0.f;
				float EffectiveThreshold = 0.f;
				float EffectivePriority = TNumericLimits<float>::Lowest();
				FString EffectiveName;

				for (TActorIterator<APostProcessVolume> It(World); It; ++It)
				{
					const APostProcessVolume* const Volume = *It;
					if (!IsValid(Volume))
					{
						continue;
					}
					++Volumes;

					const FPostProcessSettings& PP = Volume->Settings;
					UE_LOG(LogTraceGame, Display,
						TEXT("[Bloom] volume %-32s enabled=%d unbound=%d priority=%.1f blend=%.2f | ")
						TEXT("BloomIntensity override=%d value=%.3f | BloomThreshold override=%d value=%.3f | ")
						TEXT("method override=%d value=%d | sizeScale override=%d value=%.2f"),
						*GetNameSafe(Volume), Volume->bEnabled ? 1 : 0, Volume->bUnbound ? 1 : 0,
						Volume->Priority, Volume->BlendWeight,
						PP.bOverride_BloomIntensity ? 1 : 0, PP.BloomIntensity,
						PP.bOverride_BloomThreshold ? 1 : 0, PP.BloomThreshold,
						PP.bOverride_BloomMethod ? 1 : 0, static_cast<int32>(PP.BloomMethod),
						PP.bOverride_BloomSizeScale ? 1 : 0, PP.BloomSizeScale);

					// "The one in force" is the highest-priority ENABLED UNBOUND volume that actually
					// overrides bloom. Bounded volumes are skipped rather than guessed at: whether one
					// applies depends on where the camera is standing, and this command is not standing
					// anywhere. The arena's own volume is unbound (BuildPostProcess sets bUnbound), so
					// on this project that is always the one that matters.
					if (Volume->bEnabled && Volume->bUnbound && PP.bOverride_BloomThreshold
						&& Volume->Priority > EffectivePriority)
					{
						bHaveEffective = true;
						EffectivePriority = Volume->Priority;
						EffectiveIntensity = PP.BloomIntensity;
						EffectiveThreshold = PP.BloomThreshold;
						EffectiveName = GetNameSafe(Volume);
					}
				}

				if (Volumes == 0)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[Bloom] no APostProcessVolume in this world at all — the arena builder's ")
						TEXT("BuildPostProcess did not run (dedicated server?), so bloom is at engine defaults."));
					return;
				}

				if (!bHaveEffective)
				{
					UE_LOG(LogTraceGame, Warning,
						TEXT("[Bloom] %d volume(s), but none of them is an enabled UNBOUND volume that ")
						TEXT("overrides BloomThreshold. The threshold in force is the engine default."),
						Volumes);
					return;
				}

				UE_LOG(LogTraceGame, Display,
					TEXT("[Bloom] IN FORCE: %s -> BloomIntensity %.3f, BloomThreshold %.3f."),
					*EffectiveName, EffectiveIntensity, EffectiveThreshold);

				// THE FX DOC'S QUESTION, ANSWERED WITH THE FX DOC'S OWN NUMBERS.
				const FBloomBaseEmissive Bases[] =
				{
					{ TEXT("circuit_cyan"), FColor(0x25, 0xE6, 0xFF), 1.5f },
					{ TEXT("core_amber"),   FColor(0xFF, 0x8A, 0x1F), 1.4f },
				};

				bool bAllCross = true;
				for (const FBloomBaseEmissive& Base : Bases)
				{
					const float Brightness = BloomBrightnessOf(Base);
					// A NEGATIVE THRESHOLD IS NOT "a very low number", IT IS A DIFFERENT MODE. UE reads
					// anything below 0 as "every pixel contributes to bloom, weighted by brightness"
					// rather than "only pixels above the threshold" — so at a negative threshold the
					// question "does 1.5x cross it" has no crossing to do and the answer is yes by
					// construction. Both branches are printed with their reason so the log says WHICH.
					const bool bCrosses = (EffectiveThreshold < 0.f) || (Brightness > EffectiveThreshold);
					bAllCross = bAllCross && bCrosses;
					UE_LOG(LogTraceGame, Display,
						TEXT("[Bloom]   %s #%02X%02X%02X at the doc's base x%.2f = %.3f linear (brightest ")
						TEXT("channel) vs threshold %.3f -> %s%s"),
						Base.Slot, Base.Colour.R, Base.Colour.G, Base.Colour.B, Base.BaseMultiplier,
						Brightness, EffectiveThreshold,
						bCrosses ? TEXT("BLOOMS") : TEXT("DOES NOT BLOOM"),
						(EffectiveThreshold < 0.f)
							? TEXT(" (threshold is negative: every pixel contributes, weighted by brightness)")
							: TEXT(""));
				}

				// And the discharge, because "the 5.2x really punches" is the other half of the
				// sentence and it is only meaningful relative to the base that already blooms.
				const float CyanBase = BloomBrightnessOf(Bases[0]);
				UE_LOG(LogTraceGame, Display,
					TEXT("[Bloom]   headroom: the pistol's 5.2x discharge is %.3f linear, %.1fx the base ")
					TEXT("that already blooms — which is the \"punch\" the FX doc is asking for."),
					CyanBase * (5.2f / 1.5f), 5.2f / 1.5f);

				UE_LOG(LogTraceGame, Display,
					TEXT("[Bloom] VERDICT: the 1.5x base emissive %s at the live threshold %.3f. %s"),
					bAllCross ? TEXT("ALREADY BLOOMS") : TEXT("DOES *NOT* BLOOM"),
					EffectiveThreshold,
					bAllCross
						? TEXT("The FX doc's request is already met — spec v32 s6 says CHANGE NOTHING.")
						: TEXT("Spec v32 s6: lower BloomThreshold and record what it moved from and to."));
				UE_LOG(LogTraceGame, Display, TEXT("======================================================"));
			}));
}

#endif // !UE_BUILD_SHIPPING
