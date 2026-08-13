// GENERATED FILE — DO NOT EDIT BY HAND.
// Produced by Scripts/import_font_atlas.py from
// Content/Trace/UI/Fonts/Source/T_FontAtlas.json (sha1 f68332f0c0d54cd73b99505497e17a816e9c8a1c).
//
// THE ONE METRICS SOURCE (spec v22 §A1). Both renderers — the Canvas blitter in
// TraceCanvasText.cpp and the Slate leaf in TraceAtlasTextWidget.cpp — lay text out through
// TraceText::LayoutString(), and TraceText.cpp is the ONLY file in the project that includes
// this header. That is what makes "one source" structural rather than a promise in a
// comment: the two paths cannot drift because there is only one layout pass.
//
// THE CELL MODEL, because it is what makes this table so small:
//   Every cell is one FULL ADVANCE WIDTH by one FULL LINE HEIGHT, and the glyph is drawn
//   inside it with its own bearings already applied. So the pen advances by uSize and every
//   glyph sits on a shared baseline — there is no bearing table and there is no kerning.
//   Sofachrome is a wide squared face whose ink runs flush to the advance on A, T, V, W and
//   Y; that is the typeface, not clipping.
//
// Units are ATLAS PIXELS at an em of 96. On screen everything is multiplied by
// (Size / EmSize), where Size means the same thing it means in FSlateFontInfo::Size.
#pragma once

#include "CoreMinimal.h"

namespace TraceFontAtlasMetrics
{
	/** The typeface this was rasterised from. NOT in the repository — see docs/FONTS.md. */
	inline constexpr const TCHAR* SourceFont = TEXT("Sofachrome Rg.otf");

	/** The imported sheet. Written by Scripts/import_font_atlas.py; loaded once at runtime. */
	inline constexpr const TCHAR* TextureAsset =
		TEXT("/Game/Trace/UI/Fonts/T_FontAtlas.T_FontAtlas");

	/** Checked against the imported texture at runtime — a re-generated atlas that nobody
	  * re-imported is caught here and the whole atlas path stands down. */
	inline constexpr int32 AtlasWidth  = 2048;
	inline constexpr int32 AtlasHeight = 1024;

	/** Em size the sheet was rasterised at. Everything below is in these pixels. */
	inline constexpr float EmSize     = 96.f;
	inline constexpr float Ascent     = 95.f;
	inline constexpr float Descent    = 21.f;
	inline constexpr float LineHeight  = 116.f;

	/** Cap height — MEASURED off the 'H' cell's ink rows. Align to THIS to sit type where a baked word sprite sat. */
	inline constexpr float CapHeight  = 59.f;

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

	inline constexpr FCell Cells[NumGlyphs] =
	{
		{    2,    2,   34,  116 },   //  32  space
		{   40,    2,   40,  116 },   //  33  !
		{   84,    2,   58,  116 },   //  34  "
		{  146,    2,   52,  116 },   //  35  #
		{  202,    2,   72,  116 },   //  36  $
		{  278,    2,  114,  116 },   //  37  %
		{  396,    2,   75,  116 },   //  38  &
		{  475,    2,   29,  116 },   //  39  '
		{  508,    2,   37,  116 },   //  40  (
		{  549,    2,   37,  116 },   //  41  )
		{  590,    2,   51,  116 },   //  42  *
		{  645,    2,   72,  116 },   //  43  +
		{  721,    2,   33,  116 },   //  44  ,
		{  758,    2,   32,  116 },   //  45  -
		{  794,    2,   34,  116 },   //  46  .
		{  832,    2,   61,  116 },   //  47  /
		{  897,    2,  108,  116 },   //  48  0
		{ 1009,    2,   53,  116 },   //  49  1
		{ 1066,    2,  104,  116 },   //  50  2
		{ 1174,    2,   99,  116 },   //  51  3
		{ 1277,    2,  109,  116 },   //  52  4
		{ 1390,    2,  103,  116 },   //  53  5
		{ 1497,    2,  105,  116 },   //  54  6
		{ 1606,    2,   89,  116 },   //  55  7
		{ 1699,    2,  103,  116 },   //  56  8
		{ 1806,    2,  104,  116 },   //  57  9
		{ 1914,    2,   36,  116 },   //  58  :
		{ 1954,    2,   36,  116 },   //  59  ;
		{    2,  122,   59,  116 },   //  60  <
		{   65,  122,   72,  116 },   //  61  =
		{  141,  122,   59,  116 },   //  62  >
		{  204,  122,   82,  116 },   //  63  ?
		{  290,  122,   83,  116 },   //  64  @
		{  377,  122,  136,  116 },   //  65  A
		{  517,  122,  115,  116 },   //  66  B
		{  636,  122,  106,  116 },   //  67  C
		{  746,  122,  120,  116 },   //  68  D
		{  870,  122,  109,  116 },   //  69  E
		{  983,  122,  106,  116 },   //  70  F
		{ 1093,  122,  113,  116 },   //  71  G
		{ 1210,  122,  115,  116 },   //  72  H
		{ 1329,  122,   49,  116 },   //  73  I
		{ 1382,  122,   91,  116 },   //  74  J
		{ 1477,  122,  125,  116 },   //  75  K
		{ 1606,  122,   98,  116 },   //  76  L
		{ 1708,  122,  136,  116 },   //  77  M
		{ 1848,  122,  123,  116 },   //  78  N
		{    2,  242,  117,  116 },   //  79  O
		{  123,  242,  112,  116 },   //  80  P
		{  239,  242,  116,  116 },   //  81  Q
		{  359,  242,  121,  116 },   //  82  R
		{  484,  242,  100,  116 },   //  83  S
		{  588,  242,   99,  116 },   //  84  T
		{  691,  242,  114,  116 },   //  85  U
		{  809,  242,  136,  116 },   //  86  V
		{  949,  242,  188,  116 },   //  87  W
		{ 1141,  242,  124,  116 },   //  88  X
		{ 1269,  242,  109,  116 },   //  89  Y
		{ 1382,  242,  104,  116 },   //  90  Z
		{ 1490,  242,   37,  116 },   //  91  [
		{ 1531,  242,   61,  116 },   //  92  backslash
		{ 1596,  242,   36,  116 },   //  93  ]
		{ 1636,  242,   51,  116 },   //  94  ^
		{ 1691,  242,   59,  116 },   //  95  _
		{ 1754,  242,   43,  116 },   //  96  `
		{ 1801,  242,  136,  116 },   //  97  a
		{    2,  362,  115,  116 },   //  98  b
		{  121,  362,  106,  116 },   //  99  c
		{  231,  362,  120,  116 },   // 100  d
		{  355,  362,  109,  116 },   // 101  e
		{  468,  362,  106,  116 },   // 102  f
		{  578,  362,  113,  116 },   // 103  g
		{  695,  362,  115,  116 },   // 104  h
		{  814,  362,   49,  116 },   // 105  i
		{  867,  362,   91,  116 },   // 106  j
		{  962,  362,  125,  116 },   // 107  k
		{ 1091,  362,   98,  116 },   // 108  l
		{ 1193,  362,  136,  116 },   // 109  m
		{ 1333,  362,  123,  116 },   // 110  n
		{ 1460,  362,  117,  116 },   // 111  o
		{ 1581,  362,  112,  116 },   // 112  p
		{ 1697,  362,  116,  116 },   // 113  q
		{ 1817,  362,  121,  116 },   // 114  r
		{ 1942,  362,  100,  116 },   // 115  s
		{    2,  482,   99,  116 },   // 116  t
		{  105,  482,  114,  116 },   // 117  u
		{  223,  482,  136,  116 },   // 118  v
		{  363,  482,  188,  116 },   // 119  w
		{  555,  482,  124,  116 },   // 120  x
		{  683,  482,  109,  116 },   // 121  y
		{  796,  482,  104,  116 },   // 122  z
		{  904,  482,   39,  116 },   // 123  {
		{  947,  482,   27,  116 },   // 124  |
		{  978,  482,   37,  116 },   // 125  }
		{ 1019,  482,   50,  116 },   // 126  ~
	};
}
