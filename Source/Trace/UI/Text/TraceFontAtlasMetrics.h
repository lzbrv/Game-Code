// GENERATED FILE — DO NOT EDIT BY HAND.
// Produced by Scripts/import_font_atlas.py from
//   Content/Trace/UI/Fonts/Source/T_FontAtlas.json   (Light, sha1 2c02bf95f91e4c71efce5a3e684d8d7b03f17af1)
//   Content/Trace/UI/Fonts/Source/T_FontAtlasBold.json   (Bold, sha1 33bc4c364550c974efe6132585f37947cb1c24ee)
//   Content/Trace/UI/Fonts/Source/T_FontAtlasHud.json   (Hud, sha1 d84c43da61a35c2590988dfc35c7811065df739c)
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
//   sheets ever disagree about them.
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
	// SHARED BY EVERY FACE — enforced by the generator, so layout can rely on it
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

	inline constexpr int32 FirstCode = 32;
	inline constexpr int32 LastCode  = 126;
	inline constexpr int32 NumGlyphs = 95;

	/** One cell, in atlas pixels. Index by (code - FirstCode); the charset is contiguous. */
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
		/** "Light" / "Bold" / "Hud". The name TraceText::WeightFromName() matches. */
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

		/** THIS face's split of the shared line box. Baseline and CapTop alignment read these;
		  * Ascent + Descent == LineHeight for every face, which is what the guard enforces. */
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
}
