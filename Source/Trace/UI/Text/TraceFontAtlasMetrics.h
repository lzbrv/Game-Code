// GENERATED FILE — DO NOT EDIT BY HAND.
// Produced by Scripts/import_font_atlas.py from
//   Content/Trace/UI/Fonts/Source/T_FontAtlas.json   (Light, sha1 2c02bf95f91e4c71efce5a3e684d8d7b03f17af1)
//   Content/Trace/UI/Fonts/Source/T_FontAtlasBold.json   (Bold, sha1 33bc4c364550c974efe6132585f37947cb1c24ee)
//   Content/Trace/UI/Fonts/Source/T_FontAtlasHud.json   (Hud, sha1 d84c43da61a35c2590988dfc35c7811065df739c)
//   Content/Trace/UI/Fonts/Source/T_FontAtlasNames.json   (Names, the FALLBACK face, sha1 bc2f94f389ef7d76779a085c61a660d02fc391c7)
//
// THE ONE METRICS SOURCE (spec v22 §A1, two weights by v23 §A3, a third face by v25 §4). Both
// renderers — the Canvas blitter in TraceCanvasText.cpp and the Slate leaf in
// TraceAtlasTextWidget.cpp — lay text out through TraceText::LayoutString(), and
// TraceText.cpp is the ONLY file in the project that includes this header. That is what
// makes "one source" structural rather than a promise in a comment: the two paths cannot
// drift because there is only one layout pass.
//
// THE FACES, AND WHAT IS AND IS NOT ALLOWED TO DIFFER BETWEEN THEM
//   Each face is rasterised from ITS OWN REAL FONT FILE — the 'source' recorded in each
//   sheet's .json, carried per face in the table below. --thin (glyph erosion) is 0 for every
//   one of them and must stay 0: two attempts to SYNTHESISE a light cut by eroding Regular
//   damaged the letterforms, the second deleting ( ) [ ] { } / and \ outright while the HUD
//   draws '[E]'. "Light" is the default everywhere; "Bold" is the character NAMES on the
//   selection screen; "Hud" (spec v25 §4) is the in-match HUD and the ability DESCRIPTIONS,
//   and it is a different FAMILY rather than a heavier cut — the enum axis is called weight
//   because that is what the runtime already selected on.
//
//   *** WHAT ONE LAYOUT PASS ACTUALLY REQUIRES, and it is only this: *** the em, the LINE
//   HEIGHT and the charset. Those three are emitted once, below, and enforced by
//   import_font_atlas.py's check_weights_agree(), which refuses to write this file if the
//   WEIGHTS ever disagree about them. *** THE FALLBACK FACE AT THE BOTTOM OF THIS FILE IS
//   NOT A WEIGHT AND IS NOT BOUND BY THOSE THREE *** — it is never laid out, only dropped
//   into a line one glyph at a time, so it shares the em and nothing else. See its own
//   section for what replaces the other two guarantees.
//
//   *** ADVANCES ARE NOT SHARED. *** Each face has its own cell table below and its own
//   widths, because each is rasterised from its own font file. An earlier pass synthesised
//   the light cut by eroding Regular, where the advances WERE identical, and code was
//   written on that assumption; it is false now and it made a bold name overrun its
//   column. Anything that MEASURES a string must pass the weight it will DRAW it in.
//
//   *** ASCENT AND DESCENT ARE NOT SHARED EITHER (v25). *** They are how the shared line box
//   is SPLIT, not how tall it is: Erbaum-Bold splits 116 px as 93/23 where both Sofachrome
//   cuts split it 95/21. Only EVAlign::Baseline and EVAlign::CapTop read them. The module
//   constants below are the DEFAULT face's, kept so code with no opinion reads what it always
//   read; TraceText::Ascent() takes a weight and answers per face.
//
//   CAP HEIGHT is per face and is measured off each sheet's own 'H'. A caller aligning live
//   type to one of the artist's baked word sprites must ask for the cap height OF THE WEIGHT
//   IT IS DRAWING.
//
// THE CELL MODEL, because it is what makes this table so small:
//   Every cell is one FULL ADVANCE WIDTH by one FULL LINE HEIGHT, and the glyph is drawn
//   inside it with its own bearings already applied. So the pen advances by uSize and every
//   glyph sits on a shared baseline — there is no bearing table and there is no kerning.
//   Sofachrome is a wide squared face whose ink runs flush to the advance on A, T, V, W and
//   Y; that is the typeface, not clipping. Erbaum is a narrower squared grotesque and does
//   the same on its own diagonals.
//
// Units are ATLAS PIXELS at an em of 96. On screen everything is multiplied by
// (Size / EmSize), where Size means the same thing it means in FSlateFontInfo::Size.
#pragma once

#include "CoreMinimal.h"

namespace TraceFontAtlasMetrics
{
	/** The DEFAULT face's typeface. NOT in the repository — see docs/FONTS.md. Per-face
	  * sources are in FFace::Source below; this one is what names the family in a caption. */
	inline constexpr const TCHAR* SourceFont = TEXT("Sofachrome W05 ExtraLight.ttf");

	/** Indexed by ETraceTextWeight, in the same order; index 0 (Light) is the DEFAULT.
	  * Keep in step with ETraceTextWeight in TraceTextWeight.h — TraceText.cpp static_asserts it. */
	inline constexpr int32 NumWeights    = 3;
	inline constexpr int32 DefaultWeight = 0;   // Light

	// =============================================================================
	// SHARED BY EVERY WEIGHT — enforced by the generator, so layout can rely on it
	// (the FALLBACK face at the bottom shares only EmSize; see its own banner)
	// =============================================================================

	/** Em size the sheets were rasterised at. Everything below is in these pixels. */
	inline constexpr float EmSize     = 96.f;
	/** What one line of a multi-line string advances by. THE shared vertical number. */
	inline constexpr float LineHeight  = 116.f;

	/** The DEFAULT face's split of that line box. PER FACE from v25 — see FFace::Ascent.
	  * Kept at module scope so callers with no opinion about weight read what they always
	  * read; anything aligning to a baseline in a NON-default face must ask FFace. */
	inline constexpr float Ascent     = 95.f;
	inline constexpr float Descent    = 21.f;

	/** The WEIGHTS' charset — one contiguous block, which is what lets them be indexed by
	  * subtraction. The fallback face's charset is wider and has holes; it carries its own
	  * range table rather than a First/Last pair. */
	inline constexpr int32 FirstCode = 32;
	inline constexpr int32 LastCode  = 126;
	inline constexpr int32 NumGlyphs = 95;

	/** One cell, in atlas pixels. For a WEIGHT, index by (code - FirstCode); the weights'
	  * charset is contiguous. The fallback face uses FallbackCell() instead. */
	struct FCell
	{
		uint16 U;
		uint16 V;
		uint16 USize;
		uint16 VSize;
	};

	inline constexpr FCell LightCells[NumGlyphs] =
	{
		{   16,   16,   34,  116 },   //  32  space
		{   82,   16,   19,  116 },   //  33  !
		{  133,   16,   29,  116 },   //  34  "
		{  194,   16,   44,  116 },   //  35  #
		{  270,   16,   65,  116 },   //  36  $
		{  367,   16,   95,  116 },   //  37  %
		{  494,   16,   66,  116 },   //  38  &
		{  592,   16,   13,  116 },   //  39  '
		{  637,   16,   34,  116 },   //  40  (
		{  703,   16,   34,  116 },   //  41  )
		{  769,   16,   50,  116 },   //  42  *
		{  851,   16,   65,  116 },   //  43  +
		{  948,   16,   17,  116 },   //  44  ,
		{  997,   16,   29,  116 },   //  45  -
		{ 1058,   16,   18,  116 },   //  46  .
		{ 1108,   16,   50,  116 },   //  47  /
		{ 1190,   16,   96,  116 },   //  48  0
		{ 1318,   16,   29,  116 },   //  49  1
		{ 1379,   16,   94,  116 },   //  50  2
		{ 1505,   16,   89,  116 },   //  51  3
		{ 1626,   16,   93,  116 },   //  52  4
		{ 1751,   16,   92,  116 },   //  53  5
		{ 1875,   16,   94,  116 },   //  54  6
		{   16,  164,   76,  116 },   //  55  7
		{  124,  164,   90,  116 },   //  56  8
		{  246,  164,   93,  116 },   //  57  9
		{  371,  164,   20,  116 },   //  58  :
		{  423,  164,   20,  116 },   //  59  ;
		{  475,  164,   53,  116 },   //  60  <
		{  560,  164,   65,  116 },   //  61  =
		{  657,  164,   53,  116 },   //  62  >
		{  742,  164,   73,  116 },   //  63  ?
		{  847,  164,   70,  116 },   //  64  @
		{  949,  164,  118,  116 },   //  65  A
		{ 1099,  164,  103,  116 },   //  66  B
		{ 1234,  164,   94,  116 },   //  67  C
		{ 1360,  164,  107,  116 },   //  68  D
		{ 1499,  164,   97,  116 },   //  69  E
		{ 1628,  164,   95,  116 },   //  70  F
		{ 1755,  164,  101,  116 },   //  71  G
		{ 1888,  164,  103,  116 },   //  72  H
		{   16,  312,   22,  116 },   //  73  I
		{   70,  312,   81,  116 },   //  74  J
		{  183,  312,  111,  116 },   //  75  K
		{  326,  312,   88,  116 },   //  76  L
		{  446,  312,  122,  116 },   //  77  M
		{  600,  312,  110,  116 },   //  78  N
		{  742,  312,  105,  116 },   //  79  O
		{  879,  312,  100,  116 },   //  80  P
		{ 1011,  312,  105,  116 },   //  81  Q
		{ 1148,  312,  108,  116 },   //  82  R
		{ 1288,  312,   89,  116 },   //  83  S
		{ 1409,  312,   88,  116 },   //  84  T
		{ 1529,  312,  102,  116 },   //  85  U
		{ 1663,  312,  118,  116 },   //  86  V
		{ 1813,  312,  164,  116 },   //  87  W
		{   16,  460,  111,  116 },   //  88  X
		{  159,  460,   97,  116 },   //  89  Y
		{  288,  460,   93,  116 },   //  90  Z
		{  413,  460,   33,  116 },   //  91  [
		{  478,  460,   50,  116 },   //  92  backslash
		{  560,  460,   32,  116 },   //  93  ]
		{  624,  460,   53,  116 },   //  94  ^
		{  709,  460,   52,  116 },   //  95  _
		{  793,  460,   40,  116 },   //  96  `
		{  865,  460,  118,  116 },   //  97  a
		{ 1015,  460,  103,  116 },   //  98  b
		{ 1150,  460,   94,  116 },   //  99  c
		{ 1276,  460,  107,  116 },   // 100  d
		{ 1415,  460,   97,  116 },   // 101  e
		{ 1544,  460,   95,  116 },   // 102  f
		{ 1671,  460,  101,  116 },   // 103  g
		{ 1804,  460,  103,  116 },   // 104  h
		{ 1939,  460,   22,  116 },   // 105  i
		{   16,  608,   81,  116 },   // 106  j
		{  129,  608,  111,  116 },   // 107  k
		{  272,  608,   88,  116 },   // 108  l
		{  392,  608,  122,  116 },   // 109  m
		{  546,  608,  110,  116 },   // 110  n
		{  688,  608,  105,  116 },   // 111  o
		{  825,  608,  100,  116 },   // 112  p
		{  957,  608,  105,  116 },   // 113  q
		{ 1094,  608,  108,  116 },   // 114  r
		{ 1234,  608,   89,  116 },   // 115  s
		{ 1355,  608,   88,  116 },   // 116  t
		{ 1475,  608,  102,  116 },   // 117  u
		{ 1609,  608,  118,  116 },   // 118  v
		{ 1759,  608,  164,  116 },   // 119  w
		{   16,  756,  111,  116 },   // 120  x
		{  159,  756,   97,  116 },   // 121  y
		{  288,  756,   93,  116 },   // 122  z
		{  413,  756,   35,  116 },   // 123  {
		{  480,  756,   23,  116 },   // 124  |
		{  535,  756,   33,  116 },   // 125  }
		{  600,  756,   44,  116 },   // 126  ~
	};

	inline constexpr FCell BoldCells[NumGlyphs] =
	{
		{   16,   16,   34,  116 },   //  32  space
		{   82,   16,   40,  116 },   //  33  !
		{  154,   16,   58,  116 },   //  34  "
		{  244,   16,   52,  116 },   //  35  #
		{  328,   16,   72,  116 },   //  36  $
		{  432,   16,  114,  116 },   //  37  %
		{  578,   16,   75,  116 },   //  38  &
		{  685,   16,   29,  116 },   //  39  '
		{  746,   16,   37,  116 },   //  40  (
		{  815,   16,   37,  116 },   //  41  )
		{  884,   16,   51,  116 },   //  42  *
		{  967,   16,   72,  116 },   //  43  +
		{ 1071,   16,   33,  116 },   //  44  ,
		{ 1136,   16,   32,  116 },   //  45  -
		{ 1200,   16,   34,  116 },   //  46  .
		{ 1266,   16,   61,  116 },   //  47  /
		{ 1359,   16,  108,  116 },   //  48  0
		{ 1499,   16,   53,  116 },   //  49  1
		{ 1584,   16,  104,  116 },   //  50  2
		{ 1720,   16,   99,  116 },   //  51  3
		{ 1851,   16,  109,  116 },   //  52  4
		{   16,  164,  103,  116 },   //  53  5
		{  151,  164,  105,  116 },   //  54  6
		{  288,  164,   89,  116 },   //  55  7
		{  409,  164,  103,  116 },   //  56  8
		{  544,  164,  104,  116 },   //  57  9
		{  680,  164,   36,  116 },   //  58  :
		{  748,  164,   36,  116 },   //  59  ;
		{  816,  164,   59,  116 },   //  60  <
		{  907,  164,   72,  116 },   //  61  =
		{ 1011,  164,   59,  116 },   //  62  >
		{ 1102,  164,   82,  116 },   //  63  ?
		{ 1216,  164,   83,  116 },   //  64  @
		{ 1331,  164,  136,  116 },   //  65  A
		{ 1499,  164,  115,  116 },   //  66  B
		{ 1646,  164,  106,  116 },   //  67  C
		{ 1784,  164,  120,  116 },   //  68  D
		{   16,  312,  109,  116 },   //  69  E
		{  157,  312,  106,  116 },   //  70  F
		{  295,  312,  113,  116 },   //  71  G
		{  440,  312,  115,  116 },   //  72  H
		{  587,  312,   49,  116 },   //  73  I
		{  668,  312,   91,  116 },   //  74  J
		{  791,  312,  125,  116 },   //  75  K
		{  948,  312,   98,  116 },   //  76  L
		{ 1078,  312,  136,  116 },   //  77  M
		{ 1246,  312,  123,  116 },   //  78  N
		{ 1401,  312,  117,  116 },   //  79  O
		{ 1550,  312,  112,  116 },   //  80  P
		{ 1694,  312,  116,  116 },   //  81  Q
		{ 1842,  312,  121,  116 },   //  82  R
		{   16,  460,  100,  116 },   //  83  S
		{  148,  460,   99,  116 },   //  84  T
		{  279,  460,  114,  116 },   //  85  U
		{  425,  460,  136,  116 },   //  86  V
		{  593,  460,  188,  116 },   //  87  W
		{  813,  460,  124,  116 },   //  88  X
		{  969,  460,  109,  116 },   //  89  Y
		{ 1110,  460,  104,  116 },   //  90  Z
		{ 1246,  460,   37,  116 },   //  91  [
		{ 1315,  460,   61,  116 },   //  92  backslash
		{ 1408,  460,   36,  116 },   //  93  ]
		{ 1476,  460,   51,  116 },   //  94  ^
		{ 1559,  460,   59,  116 },   //  95  _
		{ 1650,  460,   43,  116 },   //  96  `
		{ 1725,  460,  136,  116 },   //  97  a
		{ 1893,  460,  115,  116 },   //  98  b
		{   16,  608,  106,  116 },   //  99  c
		{  154,  608,  120,  116 },   // 100  d
		{  306,  608,  109,  116 },   // 101  e
		{  447,  608,  106,  116 },   // 102  f
		{  585,  608,  113,  116 },   // 103  g
		{  730,  608,  115,  116 },   // 104  h
		{  877,  608,   49,  116 },   // 105  i
		{  958,  608,   91,  116 },   // 106  j
		{ 1081,  608,  125,  116 },   // 107  k
		{ 1238,  608,   98,  116 },   // 108  l
		{ 1368,  608,  136,  116 },   // 109  m
		{ 1536,  608,  123,  116 },   // 110  n
		{ 1691,  608,  117,  116 },   // 111  o
		{ 1840,  608,  112,  116 },   // 112  p
		{   16,  756,  116,  116 },   // 113  q
		{  164,  756,  121,  116 },   // 114  r
		{  317,  756,  100,  116 },   // 115  s
		{  449,  756,   99,  116 },   // 116  t
		{  580,  756,  114,  116 },   // 117  u
		{  726,  756,  136,  116 },   // 118  v
		{  894,  756,  188,  116 },   // 119  w
		{ 1114,  756,  124,  116 },   // 120  x
		{ 1270,  756,  109,  116 },   // 121  y
		{ 1411,  756,  104,  116 },   // 122  z
		{ 1547,  756,   39,  116 },   // 123  {
		{ 1618,  756,   27,  116 },   // 124  |
		{ 1677,  756,   37,  116 },   // 125  }
		{ 1746,  756,   50,  116 },   // 126  ~
	};

	inline constexpr FCell HudCells[NumGlyphs] =
	{
		{   16,   16,   25,  116 },   //  32  space
		{   73,   16,   32,  116 },   //  33  !
		{  137,   16,   51,  116 },   //  34  "
		{  220,   16,   85,  116 },   //  35  #
		{  337,   16,   70,  116 },   //  36  $
		{  439,   16,  120,  116 },   //  37  %
		{  591,   16,   76,  116 },   //  38  &
		{  699,   16,   28,  116 },   //  39  '
		{  759,   16,   43,  116 },   //  40  (
		{  834,   16,   43,  116 },   //  41  )
		{  909,   16,   57,  116 },   //  42  *
		{  998,   16,   59,  116 },   //  43  +
		{ 1089,   16,   32,  116 },   //  44  ,
		{ 1153,   16,   59,  116 },   //  45  -
		{ 1244,   16,   32,  116 },   //  46  .
		{ 1308,   16,   54,  116 },   //  47  /
		{ 1394,   16,   72,  116 },   //  48  0
		{ 1498,   16,   65,  116 },   //  49  1
		{ 1595,   16,   67,  116 },   //  50  2
		{ 1694,   16,   67,  116 },   //  51  3
		{ 1793,   16,   70,  116 },   //  52  4
		{ 1895,   16,   70,  116 },   //  53  5
		{   16,  164,   72,  116 },   //  54  6
		{  120,  164,   61,  116 },   //  55  7
		{  213,  164,   65,  116 },   //  56  8
		{  310,  164,   72,  116 },   //  57  9
		{  414,  164,   32,  116 },   //  58  :
		{  478,  164,   32,  116 },   //  59  ;
		{  542,  164,   54,  116 },   //  60  <
		{  628,  164,   59,  116 },   //  61  =
		{  719,  164,   54,  116 },   //  62  >
		{  805,  164,   63,  116 },   //  63  ?
		{  900,  164,  120,  116 },   //  64  @
		{ 1052,  164,   76,  116 },   //  65  A
		{ 1160,  164,   67,  116 },   //  66  B
		{ 1259,  164,   70,  116 },   //  67  C
		{ 1361,  164,   70,  116 },   //  68  D
		{ 1463,  164,   62,  116 },   //  69  E
		{ 1557,  164,   62,  116 },   //  70  F
		{ 1651,  164,   71,  116 },   //  71  G
		{ 1754,  164,   74,  116 },   //  72  H
		{ 1860,  164,   34,  116 },   //  73  I
		{ 1926,  164,   60,  116 },   //  74  J
		{   16,  312,   74,  116 },   //  75  K
		{  122,  312,   60,  116 },   //  76  L
		{  214,  312,   87,  116 },   //  77  M
		{  333,  312,   75,  116 },   //  78  N
		{  440,  312,   71,  116 },   //  79  O
		{  543,  312,   67,  116 },   //  80  P
		{  642,  312,   71,  116 },   //  81  Q
		{  745,  312,   74,  116 },   //  82  R
		{  851,  312,   70,  116 },   //  83  S
		{  953,  312,   63,  116 },   //  84  T
		{ 1048,  312,   72,  116 },   //  85  U
		{ 1152,  312,   73,  116 },   //  86  V
		{ 1257,  312,  108,  116 },   //  87  W
		{ 1397,  312,   75,  116 },   //  88  X
		{ 1504,  312,   74,  116 },   //  89  Y
		{ 1610,  312,   64,  116 },   //  90  Z
		{ 1706,  312,   43,  116 },   //  91  [
		{ 1781,  312,   54,  116 },   //  92  backslash
		{ 1867,  312,   43,  116 },   //  93  ]
		{ 1942,  312,   56,  116 },   //  94  ^
		{   16,  460,   59,  116 },   //  95  _
		{  107,  460,   58,  116 },   //  96  `
		{  197,  460,   66,  116 },   //  97  a
		{  295,  460,   67,  116 },   //  98  b
		{  394,  460,   63,  116 },   //  99  c
		{  489,  460,   67,  116 },   // 100  d
		{  588,  460,   65,  116 },   // 101  e
		{  685,  460,   52,  116 },   // 102  f
		{  769,  460,   67,  116 },   // 103  g
		{  868,  460,   65,  116 },   // 104  h
		{  965,  460,   50,  116 },   // 105  i
		{ 1047,  460,   38,  116 },   // 106  j
		{ 1117,  460,   65,  116 },   // 107  k
		{ 1214,  460,   35,  116 },   // 108  l
		{ 1281,  460,   94,  116 },   // 109  m
		{ 1407,  460,   66,  116 },   // 110  n
		{ 1505,  460,   66,  116 },   // 111  o
		{ 1603,  460,   67,  116 },   // 112  p
		{ 1702,  460,   67,  116 },   // 113  q
		{ 1801,  460,   64,  116 },   // 114  r
		{ 1897,  460,   63,  116 },   // 115  s
		{   16,  608,   51,  116 },   // 116  t
		{   99,  608,   65,  116 },   // 117  u
		{  196,  608,   61,  116 },   // 118  v
		{  289,  608,   94,  116 },   // 119  w
		{  415,  608,   66,  116 },   // 120  x
		{  513,  608,   61,  116 },   // 121  y
		{  606,  608,   57,  116 },   // 122  z
		{  695,  608,   55,  116 },   // 123  {
		{  782,  608,   28,  116 },   // 124  |
		{  842,  608,   55,  116 },   // 125  }
		{  929,  608,   58,  116 },   // 126  ~
	};

	// =============================================================================
	// PER WEIGHT — the sheet, its dimensions, its measured cap height, its grid
	// =============================================================================

	struct FFace
	{
		/** "Light" / "Bold" / "Hud". The name TraceText::WeightFromName() matches. The
		  * fallback face reuses this struct and calls itself "Names", but it is NOT in Faces[]
		  * and WeightFromName() must never return it — nothing may ASK to draw in it. */
		const TCHAR* Name;

		/** The font file this face was rasterised from. This is what a screenshot caption has to
		  * print to IDENTIFY the face rather than assert a flag — see spec v25 §4. */
		const TCHAR* Source;

		/** The imported sheet. Written by Scripts/import_font_atlas.py; loaded once at runtime. */
		const TCHAR* TextureAsset;

		/** Pixels per side eroded off the drawn face to synthesise this weight. 0 = as drawn. */
		float Erosion;

		/** Checked against the imported texture at runtime — a re-generated atlas that nobody
		  * re-imported is caught here and that WEIGHT stands down. */
		int32 AtlasWidth;
		int32 AtlasHeight;

		/** THIS face's split of ITS line box. Baseline and CapTop alignment read these.
		  * For every WEIGHT, Ascent + Descent == LineHeight — that is what check_weights_agree()
		  * enforces. THE FALLBACK FACE IS THE EXCEPTION: its box is taller (137 px against 116),
		  * so its Ascent + Descent does NOT equal LineHeight, and the difference in Ascent is
		  * exactly the shift that puts a fallback glyph on the drawing face's baseline. */
		float Ascent;
		float Descent;

		/** Cap height for THIS face. Align to it to sit type where a baked word sprite sat. */
		float CapHeight;

		const FCell* Cells;
	};

	inline constexpr FFace Faces[NumWeights] =
	{
		// Light — Sofachrome W05 ExtraLight.ttf, thin 0.0, ascent 95/descent 21, cap 65 px MEASURED off the 'H' cell's ink rows
		{ TEXT("Light"), TEXT("Sofachrome W05 ExtraLight.ttf"), TEXT("/Game/Trace/UI/Fonts/T_FontAtlas.T_FontAtlas"), 0.0f, 2048, 1024, 95.f, 21.f, 65.f, LightCells },
		// Bold — Sofachrome Rg.otf, thin 0.0, ascent 95/descent 21, cap 65 px MEASURED off the 'H' cell's ink rows
		{ TEXT("Bold"), TEXT("Sofachrome Rg.otf"), TEXT("/Game/Trace/UI/Fonts/T_FontAtlasBold.T_FontAtlasBold"), 0.0f, 2048, 1024, 95.f, 21.f, 65.f, BoldCells },
		// Hud — Erbaum-Bold.otf, thin 0.0, ascent 93/descent 23, cap 70 px MEASURED off the 'H' cell's ink rows
		{ TEXT("Hud"), TEXT("Erbaum-Bold.otf"), TEXT("/Game/Trace/UI/Fonts/T_FontAtlasHud.T_FontAtlasHud"), 0.0f, 2048, 1024, 93.f, 23.f, 70.f, HudCells },
	};

	/** Clamped, so a weight index that came in off a knob or a save game cannot walk off
	  * the end of the table — it draws in the default weight instead. */
	inline constexpr const FFace& Face(int32 WeightIndex)
	{
		return Faces[(WeightIndex >= 0 && WeightIndex < NumWeights) ? WeightIndex : DefaultWeight];
	}

	// =============================================================================
	// THE FALLBACK FACE — one glyph at a time, and NOT a weight (UI plan WP12)
	// =============================================================================
	//
	// THE PROBLEM IT SOLVES. The three sheets above are printable ASCII. Every string this
	// game AUTHORS is inside that set; the one class of string it does not author is a PLAYER
	// NAME, and a "Björn" arriving over the network used to draw a hole where its ö should be
	// (it advanced by a space, so at least the row's box stayed honest — but a hole is a hole).
	//
	// WHY THE FIX IS A FOURTH SHEET AND NOT MORE CELLS IN THE FIRST THREE. Sofachrome and
	// Erbaum are licensed for desktop use and this repository is public; rasterising ANOTHER
	// 96 codepoints of them would deepen exactly the licensing exposure docs/FONTS.md flags.
	// Lato-Regular.ttf is OFL, its .ttf is already committed, and so is this sheet.
	//
	// WHY IT IS NOT A FOURTH ETraceTextWeight. A weight is something a CALLER ASKS FOR, and
	// nothing should ever ask for this one — it is reached per GLYPH by a codepoint the chosen
	// face has no cell for. Putting it in the enum would put "Lato" in a UMG dropdown and in
	// WeightFromName(), which is the two-typefaces defect spec v23 §A4 removed.
	//
	// WHAT IT SHARES WITH THE WEIGHTS, AND WHAT IT DOES NOT:
	//   SHARED:      EmSize. Everything is scaled by (Size / EmSize) and there is one EmSize,
	//                so a fallback glyph in a correct line comes out the correct size. Enforced
	//                by import_font_atlas.py's check_fallback().
	//   NOT SHARED:  the LINE BOX. Lato-Regular.ttf splits 137 px at this em where the weights split 116.
	//                A fallback glyph is therefore drawn at (weight ascent - fallback ascent)
	//                px from the line top, which puts its baseline on the line's baseline.
	//   NOT SHARED:  the CHARSET. 199 codepoints in 9 runs — the C1 block, U+00AD and the
	//                gaps before the typographic marks are all skipped on purpose. That is why
	//                this face has a RANGE TABLE and the weights have a FirstCode.
	//
	// AND THE ONE THING THAT COULD GO WRONG, MEASURED RATHER THAN ARGUED: a dropped-in glyph
	// whose ink left the shared line box would collide with the line above or below it in a
	// multi-line label, silently. This sheet's ink spans rows 17..126 of its own 137 px cell
	// (MEASURED across all 199 cells), so after the baseline shift it occupies:
	//     in a Light line (ascent 95): shift -13 px  ->  ink 4..113  inside 0..116
	//     in a Bold  line (ascent 95): shift -13 px  ->  ink 4..113  inside 0..116
	//     in a Hud   line (ascent 93): shift -15 px  ->  ink 2..111  inside 0..116
	// import_font_atlas.py refuses to write this file if any of those rows leaves the box.

	inline constexpr int32 NumFallbackGlyphs = 199;
	inline constexpr int32 NumFallbackRanges = 9;

	/** One contiguous run of the fallback charset. Cells[FirstIndex + (Code - First)] is the
	  * cell for Code, for any Code in [First, Last]. */
	struct FCodeRange
	{
		int32 First;
		int32 Last;
		int32 FirstIndex;
	};

	inline constexpr FCodeRange FallbackRanges[NumFallbackRanges] =
	{
		{     32,    126,    0 },   // U+0020..U+007E, 95 glyph(s)
		{    160,    172,   95 },   // U+00A0..U+00AC, 13 glyph(s)
		{    174,    255,  108 },   // U+00AE..U+00FF, 82 glyph(s)
		{   8211,   8212,  190 },   // U+2013..U+2014, 2 glyph(s)
		{   8216,   8217,  192 },   // U+2018..U+2019, 2 glyph(s)
		{   8220,   8221,  194 },   // U+201C..U+201D, 2 glyph(s)
		{   8226,   8226,  196 },   // U+2022..U+2022, 1 glyph(s)
		{   8230,   8230,  197 },   // U+2026..U+2026, 1 glyph(s)
		{   8364,   8364,  198 },   // U+20AC..U+20AC, 1 glyph(s)
	};

	inline constexpr FCell FallbackCells[NumFallbackGlyphs] =
	{
		{   16,   16,   25,  137 },   //    32  space
		{   73,   16,   26,  137 },   //    33  !
		{  131,   16,   36,  137 },   //    34  "
		{  199,   16,   56,  137 },   //    35  #
		{  287,   16,   56,  137 },   //    36  $
		{  375,   16,   77,  137 },   //    37  %
		{  484,   16,   68,  137 },   //    38  &
		{  584,   16,   20,  137 },   //    39  '
		{  636,   16,   26,  137 },   //    40  (
		{  694,   16,   26,  137 },   //    41  )
		{  752,   16,   41,  137 },   //    42  *
		{  825,   16,   56,  137 },   //    43  +
		{  913,   16,   22,  137 },   //    44  ,
		{  967,   16,   36,  137 },   //    45  -
		{ 1035,   16,   23,  137 },   //    46  .
		{ 1090,   16,   43,  137 },   //    47  /
		{ 1165,   16,   56,  137 },   //    48  0
		{ 1253,   16,   56,  137 },   //    49  1
		{ 1341,   16,   56,  137 },   //    50  2
		{ 1429,   16,   56,  137 },   //    51  3
		{ 1517,   16,   56,  137 },   //    52  4
		{ 1605,   16,   56,  137 },   //    53  5
		{ 1693,   16,   56,  137 },   //    54  6
		{ 1781,   16,   56,  137 },   //    55  7
		{ 1869,   16,   56,  137 },   //    56  8
		{ 1957,   16,   56,  137 },   //    57  9
		{   16,  185,   24,  137 },   //    58  :
		{   72,  185,   25,  137 },   //    59  ;
		{  129,  185,   56,  137 },   //    60  <
		{  217,  185,   56,  137 },   //    61  =
		{  305,  185,   56,  137 },   //    62  >
		{  393,  185,   43,  137 },   //    63  ?
		{  468,  185,   80,  137 },   //    64  @
		{  580,  185,   65,  137 },   //    65  A
		{  677,  185,   62,  137 },   //    66  B
		{  771,  185,   64,  137 },   //    67  C
		{  867,  185,   73,  137 },   //    68  D
		{  972,  185,   55,  137 },   //    69  E
		{ 1059,  185,   54,  137 },   //    70  F
		{ 1145,  185,   70,  137 },   //    71  G
		{ 1247,  185,   73,  137 },   //    72  H
		{ 1352,  185,   27,  137 },   //    73  I
		{ 1411,  185,   41,  137 },   //    74  J
		{ 1484,  185,   64,  137 },   //    75  K
		{ 1580,  185,   49,  137 },   //    76  L
		{ 1661,  185,   89,  137 },   //    77  M
		{ 1782,  185,   73,  137 },   //    78  N
		{ 1887,  185,   77,  137 },   //    79  O
		{   16,  354,   58,  137 },   //    80  P
		{  106,  354,   77,  137 },   //    81  Q
		{  215,  354,   60,  137 },   //    82  R
		{  307,  354,   52,  137 },   //    83  S
		{  391,  354,   57,  137 },   //    84  T
		{  480,  354,   71,  137 },   //    85  U
		{  583,  354,   65,  137 },   //    86  V
		{  680,  354,   99,  137 },   //    87  W
		{  811,  354,   62,  137 },   //    88  X
		{  905,  354,   60,  137 },   //    89  Y
		{  997,  354,   58,  137 },   //    90  Z
		{ 1087,  354,   29,  137 },   //    91  [
		{ 1148,  354,   43,  137 },   //    92  backslash
		{ 1223,  354,   29,  137 },   //    93  ]
		{ 1284,  354,   56,  137 },   //    94  ^
		{ 1372,  354,   44,  137 },   //    95  _
		{ 1448,  354,   38,  137 },   //    96  `
		{ 1518,  354,   48,  137 },   //    97  a
		{ 1598,  354,   54,  137 },   //    98  b
		{ 1684,  354,   46,  137 },   //    99  c
		{ 1762,  354,   54,  137 },   //   100  d
		{ 1848,  354,   51,  137 },   //   101  e
		{ 1931,  354,   34,  137 },   //   102  f
		{   16,  523,   50,  137 },   //   103  g
		{   98,  523,   54,  137 },   //   104  h
		{  184,  523,   23,  137 },   //   105  i
		{  239,  523,   23,  137 },   //   106  j
		{  294,  523,   49,  137 },   //   107  k
		{  375,  523,   23,  137 },   //   108  l
		{  430,  523,   79,  137 },   //   109  m
		{  541,  523,   54,  137 },   //   110  n
		{  627,  523,   54,  137 },   //   111  o
		{  713,  523,   54,  137 },   //   112  p
		{  799,  523,   54,  137 },   //   113  q
		{  885,  523,   35,  137 },   //   114  r
		{  952,  523,   42,  137 },   //   115  s
		{ 1026,  523,   34,  137 },   //   116  t
		{ 1092,  523,   54,  137 },   //   117  u
		{ 1178,  523,   50,  137 },   //   118  v
		{ 1260,  523,   75,  137 },   //   119  w
		{ 1367,  523,   48,  137 },   //   120  x
		{ 1447,  523,   49,  137 },   //   121  y
		{ 1528,  523,   43,  137 },   //   122  z
		{ 1603,  523,   29,  137 },   //   123  {
		{ 1664,  523,   24,  137 },   //   124  |
		{ 1720,  523,   29,  137 },   //   125  }
		{ 1781,  523,   56,  137 },   //   126  ~
		{ 1869,  523,   25,  137 },   //   160  no-break space
		{ 1926,  523,   24,  137 },   //   161  U+00A1  ¡
		{   16,  692,   56,  137 },   //   162  U+00A2  ¢
		{  104,  692,   56,  137 },   //   163  U+00A3  £
		{  192,  692,   56,  137 },   //   164  U+00A4  ¤
		{  280,  692,   56,  137 },   //   165  U+00A5  ¥
		{  368,  692,   24,  137 },   //   166  U+00A6  ¦
		{  424,  692,   48,  137 },   //   167  U+00A7  §
		{  504,  692,   38,  137 },   //   168  U+00A8  ¨
		{  574,  692,   80,  137 },   //   169  U+00A9  ©
		{  686,  692,   35,  137 },   //   170  U+00AA  ª
		{  753,  692,   41,  137 },   //   171  U+00AB  «
		{  826,  692,   56,  137 },   //   172  U+00AC  ¬
		{  914,  692,   80,  137 },   //   174  U+00AE  ®
		{ 1026,  692,   38,  137 },   //   175  U+00AF  ¯
		{ 1096,  692,   40,  137 },   //   176  U+00B0  °
		{ 1168,  692,   56,  137 },   //   177  U+00B1  ±
		{ 1256,  692,   32,  137 },   //   178  U+00B2  ²
		{ 1320,  692,   32,  137 },   //   179  U+00B3  ³
		{ 1384,  692,   38,  137 },   //   180  U+00B4  ´
		{ 1454,  692,   62,  137 },   //   181  U+00B5  µ
		{ 1548,  692,   67,  137 },   //   182  U+00B6  ¶
		{ 1647,  692,   25,  137 },   //   183  U+00B7  ·
		{ 1704,  692,   38,  137 },   //   184  U+00B8  ¸
		{ 1774,  692,   32,  137 },   //   185  U+00B9  ¹
		{ 1838,  692,   39,  137 },   //   186  U+00BA  º
		{ 1909,  692,   41,  137 },   //   187  U+00BB  »
		{   16,  861,   70,  137 },   //   188  U+00BC  ¼
		{  118,  861,   70,  137 },   //   189  U+00BD  ½
		{  220,  861,   71,  137 },   //   190  U+00BE  ¾
		{  323,  861,   42,  137 },   //   191  U+00BF  ¿
		{  397,  861,   65,  137 },   //   192  U+00C0  À
		{  494,  861,   65,  137 },   //   193  U+00C1  Á
		{  591,  861,   65,  137 },   //   194  U+00C2  Â
		{  688,  861,   65,  137 },   //   195  U+00C3  Ã
		{  785,  861,   65,  137 },   //   196  U+00C4  Ä
		{  882,  861,   65,  137 },   //   197  U+00C5  Å
		{  979,  861,   89,  137 },   //   198  U+00C6  Æ
		{ 1100,  861,   64,  137 },   //   199  U+00C7  Ç
		{ 1196,  861,   55,  137 },   //   200  U+00C8  È
		{ 1283,  861,   55,  137 },   //   201  U+00C9  É
		{ 1370,  861,   55,  137 },   //   202  U+00CA  Ê
		{ 1457,  861,   55,  137 },   //   203  U+00CB  Ë
		{ 1544,  861,   27,  137 },   //   204  U+00CC  Ì
		{ 1603,  861,   27,  137 },   //   205  U+00CD  Í
		{ 1662,  861,   27,  137 },   //   206  U+00CE  Î
		{ 1721,  861,   27,  137 },   //   207  U+00CF  Ï
		{ 1780,  861,   74,  137 },   //   208  U+00D0  Ð
		{ 1886,  861,   73,  137 },   //   209  U+00D1  Ñ
		{   16, 1030,   77,  137 },   //   210  U+00D2  Ò
		{  125, 1030,   77,  137 },   //   211  U+00D3  Ó
		{  234, 1030,   77,  137 },   //   212  U+00D4  Ô
		{  343, 1030,   77,  137 },   //   213  U+00D5  Õ
		{  452, 1030,   77,  137 },   //   214  U+00D6  Ö
		{  561, 1030,   56,  137 },   //   215  U+00D7  ×
		{  649, 1030,   77,  137 },   //   216  U+00D8  Ø
		{  758, 1030,   71,  137 },   //   217  U+00D9  Ù
		{  861, 1030,   71,  137 },   //   218  U+00DA  Ú
		{  964, 1030,   71,  137 },   //   219  U+00DB  Û
		{ 1067, 1030,   71,  137 },   //   220  U+00DC  Ü
		{ 1170, 1030,   60,  137 },   //   221  U+00DD  Ý
		{ 1262, 1030,   57,  137 },   //   222  U+00DE  Þ
		{ 1351, 1030,   56,  137 },   //   223  U+00DF  ß
		{ 1439, 1030,   48,  137 },   //   224  U+00E0  à
		{ 1519, 1030,   48,  137 },   //   225  U+00E1  á
		{ 1599, 1030,   48,  137 },   //   226  U+00E2  â
		{ 1679, 1030,   48,  137 },   //   227  U+00E3  ã
		{ 1759, 1030,   48,  137 },   //   228  U+00E4  ä
		{ 1839, 1030,   48,  137 },   //   229  U+00E5  å
		{ 1919, 1030,   77,  137 },   //   230  U+00E6  æ
		{   16, 1199,   46,  137 },   //   231  U+00E7  ç
		{   94, 1199,   51,  137 },   //   232  U+00E8  è
		{  177, 1199,   51,  137 },   //   233  U+00E9  é
		{  260, 1199,   51,  137 },   //   234  U+00EA  ê
		{  343, 1199,   51,  137 },   //   235  U+00EB  ë
		{  426, 1199,   23,  137 },   //   236  U+00EC  ì
		{  481, 1199,   23,  137 },   //   237  U+00ED  í
		{  536, 1199,   23,  137 },   //   238  U+00EE  î
		{  591, 1199,   23,  137 },   //   239  U+00EF  ï
		{  646, 1199,   54,  137 },   //   240  U+00F0  ð
		{  732, 1199,   54,  137 },   //   241  U+00F1  ñ
		{  818, 1199,   54,  137 },   //   242  U+00F2  ò
		{  904, 1199,   54,  137 },   //   243  U+00F3  ó
		{  990, 1199,   54,  137 },   //   244  U+00F4  ô
		{ 1076, 1199,   54,  137 },   //   245  U+00F5  õ
		{ 1162, 1199,   54,  137 },   //   246  U+00F6  ö
		{ 1248, 1199,   56,  137 },   //   247  U+00F7  ÷
		{ 1336, 1199,   54,  137 },   //   248  U+00F8  ø
		{ 1422, 1199,   54,  137 },   //   249  U+00F9  ù
		{ 1508, 1199,   54,  137 },   //   250  U+00FA  ú
		{ 1594, 1199,   54,  137 },   //   251  U+00FB  û
		{ 1680, 1199,   54,  137 },   //   252  U+00FC  ü
		{ 1766, 1199,   49,  137 },   //   253  U+00FD  ý
		{ 1847, 1199,   54,  137 },   //   254  U+00FE  þ
		{ 1933, 1199,   49,  137 },   //   255  U+00FF  ÿ
		{   16, 1368,   56,  137 },   //  8211  U+2013  –
		{  104, 1368,   76,  137 },   //  8212  U+2014  —
		{  212, 1368,   21,  137 },   //  8216  U+2018  ‘
		{  265, 1368,   20,  137 },   //  8217  U+2019  ’
		{  317, 1368,   35,  137 },   //  8220  U+201C  “
		{  384, 1368,   35,  137 },   //  8221  U+201D  ”
		{  451, 1368,   56,  137 },   //  8226  U+2022  •
		{  539, 1368,   72,  137 },   //  8230  U+2026  …
		{  643, 1368,   56,  137 },   //  8364  U+20AC  €
	};

	/** '?' in the table above — what a codepoint MISSING FROM BOTH sheets draws. A visible
	  * question mark is the honest answer there; advancing silently is what produced the hole
	  * this face exists to remove. */
	inline constexpr int32 FallbackQuestionIndex = 31;

	// Names — Lato-Regular.ttf, ascent 108/descent 29, cap 70 px MEASURED off the 'H' cell's ink rows
	inline constexpr FFace FallbackFace =
		{ TEXT("Names"), TEXT("Lato-Regular.ttf"), TEXT("/Game/Trace/UI/Fonts/T_FontAtlasNames.T_FontAtlasNames"), 0.0f, 2048, 2048, 108.f, 29.f, 70.f, FallbackCells };

	/** The index into FallbackCells for @p Code, or INDEX_NONE. Linear over 9 ranges — it
	  * is only ever reached for a codepoint the DRAWING face already failed to supply, which
	  * is a handful of glyphs in a player name and never a whole authored string. */
	inline constexpr int32 FallbackIndexOf(int32 Code)
	{
		for (const FCodeRange& Range : FallbackRanges)
		{
			if (Code >= Range.First && Code <= Range.Last)
			{
				return Range.FirstIndex + (Code - Range.First);
			}
		}
		return INDEX_NONE;
	}
}
