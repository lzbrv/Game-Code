// Trace — the ten characters, as data. See TraceCharacterRoster.h.
//
// Since spec v17 §5 this file answers All() from ONE OF TWO PLACES: the
// UTraceCharacterDefinition assets under /Game/Trace/Data/Characters, or the C++ table below. The
// C++ table is not dead code kept for sentiment — it is the fallback rule §0.1 requires, it is what
// the generator reads when it authors the assets, and it is what Trace.VerifyCharacterData compares
// the assets against. Deleting it would make the migration untestable and unbackable-out-of.

#include "Core/TraceCharacterRoster.h"

#include "Abilities/TraceAbilityTypes.h"   // ETraceCharacterId — asserted against, never used as a type here
#include "Data/TraceCharacterDefinition.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Trace.h"
#include "UObject/UObjectGlobals.h"

// ---------------------------------------------------------------------------------------------
// THE ID SPACE, ASSERTED RATHER THAN ASSUMED.
//
// This file stores character ids as plain uint8 so the UI slice can draw a menu without taking the
// reflected ability headers. That is only safe as long as the two numberings are the same numbering,
// and "the same numbering" is exactly the kind of agreement that survives right up until somebody
// inserts an enumerator. So it is checked at compile time, here, in the one translation unit that
// can see both — a static_assert cannot be argued with, cannot go stale, and costs nothing.
// ---------------------------------------------------------------------------------------------

static_assert(static_cast<uint8>(ETraceCharacterId::None)      == TraceCharacterRoster::NoneId,
	"TraceCharacterRoster::NoneId must equal ETraceCharacterId::None.");
static_assert(static_cast<uint8>(ETraceCharacterId::Rocco)     == TraceCharacterRoster::FirstId,
	"TraceCharacterRoster::FirstId must equal ETraceCharacterId::Rocco.");

// *** THE ONE THAT MOVES. *** It named X until spec v18 §2 appended Roxie, Elle and Slimeball, and
// Slimeball until spec v19 §3 appended Mortimer and Lily; it names Lily now and it must name whoever
// is last after the NEXT character is added. This is the assertion that turns "somebody appended an
// enumerator and forgot the roster" — a bug whose only symptom is a character nobody can pick and a
// whole asset table that quietly stops being used — into a build that will not compile.
//
// IT DID ITS JOB THIS PASS. Appending Mortimer and Lily to the enum before touching this file is a
// compile error on exactly these three lines, which is the point of writing them down.
static_assert(static_cast<uint8>(ETraceCharacterId::Lily) == TraceCharacterRoster::LastId,
	"TraceCharacterRoster::LastId must equal the LAST enumerator of ETraceCharacterId (Lily as "
	"of spec v19 §3). If you appended a character, this line and TraceCharacterRoster::Count move too.");

static_assert(TraceCharacterCount == TraceCharacterRoster::Count,
	"The roster table and ETraceCharacterId disagree about how many characters there are.");
static_assert(TraceCharacterRoster::Count
	== (static_cast<int32>(TraceCharacterRoster::LastId) - static_cast<int32>(TraceCharacterRoster::FirstId) + 1),
	"Count must be the length of the CONTIGUOUS id range FirstId..LastId — Find() indexes the table "
	"with (Id - FirstId) rather than searching it, so a gap in the ids would hand a player the wrong "
	"character rather than failing.");

// The file's own namespace rather than an anonymous one. UBT compiles this module as a unity/jumbo
// build, so two files that each say `namespace { ... }` become one namespace with two definitions of
// everything they share a name with — GetTable() being about as collidable a name as exists. This
// project has already lost a Windows build that way; Scripts/check-jumbo-build-collisions.py gates
// on it and build.sh runs the check.
namespace TraceCharacterRosterFile
{
	/**
	 * THE C++ TABLE.
	 *
	 * Function-local static rather than a file-scope global: FLinearColor has a non-trivial
	 * constructor, and a file-scope TArray of them would run before main() in an order no standard
	 * guarantees. Everything in this file is read-only after first use, so the one-time initialisation
	 * is thread-safe by C++11 magic-statics.
	 *
	 * THE PROSE IS THE SPEC'S, TRIMMED TO FIT A CARD. Where a number was assumed rather than stated
	 * (Mace's and Oyster's cooldowns) the card still prints it, because a player choosing between
	 * ten characters needs to compare them on the same axes — but see the report: those two are
	 * [ASSUMPTION] 20 s and the ability framework, not this table, is what actually enforces any of it.
	 *
	 * *** THE COOLDOWN IN A ROW IS THE PRINTED ONE AND IT MUST MATCH THE ENFORCED ONE. *** The card
	 * and the HUD ring read it from here; the E key reads UTraceSettings. Trace.VerifyCharacterData
	 * section D compares the two and FAILS when they drift, which is how the three v18 §2 rows below
	 * were checked (Modded 25 s, Snap 35 s, Slimewall 25 s).
	 *
	 * *** THIS IS THE ORIGIN OF THE GENERATED ASSETS. *** Scripts/generate-data-assets.py contains no
	 * character data at all; it calls UTraceCharacterDefinition::CopyFallbackValues, which reads this.
	 * Change a word here and regenerate, and the assets follow. Change a word in an asset and this
	 * does not follow — Trace.VerifyCharacterData is what tells you.
	 */
	// =============================================================================================
	// *** DEMO 29 ITEM 1 — THE TEN GENERATED BODIES ARE UNWIRED, NOT DELETED. ***
	//
	// The owner played the overhaul and reported: "The third person view models are extremely messed
	// up, teleporting around and stretching at extreme angles. It's impossible to shoot at anyone."
	// He is right, and the cause is NOT the bodies. Every one of the ten rows below therefore has an
	// EMPTY BodyMeshPath and an EMPTY BodyAnimClassPath, which is a legal value this file has always
	// supported and which means "Epic's Mannequin" (ATraceCharacter::ApplyCharacterBodyMesh). The ten
	// SK_* assets, their materials, their physics assets and the shared skeleton all stay on disk,
	// untouched; the twenty strings that pointed at them are shelved in comments beside the rows they
	// came from.
	//
	// WHAT IS ACTUALLY BROKEN, MEASURED (reports/D29-REVERTS.md §3):
	//   Scripts/retarget_body.py bakes ABP_Unarmed onto SK_TraceBody_Skeleton with a TWO-op stack,
	//   PelvisMotion + FKChains. It deliberately drops the engine's RootMotion op on the stated
	//   grounds that "every sequence deliberately leaves the root in place". That is FALSE of the
	//   sixteen MF_Unarmed_* locomotion sequences: their travel lives entirely in the ROOT track
	//   (MF_Unarmed_Jog_Fwd's root runs 0 -> 1060.6 uu over 1.767 s with bEnableRootMotion = true).
	//   With no RootMotion op the target root is left flat at the origin, and PelvisMotionOp — which
	//   copies the SOURCE PELVIS'S GLOBAL transform — folds the whole of that root travel into the
	//   target's PELVIS track. Measured on the baked asset: source root 1060.6 + source pelvis -11.0
	//   = 1049.6, times the rig's 0.97486 uniform scale = 1023.2, and the baked pelvis reads 1023.4.
	//   So every walking or running pawn drags its pelvis — and therefore its whole body — up to
	//   10.3 metres away from its own capsule and snaps back at the loop point, and the blend space
	//   lerps sixteen such tracks pulling in six different directions. That is the teleporting, the
	//   stretching, and the reason bullets miss a body that is not where it is drawn.
	//
	// RESTORING THEM, once retarget_body.py is fixed and the bake re-run:
	//   1. put the twenty shelved strings back into the twenty slots marked "SHELVED, Demo 29 item 1"
	//      (mesh and anim class TOGETHER — a mesh without its anim class is a bind pose);
	//   2. chmod u+w Content/Trace/Data/Characters/*.uasset and re-run
	//      Scripts/generate-data-assets.py, or the assets will keep serving the empty paths;
	//   3. re-run Trace.VerifyCharacterData, and then a THIRD-PERSON MOVING capture, not a census.
	// =============================================================================================
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& GetCppTable()
	{
		static const TArray<TraceCharacterRoster::FTraceCharacterEntry> Table =
		{
			{
				1, TEXT("ROCCO"),
				TEXT("A VERY SMALL SECOND JUMP. THE POINT IS THE INSTANT MIDAIR DIRECTION CHANGE, NOT THE HEIGHT."),
				// SPEC v24 §11 MADE "1S" UNTRUE — RoccoHeadshotSpeedDurationSeconds is 3 now
				// (TraceSettings.h + Config/DefaultGame.ini). Only the number moved: the stacking and
				// the "each kill extends the whole boost" rule are the same sentence they were.
				TEXT("HEADSHOT KILLS GIVE +3% SPEED FOR 3S. STACKS, AND EACH KILL EXTENDS THE WHOLE BOOST."),
				TEXT("RIPPLE"),
				TEXT("DASH ON ITS OWN COOLDOWN, LEAVING A RIPPLE FOR 4S. ANYONE ON EITHER TEAM CAN RIDE IT, "
				     "INCLUDING THE CORE CARRIER, AND YOU CAN SHOOT WHILE RIDING."),
				20.f,
				FLinearColor(0.80f, 1.00f, 0.25f, 1.f),   // acid gold #E7FF89, sRGB hue 72.2
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Rocco's body:
				//   TEXT("/Game/Trace/Characters/Rocco/SK_Rocco.SK_Rocco"),
				TEXT(""),
				// -90 IS THE MANNEQUIN'S OWN YAW (TraceCharacterLayout::MeshYaw), so this row is
				// honest whichever body is wired: Epic's Manny is authored facing +Y and needs it,
				// and so did the shelved generated body, which import_characters.py MEASURED at -90
				// (facing_heading() runs `Z x (LeftHand - RightHand)` and fails the import outside
				// -90 +/- 2). It is not read at all while BodyMeshPath is empty — see
				// ATraceCharacter::ApplyCharacterBodyMesh, which only reads the yaw alongside a mesh.
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own. An anim class is
				// compiled against ONE skeleton, so ABP_Unarmed_Body only means anything on a pawn
				// that is actually wearing SK_TraceBody_Skeleton:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},
			// ---------------------------------------------------------------------------------
			// AND THE OTHER NINE BODIES — ALL NINE SHELVED TOO (Demo 29 item 1).
			//
			// Every row below ends in the same three fields for the same reason Rocco's does, so the
			// argument is made once, there, and not nine times here. The short version: there WAS one
			// generated GLB per character, all ten bound to the ONE shared SK_TraceBody_Skeleton
			// (PIPELINE_DESIGN.md §9.1), all ten taking the same measured -90 facing correction and
			// all ten running the ONE retargeted anim class. All ten strings are now shelved in
			// comments beside the rows that used to carry them, for the reason the block at the top
			// of GetCppTable() gives; the yaw stays -90 because that is also the Mannequin's.
			// ---------------------------------------------------------------------------------
			{
				2, TEXT("CHUT"),
				TEXT("BASH. HITTING A PLAYER WITH THE END OF A STANDARD DASH KNOCKS THEM ALONG YOUR TRAVEL. "
				     "NO EFFECT ON THE CORE CARRIER."),
				TEXT("KNIFE DEALS 50 FROM THE FRONT INSTEAD OF 30. THE 60 DEGREE BACK ZONE STAYS AT 100."),
				TEXT("CHUD"),
				TEXT("TAKE 30% LESS DAMAGE FROM BODY SHOTS AND MELEES FOR 10S. A KNIFE KILL REFRESHES THE "
				     "TIMER. DOES NOT STACK."),
				20.f,
				FLinearColor(0.35f, 0.95f, 0.37f, 1.f),   // signal green #A0F9A4, sRGB hue 122.7
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Chut's body:
				//   TEXT("/Game/Trace/Characters/Chut/SK_Chut.SK_Chut"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},
			{
				3, TEXT("MACE"),
				TEXT("HOLD V IN THE AIR TO SUSPEND FOR UP TO 1.25S. NO GRAVITY, LATERAL MOVE CAPPED AT 550. "
				     "RELEASING V CANCELS INSTANTLY."),
				TEXT("+30% CORE MAGNET RADIUS. THE BASE IS 450, SO HERS IS 585."),
				TEXT("SPIKE"),
				TEXT("THROW A ROPED SPIKE. IT EMBEDS IN A WALL FOR 2S; PRESS AGAIN TO BE PULLED TO IT AT THE "
				     "MOMENTUM CEILING. ANY MOVEMENT INPUT CANCELS. YOU CAN SHOOT AND BE SHOT WHILE PULLED."),
				20.f,
				FLinearColor(0.74f, 0.55f, 0.99f, 1.f),   // violet #DFC4FE, sRGB hue 267.9
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Mace's body:
				//   TEXT("/Game/Trace/Characters/Mace/SK_Mace.SK_Mace"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},
			{
				4, TEXT("OYSTER"),
				TEXT("JUMPING WHILE STOOD ON ONE OF YOUR OWN JARS BREAKS IT AND BOOSTS YOU UPWARD."),
				// SPEC v26 §6a. The refund is a POISON rule, not a Pickler rule — a dash jar an enemy
				// walks into pays it too — so it is stated on the line that describes the poison and
				// only cross-referenced from the Pickler line below. A card that hid it under PICKLER
				// would have players believing only the lob refunds.
				TEXT("EVERY DASH LEAVES A POISON JAR, EVEN CARRYING THE CORE. AN ENEMY WHO TOUCHES ONE "
				     "BREAKS IT: 3 DAMAGE EVERY 0.5S FOR 4S AND -30% SPEED. JARS LAST 4S, MAX 3. "
				     "POISONING AN ENEMY RESETS YOUR E, EVERY TIME."),
				TEXT("PICKLER"),
				// SPEC v26 §6b MADE THE OLD SENTENCE UNTRUE. It read "...THEN STAYS ON THE GROUND AS A
				// NORMAL JAR", which was the doc's own v14 clarification and is now the opposite of
				// what the jar does. The 20 S below is still the charged cooldown and still what the
				// HUD ring counts down, so it stays — but it is a ceiling now, which is why the second
				// sentence is here rather than left for a player to discover.
				TEXT("LOB A JAR. ON LANDING IT DEALS 30 IN AN AREA AND PULLS NEARBY ENEMIES IN, THEN "
				     "EXPLODES INTO POISON THE MOMENT THE PULL ENDS. ANY POISON YOU LAND HANDS E STRAIGHT "
				     "BACK, SO THE 20S IS A CEILING, NOT A WAIT."),
				20.f,
				FLinearColor(0.16f, 0.78f, 0.36f, 1.f),   // deep sea green #6FE5A2, sRGB hue 145.9
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Oyster's body:
				//   TEXT("/Game/Trace/Characters/Oyster/SK_Oyster.SK_Oyster"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},
			{
				5, TEXT("X"),
				// XVulnerableSpeedBonus went 0.10 -> 0.15 back in Demo 18 and this line was never
				// updated; the game's own knob dump has been printing "+15.0%" beside it since.
				// Caught by the spec v24 verifier, fixed here rather than left to rot next to the
				// vulnerable numbers this patch DID correct.
				TEXT("+15% SPEED WHILE ANY ENEMY IS VULNERABLE."),
				// SPEC v24 §9 MADE "+25%" UNTRUE — XVulnerableDamageBonus is 0.35 now (TraceSettings.h
				// + Config/DefaultGame.ini). "DOES NOT STACK" was ALREADY untrue and is fixed in the
				// same breath rather than left standing next to a corrected number: spec v16 §4 made
				// the mark stack at +5% a hit (XVulnerableStackBonus) up to five (XVulnerableMaxStacks),
				// and the HUD chip has been printing the stack count since. The "a new hit resets the
				// timer" half of v14 §6 did survive, so it stays.
				TEXT("FIVE MECHANICAL BEES ORBIT YOU. AN ENEMY THEY TOUCH IS VULNERABLE FOR 2S AND TAKES "
				     "+35% DAMAGE FROM EVERYTHING, +5% MORE PER EXTRA HIT UP TO FIVE. A NEW HIT RESETS "
				     "THE TIMER."),
				TEXT("STING"),
				TEXT("LOAD THE 5 BEES INTO YOUR GUN. YOUR NEXT FIVE BULLETS APPLY VULNERABLE AT NORMAL "
				     "DAMAGE. THE BEES RESUME ORBITING ONCE ALL FIVE ARE FIRED."),
				25.f,
				FLinearColor(1.00f, 0.40f, 0.72f, 1.f),   // rose #FFAADD, sRGB hue 324.0
				// SHELVED, Demo 29 item 1 — put this string back to re-wire X's body:
				//   TEXT("/Game/Trace/Characters/X/SK_X.SK_X"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},

			// ---------------------------------------------------------------------------------
			// SPEC v18 §2 — the three new characters.
			//
			// The prose is the Demo 16 doc's, trimmed the same way the first five were. Every NUMBER
			// quoted in these strings is also a UTraceSettings knob under "Abilities|<Name>", and the
			// knob is the one that plays — a card that says 35% and a game that slows by 20% is a card
			// that lied, so retuning any of these means editing the sentence here as well.
			//
			// THE ACCENTS ARE ALL NEW HUES. There are eight cards on one screen now and the stripe is
			// how a player finds theirs at a glance, so none of the three below is a near-neighbour of
			// the five above it. *** WAVE 5 RE-SPACED THE WHOLE RING and this sentence now means more
			// than card-vs-card separation: see the block at the top of GetCppTable()'s accent notes
			// (and Scripts/character_bodies.py's CHARACTERS table) — every accent is now also >= 40 deg
			// of sRGB hue away from BOTH TEAM colours, which the original ten were not.
			// ---------------------------------------------------------------------------------
			{
				6, TEXT("ROXIE"),
				TEXT("V FIRES A WOBBLING ROCKET THAT THROWS HER BACKWARDS, FAST AND FAR. IT IS HARD TO "
				     "AIM AND DEALS 100 ANYWHERE IT HITS. 35S COOLDOWN."),
				TEXT("JUMPS 15% HIGHER THAN EVERYONE ELSE."),
				TEXT("MODDED"),
				TEXT("LOAD A MODDED CLIP: THE GUN GOES FULL AUTO AND FIRES 1.65X FASTER. ENDS AFTER ONE "
				     "CLIP OR 5S, WHICHEVER COMES FIRST."),
				25.f,
				FLinearColor(1.00f, 0.12f, 0.20f, 1.f),   // ember #FF617C, sRGB hue 349.7
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Roxie's body:
				//   TEXT("/Game/Trace/Characters/Roxie/SK_Roxie.SK_Roxie"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},
			{
				7, TEXT("ELLE"),
				// *** 30, NOT 40, AND THE NUMBER IS UTraceSettings::ElleSlideJumpGainBonus (0.30). ***
				// Patch 28 §3 cut her slide-jump bonus from 40% to 30% and this string was not moved
				// with it, so for two passes the card told the player a number the game did not give.
				// Trace.VerifyCharacterData stayed GREEN throughout, and that is the part worth
				// remembering: section D compares this table to the generated asset, and the asset is
				// generated FROM this table, so the two agreed with each other about the wrong number.
				// Nothing in the build compares either of them to the knob. Retune the knob and you
				// must retune this line by hand — see W9-UIFIX.md §1 for the check that would close
				// the class, and TraceSettings.h's Elle banner and DefaultGame.ini's Elle block, which
				// both carry a pointer back here.
				TEXT("WELL-TIMED SLIDE JUMPS GIVE HER 30% MORE OF THE MOMENTUM BOOST THAN ANYONE ELSE."),
				TEXT("PASSING OR THROWING THE CORE CLOAKS HER FOR 3S - SEMI-TRANSPARENT AND HARD TO SEE "
				     "OR AIM AT."),
				TEXT("SNAP"),
				TEXT("PLACE A GATE, THEN PRESS AGAIN WITHIN 4S TO PLACE ITS PAIR. PLAYERS ON EITHER TEAM "
				     "CAN TELEPORT BETWEEN THEM. BOTH VANISH AFTER 8S."),
				35.f,
				FLinearColor(0.96f, 0.42f, 1.00f, 1.f),   // orchid #FAADFF, sRGB hue 296.3
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Elle's body:
				//   TEXT("/Game/Trace/Characters/Elle/SK_Elle.SK_Elle"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},
			{
				8, TEXT("SLIMEBALL"),
				TEXT("HOLD V TO STICK TO A WALL."),
				TEXT("WHILE STUCK: FIRES 30% FASTER AND TAKES 30% LESS FROM BODY SHOTS AND FRONT KNIFE "
				     "STABS. HEADSHOTS AND BACKSTABS STILL HURT IN FULL."),
				TEXT("SLIMEWALL"),
				TEXT("THROW UP A WALL WHERE YOU ARE AIMING, FOR 4S. BULLETS PASS STRAIGHT THROUGH IT BUT "
				     "NOBODY CAN SEE THROUGH IT, AND ENEMIES WALKING THROUGH ARE SLOWED 35%."),
				25.f,
				FLinearColor(0.33f, 0.92f, 0.16f, 1.f),   // slime #9BF66F, sRGB hue 100.4
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Slimeball's body:
				//   TEXT("/Game/Trace/Characters/Slimeball/SK_Slimeball.SK_Slimeball"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},

			// ---------------------------------------------------------------------------------
			// SPEC v19 §3 — the two Demo 18 characters.
			//
			// Same rules as the v18 §2 block above: the prose is the doc's, trimmed to a card, and
			// every NUMBER quoted in it is also a UTraceSettings knob under "Abilities|<Name>". A card
			// that says 30% while the game does 20% is a card that lied.
			//
			// TWO ACCENTS THAT ARE NOT NEIGHBOURS OF THE EIGHT ABOVE. Ten cards now share one screen
			// across two rows, so the stripe is doing more work than ever.
			//
			// *** THE ORIGINAL TWO WERE A DESATURATED BLUE AND A NEAR-WHITE, AND MEASUREMENT KILLED
			//     BOTH. *** W4-CENSUS photographed the ten in the arena and measured hue over the lit
			// body pixels: Mortimer's slate #A6BFED sat 7.2 deg from team Blue #5B81FF, so 83.4% of his
			// lit body read "in his own hue" for the trivial reason that his own hue WAS the team's,
			// and on Blue he had no colour identity at all. Lily's ice #E1F6FF carried a saturation of
			// 0.118, below which a hue is not perceptible — W4-PORTRAITS had to grant her a near-white
			// EXEMPTION from the portrait accent-discrimination check for exactly that reason. So
			// Mortimer is now a dark, saturated patinated steel (167.0 deg, 59.1 from Blue) and Lily a
			// pale glacier ice with saturation 0.278 (185.9 deg, 40.2 from Blue). Lily is still the
			// palest accent on the roster and Mortimer still the coldest and heaviest; what changed is
			// that both are now nameable next to a team panel.
			//
			// *** BOTH ACTIVATED NAMES ARE [ASSUMPTION] AND ARE FLAGGED IN THE REPORT. *** Demo 18
			// names Lily's ability ("Zip") and does NOT name Mortimer's blast; "QUAKE" is ours, chosen
			// because the ability is refused unless he is stood on something. Renaming it is this one
			// string plus a regenerate.
			// ---------------------------------------------------------------------------------
			{
				9, TEXT("MORTIMER"),
				TEXT("MANTLE. HE IS THE ONLY CHARACTER WHO CAN PULL HIMSELF UP ONTO A LEDGE OR THE TOP "
				     "OF AN OBJECT, AND HIS REACH AND HEIGHT WINDOW ARE 30% MORE GENEROUS THAN THE OLD "
				     "IN-GAME MANTLE."),
				// DEMO 20 ITEM 2 MADE THIS STRING UNTRUE AND THEN INCREASED THE COOLDOWN TOO, so both
				// halves are stated here. MortimerDashDistanceScale is 0.40 and MortimerDashCooldownScale
				// is 1.25 (TraceSettings.h + Config/DefaultGame.ini); measured live by
				// Trace.Mortimer.DashTest. Same precedent as commit c060875, which fixed Lily's card
				// after Demo 19 changed her kit — a card that lies about the kit is worse than no card.
				// SPEC v24 §7 MADE "ON THE SAME SCALE ... ABOUT TWICE AS FAR" UNTRUE the moment it
				// landed: charge past the ordinary full point now counts at 0.6x
				// (MortimerThrowChargePastFullScale). Deliberately no multiplier in the new wording —
				// the old line went stale because it hardcoded one, and the honest statement of the
				// mechanic ("further than anyone, not double") survives a re-tune of that 0.6.
				TEXT("HIS DASH COVERS ONLY TWO FIFTHS OF THE NORMAL DISTANCE AND RECHARGES A QUARTER "
				     "SLOWER. IN EXCHANGE HE MAY CHARGE A CORE THROW FOR TWICE AS LONG AS ANYONE "
				     "ELSE, THOUGH CHARGE BEYOND A NORMAL FULL ONE COUNTS FOR LESS - SO HE STILL "
				     "THROWS IT FURTHER THAN ANYONE, JUST NOT DOUBLE."),
				TEXT("QUAKE"),
				TEXT("ONLY WHILE CARRYING THE CORE AND STOOD ON THE GROUND OR ON TOP OF AN OBJECT: A "
				     "BLAST THAT KNOCKS EVERY NEARBY ENEMY AWAY FROM HIM. IT CANNOT MOVE A CORE "
				     "CARRIER."),
				20.f,
				FLinearColor(0.11f, 0.46f, 0.36f, 1.f),   // patinated steel #5DB5A2, sRGB hue 167.0
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Mortimer's body:
				//   TEXT("/Game/Trace/Characters/Mortimer/SK_Mortimer.SK_Mortimer"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			},
			{
				// DEMO 19 ITEMS 8 AND 4 MADE BOTH OF LILY'S FIRST TWO STRINGS UNTRUE, so both moved.
				// The card used to promise "THREE WHILE CARRYING THE CORE" (item 8 took that away) and
				// "CLIMBS AT WALKING SPEED" (item 4 halved it). A card that lies about the kit is worse
				// than no card, and these are the only two edits: the layout, the colour and every other
				// character's rows are untouched.
				10, TEXT("LILY"),
				TEXT("ONE EXTRA DASH CHARGE, BUT ONLY WHILE SHE IS NOT CARRYING THE CORE: TWO EITHER WAY."),
				TEXT("WALL JUMPS CARRY 30% MORE MOMENTUM THAN ANYONE ELSE'S. SHE IS ALSO THE FRAILEST "
				     "CHARACTER IN THE GAME AT 60 HEALTH INSTEAD OF 100."),
				TEXT("ZIP"),
				TEXT("FLY FOR 5S. JUMP CLIMBS AND CROUCH DESCENDS, BOTH AT HALF WALKING SPEED, AND "
				     "EVERYTHING ELSE PLAYS AS NORMAL. CARRYING THE CORE HALVES IT - AND PICKING THE "
				     "CORE UP MID-FLIGHT HALVES WHATEVER IS LEFT."),
				30.f,
				FLinearColor(0.48f, 0.94f, 1.00f, 1.f),   // glacier ice #B8F8FF, sRGB hue 185.9
				// SHELVED, Demo 29 item 1 — put this string back to re-wire Lily's body:
				//   TEXT("/Game/Trace/Characters/Lily/SK_Lily.SK_Lily"),
				TEXT(""),
				-90.f,
				// SHELVED, Demo 29 item 1 — and this one with it, never on its own:
				//   TEXT("/Game/Trace/Characters/Shared/Anims/ABP_Unarmed_Body.ABP_Unarmed_Body_C")
				TEXT("")
			}
		};

		return Table;
	}

	// =============================================================================================
	// THE ASSET PATH
	// =============================================================================================

	/**
	 * The Count entries built from the assets, and the strings they point INTO.
	 *
	 * FTraceCharacterEntry holds `const TCHAR*`, because it is read by a Canvas HUD that draws every
	 * frame and by a header that deliberately takes no dependency on UObject. Those pointers must
	 * outlive every draw, so the FStrings that own the characters live here, for the life of the
	 * process, and are never touched after the table is built.
	 *
	 * WHY THAT IS SAFE, stated rather than hoped: FString's characters are a heap allocation owned by
	 * a TArray (UE has no small-string optimisation), so the address of the first character does not
	 * move when the FString is moved. Even so, Strings is RESERVED TO ITS FINAL SIZE before the first
	 * Add, so nothing is ever moved at all. Both belts; the trousers stay up either way.
	 */
	struct FAssetRosterStorage
	{
		TArray<FString> Strings;
		TArray<TraceCharacterRoster::FTraceCharacterEntry> Entries;

		void Reset()
		{
			Entries.Reset();
			Strings.Reset();
		}
	};

	FAssetRosterStorage& AssetStorage()
	{
		static FAssetRosterStorage Storage;
		return Storage;
	}

	bool GResolved = false;
	TraceCharacterRoster::ESource GSource = TraceCharacterRoster::ESource::CppTable;

	void OnUseAssetsCVarChanged(IConsoleVariable* /*Variable*/);

	/**
	 * RULE §0.1's toggle. 1 (default) = use the assets when ALL of them are present and valid;
	 * 0 = the C++ table, unconditionally, exactly as the game behaved before spec v17 §5.
	 *
	 * Default 1 rather than 0 because spec v17 §5 asks for "loads definitions from assets when
	 * present", the assets are generated FROM the C++ values, and Trace.VerifyCharacterData proves
	 * field by field that the two agree. The fallback is not the off-switch — the fallback is
	 * automatic and silent-except-in-the-log whenever the assets are missing or wrong. This toggle
	 * is the manual override for the day somebody needs to bisect a bug against the old path.
	 */
	TAutoConsoleVariable<int32> CVarUseCharacterAssets(
		TEXT("Trace.Data.UseCharacterAssets"),
		1,
		TEXT("Trace, spec v17 §5. 1 = the roster reads /Game/Trace/Data/Characters when ALL the character assets\n")
		TEXT("load and validate; 0 = always use the C++ table compiled into TraceCharacterRoster.cpp.\n")
		TEXT("Either way the game plays identically — the assets are generated from the C++ values and\n")
		TEXT("Trace.VerifyCharacterData proves it. Changing this re-resolves the roster immediately."),
		FConsoleVariableDelegate::CreateStatic(&OnUseAssetsCVarChanged),
		ECVF_Default);

	/**
	 * Load and validate ALL of them, or none. See the all-or-none note in the header.
	 *
	 * @param OutWhyNot  why the C++ table is being used, phrased for a log line a designer will read.
	 * @return true only when Storage holds Count good entries in id order.
	 */
	bool TryBuildFromAssets(FAssetRosterStorage& Storage, FString& OutWhyNot)
	{
		Storage.Reset();

		// SEVEN strings PER CHARACTER (name, movement, passive, activated name, activated, body mesh
		// path, body anim class path) — seven is the number of STRINGS on a row, not the number of
		// characters. It was five until the body mesh landed, six until the retarget, and it did not
		// move in v18 §2. Reserved up front so no Add can ever reallocate — see FAssetRosterStorage's
		// comment. *** IF YOU ADD A STRING TO FTraceCharacterEntry, THIS NUMBER MOVES WITH IT: an Add
		// past the reserve reallocates and every TCHAR* already handed out points at freed memory. ***
		Storage.Strings.Reserve(TraceCharacterRoster::Count * 7);
		Storage.Entries.Reserve(TraceCharacterRoster::Count);

		for (int32 IdValue = TraceCharacterRoster::FirstId; IdValue <= TraceCharacterRoster::LastId; ++IdValue)
		{
			const ETraceCharacterId TypedId = static_cast<ETraceCharacterId>(IdValue);
			const FString PackagePath = UTraceCharacterDefinition::PackagePathFor(TypedId);
			const FString ObjectPath  = UTraceCharacterDefinition::ObjectPathFor(TypedId);

			// Asked BEFORE LoadObject so that the ordinary "the assets have not been generated yet"
			// case produces one calm log line rather than one engine load warning per character.
			if (!FPackageName::DoesPackageExist(PackagePath))
			{
				OutWhyNot = FString::Printf(TEXT("%s does not exist"), *PackagePath);
				Storage.Reset();
				return false;
			}

			UTraceCharacterDefinition* const Definition =
				LoadObject<UTraceCharacterDefinition>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);

			if (Definition == nullptr)
			{
				OutWhyNot = FString::Printf(TEXT("%s exists but did not load as a UTraceCharacterDefinition"), *ObjectPath);
				Storage.Reset();
				return false;
			}

			FString WhyUnusable;
			if (!Definition->IsUsable(WhyUnusable))
			{
				OutWhyNot = FString::Printf(TEXT("%s is not usable: %s"), *ObjectPath, *WhyUnusable);
				Storage.Reset();
				return false;
			}

			if (static_cast<int32>(Definition->CharacterId) != IdValue)
			{
				// The file name and the id inside it disagree. Refusing is the only safe answer: the
				// id is an array index in the select screen's card layout, so a table out of id order
				// would draw ROCCO's card and hand the player CHUT.
				OutWhyNot = FString::Printf(TEXT("%s holds CharacterId %d but its name says %d"),
					*ObjectPath, static_cast<int32>(Definition->CharacterId), IdValue);
				Storage.Reset();
				return false;
			}

			const int32 NameIndex      = Storage.Strings.Add(Definition->DisplayName);
			const int32 MovementIndex  = Storage.Strings.Add(Definition->MovementSlot.Description);
			const int32 PassiveIndex   = Storage.Strings.Add(Definition->PassiveSlot.Description);
			const int32 ActivatedNameIndex = Storage.Strings.Add(Definition->ActivatedSlot.DisplayName);
			const int32 ActivatedIndex = Storage.Strings.Add(Definition->ActivatedSlot.Description);

			// EMPTY WHEN THE ASSET NAMES NO MESH — the normal case again since Demo 29 item 1 shelved
			// all ten body paths (see the block above GetCppTable()), and always a legal one. It is
			// stored anyway rather than skipped, because the reserve above counts seven strings a row
			// and an Add that only sometimes happens is an Add nobody can count.
			const int32 BodyMeshIndex = Storage.Strings.Add(
				Definition->BodyMesh.IsNull() ? FString() : Definition->BodyMesh.ToSoftObjectPath().ToString());
			const int32 BodyAnimIndex = Storage.Strings.Add(
				Definition->BodyAnimClass.IsNull() ? FString() : Definition->BodyAnimClass.ToSoftObjectPath().ToString());

			TraceCharacterRoster::FTraceCharacterEntry Entry;
			Entry.Id                = static_cast<uint8>(IdValue);
			Entry.Name              = *Storage.Strings[NameIndex];
			Entry.Movement          = *Storage.Strings[MovementIndex];
			Entry.Passive           = *Storage.Strings[PassiveIndex];
			Entry.ActivatedName     = *Storage.Strings[ActivatedNameIndex];
			Entry.Activated         = *Storage.Strings[ActivatedIndex];
			Entry.ActivatedCooldown = Definition->ActivatedSlot.CooldownSeconds;
			Entry.Accent            = Definition->AccentColor;
			Entry.BodyMeshPath      = *Storage.Strings[BodyMeshIndex];
			Entry.BodyMeshYaw       = Definition->BodyMeshYaw;
			Entry.BodyAnimClassPath = *Storage.Strings[BodyAnimIndex];

			Storage.Entries.Add(Entry);
		}

		if (Storage.Entries.Num() != TraceCharacterRoster::Count)
		{
			OutWhyNot = FString::Printf(TEXT("built %d entries, expected %d"),
				Storage.Entries.Num(), TraceCharacterRoster::Count);
			Storage.Reset();
			return false;
		}

		OutWhyNot.Reset();
		return true;
	}

	/**
	 * Decide once, then answer from the decision.
	 *
	 * GAME THREAD ONLY, like everything that reads this table (the HUD's draw, the game mode's log
	 * lines, the select screen). LoadObject is game-thread work and the storage is not guarded.
	 */
	const TArray<TraceCharacterRoster::FTraceCharacterEntry>& Resolve()
	{
		if (!GResolved)
		{
			// DO NOT LATCH BEFORE THE ENGINE EXISTS. Something could ask the roster for a name during
			// static initialisation or very early module startup, long before any package can be
			// loaded; answering "C++ table" is right for that call and WRONG to remember forever.
			if (GEngine == nullptr)
			{
				return GetCppTable();
			}

			GResolved = true;
			GSource = TraceCharacterRoster::ESource::CppTable;

			if (CVarUseCharacterAssets.GetValueOnGameThread() == 0)
			{
				UE_LOG(LogTraceGame, Display,
					TEXT("[CharacterData] Roster source: C++ table — Trace.Data.UseCharacterAssets is 0. "
					     "The characters are exactly what they were before spec v17 §5."));
			}
			else
			{
				FString WhyNot;
				if (TryBuildFromAssets(AssetStorage(), WhyNot))
				{
					GSource = TraceCharacterRoster::ESource::Assets;
					UE_LOG(LogTraceGame, Display,
						TEXT("[CharacterData] Roster source: ASSETS — all %d loaded from %s. "
						     "Run Trace.VerifyCharacterData to compare them with the C++ table."),
						TraceCharacterRoster::Count, UTraceCharacterDefinition::CharactersPackageRoot());
				}
				else
				{
					UE_LOG(LogTraceGame, Display,
						TEXT("[CharacterData] Roster source: C++ table (FALLBACK) — %s. This is not an error: "
						     "the game plays exactly as it did before spec v17 §5. Run "
						     "Scripts/generate-data-assets.py to author the assets."),
						*WhyNot);
				}
			}
		}

		return (GSource == TraceCharacterRoster::ESource::Assets) ? AssetStorage().Entries : GetCppTable();
	}

	void OnUseAssetsCVarChanged(IConsoleVariable* /*Variable*/)
	{
		TraceCharacterRoster::ForceReload();
	}
}

const TArray<TraceCharacterRoster::FTraceCharacterEntry>& TraceCharacterRoster::All()
{
	return TraceCharacterRosterFile::Resolve();
}

const TArray<TraceCharacterRoster::FTraceCharacterEntry>& TraceCharacterRoster::CppFallbackTable()
{
	return TraceCharacterRosterFile::GetCppTable();
}

TraceCharacterRoster::ESource TraceCharacterRoster::CurrentSource()
{
	TraceCharacterRosterFile::Resolve();
	return TraceCharacterRosterFile::GSource;
}

const TCHAR* TraceCharacterRoster::CurrentSourceName()
{
	return (CurrentSource() == ESource::Assets)
		? TEXT("assets (/Game/Trace/Data/Characters)")
		: TEXT("C++ table (Core/TraceCharacterRoster.cpp)");
}

void TraceCharacterRoster::ForceReload()
{
	// Callers hold FTraceCharacterEntry pointers only for the duration of one draw or one log line,
	// and this runs from a console command, i.e. between frames on the game thread. Anything that
	// cached an entry ACROSS frames was already wrong — All() has never promised a stable address.
	TraceCharacterRosterFile::GResolved = false;
	TraceCharacterRosterFile::GSource = ESource::CppTable;
	TraceCharacterRosterFile::AssetStorage().Reset();
}

const TraceCharacterRoster::FTraceCharacterEntry* TraceCharacterRoster::Find(uint8 Id)
{
	if (!IsValidId(Id))
	{
		return nullptr;
	}

	// The table is authored in id order and asserted so below, which makes this an index rather than
	// a search. If the assertion ever fires, fix the TABLE — do not turn this into a linear scan,
	// because the id is also an array index in the select screen's card layout. (The asset loader
	// enforces the same order before it will serve a single row, for the same reason.)
	const TArray<FTraceCharacterEntry>& Table = All();

	const int32 Index = static_cast<int32>(Id) - static_cast<int32>(FirstId);
	if (!Table.IsValidIndex(Index))
	{
		return nullptr;
	}

	checkf(Table[Index].Id == Id,
		TEXT("TraceCharacterRoster: the table is out of id order at index %d (holds %d, expected %d)."),
		Index, static_cast<int32>(Table[Index].Id), static_cast<int32>(Id));

	return &Table[Index];
}

bool TraceCharacterRoster::IsValidId(uint8 Id)
{
	return Id >= FirstId && Id <= LastId;
}

FString TraceCharacterRoster::NameFor(uint8 Id)
{
	if (Id == NoneId)
	{
		// Never "NONE". This string ends up in a kill feed, a log line and on the select screen's
		// "you are playing as" footer, and "none" reads as a fault where "mannequin" reads as a
		// deliberate state — which is exactly what it is in mode A, with the characters toggle off,
		// and for anybody a full team roster could not serve. (It is no longer the bots' permanent
		// state: spec v15 §2 gives them characters too.)
		return TEXT("MANNEQUIN");
	}

	if (const FTraceCharacterEntry* Entry = Find(Id))
	{
		return Entry->Name;
	}

	return FString::Printf(TEXT("CHARACTER %d"), static_cast<int32>(Id));
}
