#include "cl_dll/IGameClientExports.h"
#include "ClientInfo.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "BaseMenu.h"
#include "BaseWindow.h"
#include "Action.h"
#include "Table.h"
#include "BaseModel.h"
#include "Scoreboard.h"
#include "const.h"
#include "com_model.h"
#include "Utils.h"

// ============================================================
// Colors
// ============================================================

uint g_ColorBlue = PackRGB( 154, 204, 255 );
uint g_ColorRed = PackRGB( 255, 64, 64 );
uint g_ColorYellow = PackRGB( 255, 160, 0 );
uint g_ColorWhite = PackRGB( 255, 255, 255 );

// ============================================================
// constants
// ============================================================

#define SCOREBOARD_WIDTH  924
#define SCOREBOARD_HEIGHT 668
#define SCOREBOARD_TABLE_PADDING_X 12
#define SCOREBOARD_TABLE_PADDING_Y 2
#define SCOREBOARD_ROW_HEIGHT_MAX   32
#define SCOREBOARD_ROW_PADDING_MAX  32

#define SCOREBOARD_FRIEND_WIDTH  SCOREBOARD_ROW_HEIGHT_MAX
#define SCOREBOARD_AVATAR_WIDTH  SCOREBOARD_ROW_HEIGHT_MAX

#define SCOREBOARD_SEPARATOR_HEIGHT 1
#define SCOREBOARD_SPACER_HEIGHT    6
#define SCOREBOARD_ROW_PADDING_Y    2

#define UI_FRIEND_INDICATOR   "resource/icon_friend_indicator_scoreboard"

enum ETeamColor
{
	TEAM_COLOR_T,
	TEAM_COLOR_CT,
	TEAM_COLOR_SPECTATOR,
	TEAM_COLOR_UNASSIGMENT,
	TEAM_COLOR_NONE
};

static uint GetTeamColor( ETeamColor color )
{
	switch( color )
	{
	case TEAM_COLOR_T:                      return g_ColorRed;
	case TEAM_COLOR_CT:                     return g_ColorBlue;
	case TEAM_COLOR_SPECTATOR:
	case TEAM_COLOR_UNASSIGMENT:    return g_ColorWhite;
	default:                                return g_ColorYellow;
	}
}

// ============================================================
// Scoreboard rows
// ============================================================

enum scoreboard_row_e
{
	ROW_HEADER,
	ROW_SEPARATOR,
	ROW_SPACER,
	ROW_PLAYER,
	ROW_SPECTATOR
};

// ============================================================
// Player data
// ============================================================

struct player_t
{
	const char *name;
	const char *attrib;

	byte     thisplayer;

	int      sb_health;
	int      sb_account;
	int      deaths;
	int      kills;
	int      ping;

	uint64_t m_nSteamID;
	int      clientIndex;
};

// ============================================================
// Scoreboard row
// ============================================================

struct scoreboard_row_t
{
	scoreboard_row_e kind;

	ETeamColor color;

	bool thisplayer;
	bool isFriend;

	char name[64];
	char attrib[16];

	char health[8];
	char money[16];

	char score[8];
	char deaths[8];
	char ping[8];

	const char *avatar;
};

// ============================================================
// Steam presentation cache (avatar texture + friend flag)
//
// GetPlayerSteamInfo() hands back a *copy* of the broker's cache entry --
// there's no way for the UI to clear avatar_dirty on the master copy, so
// we can't use it to decide "do I need to re-upload". Instead we track
// our own idempotency here: once a steamid's avatar has been handed to
// PIC_Load, we never upload it again for the lifetime of this slot.
//
// Keyed by client slot index rather than steamid so it's a flat, bounded
// array indexed directly by the same loop variable Draw() already uses to
// walk MAX_CLIENTS -- no search needed. If a slot's steamid changes (a
// different player reconnected into the same slot), the entry is reset.
// ============================================================

struct scoreboard_steam_cache_t
{
	uint64_t steamid;
	bool     isFriend;
	bool     avatarUploaded;
	char     avatarPicName[32]; // "#steam_avatar_<16 hex digits>"
};

static scoreboard_steam_cache_t g_ScoreboardSteamCache[MAX_CLIENTS];

// Looks up (or fetches) Steam presence for `player` and writes the result
// into `row.isFriend` / `row.avatar`. Safe to call every frame: once an
// avatar has been uploaded once, this is just an array read plus one
// GetPlayerSteamInfo() call (itself a cheap cache hit on the broker side
// once status is READY).
static void UpdatePlayerSteamPresentation( scoreboard_row_t &row, const player_t &player )
{
	row.isFriend = false;
	row.avatar = NULL;

	if( player.clientIndex < 0 || player.clientIndex >= MAX_CLIENTS )
		return;

	scoreboard_steam_cache_t &cache = g_ScoreboardSteamCache[player.clientIndex];

	if( cache.steamid != player.m_nSteamID )
	{
		// Slot reused by a different SteamID (player left, someone else
		// took the slot) -- drop the stale texture and start fresh.
		if( cache.avatarUploaded )
			EngFuncs::PIC_Free( cache.avatarPicName );

		memset( &cache, 0, sizeof( cache ));
		cache.steamid = player.m_nSteamID;
	}

	if( !player.m_nSteamID )
		return; // no Steam identity for this player (bot, old demo, etc.)

	sbrk_player_info_t info;
	int status = g_pClient->GetPlayerSteamInfo( player.m_nSteamID, &info );

	if( status != SBRK_PLAYER_READY )
	{
		// Still resolves whatever we already have cached from an earlier
		// frame (e.g. avatar uploaded previously, relationship known) --
		// only bail out on the fields we can't fill in yet.
		row.isFriend = cache.isFriend;
		row.avatar = cache.avatarUploaded ? cache.avatarPicName : NULL;
		return;
	}

	// cache.isFriend = ( info.relationship == SBRK_PLAYER_RELATIONSHIP_FRIEND );

	if( !cache.avatarUploaded && info.avatar_png_size > 0 )
	{
		snprintf(
			cache.avatarPicName,
			sizeof(cache.avatarPicName),
			"#steam_avatar_%016" PRIx64,
			player.m_nSteamID
		);

		HIMAGE hPic = EngFuncs::PIC_Load( cache.avatarPicName, info.avatar_png, (int)info.avatar_png_size );

		if( hPic )
			cache.avatarUploaded = true;
	}

	cache.isFriend = cache.avatarUploaded && player.thisplayer == 0;

	row.isFriend = cache.isFriend;
	row.avatar = cache.avatarUploaded ? cache.avatarPicName : NULL;
}

// ============================================================
// Adaptive font tiers
// ============================================================

struct scoreboard_font_tier_t
{
	EFontSizes font;
	int        charHeight; // "logical" (unscaled) char height
};

static const scoreboard_font_tier_t g_ScoreboardFontTiers[] =
{
	{ QM_SMALLFONT,   UI_SMALL_CHAR_HEIGHT   },
	{ QM_SMALLERFONT, UI_SMALLER_CHAR_HEIGHT },
	{ QM_TINYFONT,    UI_TINY_CHAR_HEIGHT    },
};

#define SCOREBOARD_FONT_TIER_COUNT (int)( sizeof( g_ScoreboardFontTiers ) / sizeof( g_ScoreboardFontTiers[0] ))

// ============================================================
// Scoreboard model
// ============================================================

class CMenuScoreboardModel : public CMenuBaseModel
{
public:
	CMenuScoreboardModel() :
		CMenuBaseModel(),
		rowPaddingY( SCOREBOARD_ROW_PADDING_Y )
	{
		rows.EnsureCapacity( 40 );
	}

	void Update() override
	{
	}

	int GetColumns() const override
	{
		return 9;
	}

	int GetRows() const override
	{
		return rows.Count();
	}

	// NOTE ON UNITS: `defaultHeight` arrives from Table.cpp as m_scChSize,
	// which is already *scaled* (uiStatic.scaleX/Y applied via SetCharSize).
	// rowPaddingY, SCOREBOARD_SEPARATOR_HEIGHT and SCOREBOARD_SPACER_HEIGHT
	// are all *logical* constants/derived values, so they must be scaled
	// by uiStatic.scaleY before being combined with defaultHeight here --
	// otherwise padding/separator/spacer sizes drift out of proportion
	// with the text/avatar on any resolution where scaleY != 1.
	int GetRowHeight( int line, int defaultHeight ) const override
	{
		switch( rows[line].kind )
		{
		case ROW_SEPARATOR:
			return Q_max( 1, (int)( SCOREBOARD_SEPARATOR_HEIGHT * uiStatic.scaleY ));
		case ROW_SPACER:
			return (int)( SCOREBOARD_SPACER_HEIGHT * uiStatic.scaleY );
		case ROW_PLAYER:
		case ROW_SPECTATOR:
			return defaultHeight + (int)( rowPaddingY * uiStatic.scaleY ) * 2;
		default:
			return defaultHeight;
		}
	}

	ECellType GetCellType( int line, int column ) override
	{
		const scoreboard_row_t &row = rows[line];

		if( row.kind != ROW_PLAYER && row.kind != ROW_SPECTATOR )
			return CELL_TEXT;

		switch( column )
		{
		case 0:
			return CELL_IMAGE_ROWSCALED; // scales with rowHeight, same as avatar
		case 1:
			return CELL_IMAGE_ROWSCALED; // scales with rowHeight, not with the font
		default:
			return CELL_TEXT;
		}
	}

	const char *GetCellText( int line, int column ) override
	{
		const scoreboard_row_t &row = rows[line];

		if( row.kind == ROW_SEPARATOR || row.kind == ROW_SPACER )
			return NULL;

		switch( column )
		{
		case 0: return row.isFriend ? UI_FRIEND_INDICATOR : NULL;
		case 1: return row.avatar;
		case 2: return row.name;
		case 3: return row.attrib;
		case 4: return row.health;
		case 5: return row.money;
		case 6: return row.score;
		case 7: return row.deaths;
		case 8: return row.ping;
		}

		return NULL;
	}

	/*
	 * Color of the entire row.
	 *
	 * Used for the 1px separators (they fill the whole line) and for
	 * highlighting the local player's row.
	 */
	bool GetLineColor( int line, uint &color, bool &force ) const override
	{
		const scoreboard_row_t &row = rows[line];

		switch( row.kind )
		{
		case ROW_SEPARATOR:
			color = GetTeamColor( row.color );
			force = true;
			return true;

		case ROW_PLAYER:
			if( row.thisplayer )
			{
				color = PackRGBA( 255, 255, 255, 32 );
				force = true;
				return true;
			}
			break;

		default:
			break;
		}

		return false;
	}

	bool GetCellColors( int line, int column, uint &color, bool &force ) const override
	{
		const scoreboard_row_t &row = rows[line];

		switch( row.kind )
		{
		case ROW_HEADER:
			color = GetTeamColor( row.color );
			force = true;
			return true;

		case ROW_SPECTATOR:
			if( column == 0 || column == 1 ) // friend icon / avatar: draw untinted
				return false;
			color = GetTeamColor( TEAM_COLOR_SPECTATOR );
			force = false;
			return true;

		case ROW_SEPARATOR:
		case ROW_SPACER:
			return false;

		case ROW_PLAYER:
		default:
			if( column == 0 || column == 1 )
				return false;

			color = GetTeamColor( row.color );
			force = false;
			return true;
		}
	}

	unsigned int GetAlignmentForColumn( int column ) const override
	{
		switch( column )
		{
		case 0: // Friend
			return QM_RIGHT;
		case 1: // Avatar
		case 2: // Name
		case 3: // Attribute
			return QM_LEFT;
		case 4: // HP
		case 5: // Money
		case 6: // Score
		case 7: // Deaths
		case 8: // Ping
		default:
			return QM_RIGHT;
		}
	}

	CUtlVector<scoreboard_row_t> rows;
	int rowPaddingY;
};

// ============================================================
// Scoreboard window
// ============================================================

class CMenuScoreboard : public CMenuBaseWindow
{
public:
	typedef CMenuBaseWindow BaseClass;

	CMenuScoreboard() : CMenuBaseWindow( "Scoreboard", &uiStatic.menu ),
		m_iLastRowCount( -1 ),
		roundCornerSize( Size( 16, 16 )),
		m_iBackgroundAlpha( 125 ),
		m_eCurrentFont( g_ScoreboardFontTiers[0].font ),
		m_iCurrentRowHeight( SCOREBOARD_ROW_HEIGHT_MAX )
	{
	}

	void Init() override;
	void VidInit() override;
	void Draw() override;

	void Clear();

private:
	static int PlayerCompar( const void *a, const void *b );

	void AddSectionHeader( ETeamColor color, const char *displayName, int score, bool hasScore );
	void AddSeparator( ETeamColor color );
	void AddSpacer();

	void AddPlayerRow( const player_t &player, ETeamColor color );
	void AddSpectatorRow( const player_t &player );

	void AddTeamSection( ETeamColor color, const char *displayName, int score, const CUtlVector<player_t> &players );

	void DrawBackground();

	int  CalcFixedRowsHeight( int charHeight ) const;
	bool UpdateRowLayout( int numFlexRows );

private:
	CMenuScoreboardModel model;
	CMenuTable table;

	CUtlVector<player_t> CTs_players;
	CUtlVector<player_t> Ts_players;
	CUtlVector<player_t> spectators_players;

	char serverName_buf[256];
	int  m_iLastRowCount;
	Size roundCornerSize;
	int  m_iBackgroundAlpha;
	EFontSizes m_eCurrentFont;
	int m_iCurrentRowHeight;
};

static CMenuScoreboard *scoreboard = NULL;

static void Scoreboard_Precache()
{
	scoreboard = new CMenuScoreboard();
}

static void Scoreboard_Shutdown()
{
	delete scoreboard;
	scoreboard = NULL;
}

ADD_MENU4( scoreboard, Scoreboard_Precache, UI_DrawScoreboard, Scoreboard_Shutdown );

// ============================================================
// Sorting
// ============================================================

int CMenuScoreboard::PlayerCompar( const void *a, const void *b )
{
	const player_t *_a = (const player_t *)a;
	const player_t *_b = (const player_t *)b;

	if( _a->kills < _b->kills )
		return 1;

	if( _a->kills > _b->kills )
		return -1;

	return 0;
}

static void FormatSectionTitle( char *buf, size_t size, const char *name, int count )
{
	snprintf( buf, size, "%s - %d %s", name, count, count == 1 ? "player" : "players" );
}

// ============================================================
// Adaptive layout
// ============================================================
int CMenuScoreboard::CalcFixedRowsHeight( int charHeight ) const
{
	int total = (int)( charHeight * HEADER_HEIGHT_FRAC ); // шапка таблицы (SCORE/DEATHS/...)

	for( int i = 0; i < model.rows.Count(); i++ )
	{
		switch( model.rows[i].kind )
		{
		case ROW_HEADER:    total += charHeight; break;
		case ROW_SEPARATOR: total += SCOREBOARD_SEPARATOR_HEIGHT; break;
		case ROW_SPACER:    total += SCOREBOARD_SPACER_HEIGHT; break;
		default: break;
		}
	}

	return total;
}

bool CMenuScoreboard::UpdateRowLayout( int numFlexRows )
{
	if( numFlexRows <= 0 )
		numFlexRows = 1;

	const int boxHeight = size.h - SCOREBOARD_TABLE_PADDING_Y * 2;

	for( int i = 0; i < SCOREBOARD_FONT_TIER_COUNT; i++ )
	{
		const EFontSizes font     = g_ScoreboardFontTiers[i].font;
		const int        charH    = g_ScoreboardFontTiers[i].charHeight;
		const bool       lastTier = ( i == SCOREBOARD_FONT_TIER_COUNT - 1 );

		const int fixedHeight = CalcFixedRowsHeight( charH );
		const int available   = boxHeight - fixedHeight;

		const int idealRowHeight = available / numFlexRows;

		const int maxRowHeight = Q_min( SCOREBOARD_ROW_HEIGHT_MAX, charH + SCOREBOARD_ROW_PADDING_MAX );
		const int rowHeight    = bound( charH, idealRowHeight, maxRowHeight );

		const bool fits = ( fixedHeight + rowHeight * numFlexRows ) <= boxHeight;

		if( fits || lastTier )
		{
			const int padding     = bound( 0, rowHeight - charH, SCOREBOARD_ROW_PADDING_MAX );
			const int newPaddingY = padding / 2;
			const bool needsScrollbar = !fits;
			const bool rowHeightChanged = ( rowHeight != m_iCurrentRowHeight );

			const bool changed = ( font != m_eCurrentFont )
				|| ( newPaddingY != model.rowPaddingY )
				|| ( needsScrollbar != table.bShowScrollBar )
				|| rowHeightChanged;

			if( font != m_eCurrentFont )
			{
				m_eCurrentFont = font;
				table.SetCharSize( font );
			}

			if( rowHeightChanged )
			{
				m_iCurrentRowHeight = rowHeight;
				table.SetupColumn( 0, "", rowHeight, true );
				table.SetupColumn( 1, "", rowHeight, true );
			}

			model.rowPaddingY = newPaddingY;
			table.bShowScrollBar = needsScrollbar;

			return changed;
		}
	}

	return false;
}

// ============================================================
// Data
// ============================================================

void CMenuScoreboard::Clear()
{
	snprintf( serverName_buf, sizeof( serverName_buf ), "%s", g_pClient->GetServerHostName());

	model.rows.RemoveAll();

	CTs_players.RemoveAll();
	Ts_players.RemoveAll();
	spectators_players.RemoveAll();
}

// ============================================================
// Row builders
// ============================================================

void CMenuScoreboard::AddSectionHeader( ETeamColor color, const char *displayName, int score, bool hasScore )
{
	scoreboard_row_t row;
	memset( &row, 0, sizeof( row ));

	row.kind = ROW_HEADER;
	row.color = color;
	snprintf( row.name, sizeof( row.name ), "%s", displayName );

	if( hasScore )
		snprintf( row.score, sizeof( row.score ), "%d", score );

	model.rows.AddToTail( row );
}

void CMenuScoreboard::AddSeparator( ETeamColor color )
{
	scoreboard_row_t row;
	memset( &row, 0, sizeof( row ));

	row.kind = ROW_SEPARATOR;
	row.color = color;

	model.rows.AddToTail( row );
}

void CMenuScoreboard::AddSpacer()
{
	scoreboard_row_t row;
	memset( &row, 0, sizeof( row ));

	row.kind = ROW_SPACER;

	model.rows.AddToTail( row );
}

void CMenuScoreboard::AddPlayerRow( const player_t &player, ETeamColor color )
{
	scoreboard_row_t row;
	memset( &row, 0, sizeof( row ));

	row.kind = ROW_PLAYER;
	row.color = color;
	row.thisplayer = player.thisplayer != 0;

	UpdatePlayerSteamPresentation( row, player );

	snprintf( row.name, sizeof( row.name ), "%s", player.name );
	if( player.attrib )
		snprintf( row.attrib, sizeof( row.attrib ), "%s", player.attrib );

	if( player.sb_health > 0 && ( !player.attrib || strcmp( player.attrib, "Dead" ) != 0 ))
		snprintf( row.health, sizeof( row.health ), "%d", player.sb_health );

	if( player.sb_account > 0 )
		snprintf( row.money, sizeof( row.money ), "%d$", player.sb_account );

	snprintf( row.score, sizeof( row.score ), "%d", player.kills );
	snprintf( row.deaths, sizeof( row.deaths ), "%d", player.deaths );

	if( player.ping > 0 || player.thisplayer != 0 )
		snprintf( row.ping, sizeof( row.ping ), "%d", player.ping );
	else
		snprintf( row.ping, sizeof( row.ping ), "%s", "BOT" );

	model.rows.AddToTail( row );
}

void CMenuScoreboard::AddSpectatorRow( const player_t &player )
{
	scoreboard_row_t row;
	memset( &row, 0, sizeof( row ));

	row.kind = ROW_PLAYER;
	row.color = TEAM_COLOR_SPECTATOR;
	row.thisplayer = player.thisplayer != 0;
	UpdatePlayerSteamPresentation( row, player );

	snprintf( row.name, sizeof( row.name ), "%s", player.name );

	model.rows.AddToTail( row );
}

// ============================================================
// Section (header + separator + spacer + player rows), unified
// so team and spectator blocks are built identically.
// ============================================================

void CMenuScoreboard::AddTeamSection( ETeamColor color, const char *displayName, int score, const CUtlVector<player_t> &players )
{
	if( !players.Count())
		return;

	char title[80];
	FormatSectionTitle( title, sizeof( title ), displayName, players.Count());

	AddSpacer();
	AddSectionHeader( color, title, score, true );
	AddSpacer();
	AddSeparator( color );
	AddSpacer();

	for( int i = 0; i < players.Count(); i++ )
		AddPlayerRow( players[i], color );
}

// ============================================================
// Rounded background
// ============================================================

void CMenuScoreboard::DrawBackground()
{
	const uint bgColor = PackRGBA( 0, 0, 0, m_iBackgroundAlpha );

	UI_DrawPic( m_scPos, roundCornerSize, bgColor, "gfx/vgui/round_corner_nw.tga", QM_DRAWTRANS );

	UI_DrawPic( m_scPos
		    + Size( m_scSize.w
			    - roundCornerSize.w,
			    0 ),
		    roundCornerSize, bgColor, "gfx/vgui/round_corner_ne.tga", QM_DRAWTRANS );

	UI_DrawPic( m_scPos
		    + Size( 0, m_scSize.h
			    - roundCornerSize.h ),
		    roundCornerSize, bgColor, "gfx/vgui/round_corner_sw.tga", QM_DRAWTRANS );

	UI_DrawPic( m_scPos
		    + ( m_scSize
			- roundCornerSize ),
		    roundCornerSize, bgColor, "gfx/vgui/round_corner_se.tga", QM_DRAWTRANS );

	UI_FillRect( m_scPos
		     + Size( roundCornerSize.w, 0 ),
		     Size( m_scSize.w
			   - roundCornerSize.w * 2,
			   roundCornerSize.h ),
		     bgColor );

	UI_FillRect( m_scPos
		     + Size( 0, roundCornerSize.h ),
		     Size( m_scSize.w, m_scSize.h
			   - roundCornerSize.h * 2 ),
		     bgColor );

	UI_FillRect( m_scPos
		     + Size( roundCornerSize.w, m_scSize.h
			     - roundCornerSize.h ),
		     Size( m_scSize.w
			   - roundCornerSize.w * 2,
			   roundCornerSize.h ),
		     bgColor );
}

// ============================================================
// Draw
// ============================================================

void CMenuScoreboard::Draw()
{
	Clear();

	int Ts_score = 0;
	int CTs_score = 0;

	// ========================================================
	// Collect players
	// ========================================================

	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		if( i < 3 )
		{
			team_info_t *team;

			if( g_pClient->GetTeamInfo( i, &team ))
			{
				if( !strcmp( team->name, "TERRORIST" ))
					Ts_score = team->frags;
				else if( !strcmp( team->name, "CT" ))
					CTs_score = team->frags;
			}
		}

		extra_player_info_t *extra;
		hud_player_info_t   *pplayer;
		bool isBot;

		if( g_pClient->GetPlayerExtraInfo( i, &pplayer, &extra, &isBot ))
		{
			player_t player;

			memset( &player, 0, sizeof( player ));

			player.kills = extra->frags;
			player.name = pplayer->name;
			player.clientIndex = i;

			if( isBot )
				player.ping = -1;
			else
				player.ping = pplayer->ping;

			player.sb_health = extra->sb_health;
			player.sb_account = extra->sb_account;
			player.deaths = extra->deaths;
			player.kills = extra->frags;
			player.thisplayer = pplayer->thisplayer;
			player.m_nSteamID = pplayer->m_nSteamID;

			if( extra->dead )
				player.attrib = "Dead";
			else if( extra->has_c4 )
				player.attrib = "Bomb";
			else if( extra->vip )
				player.attrib = "VIP";
			else if( extra->has_defuse_kit )
				player.attrib = "D. Kit";
			else
				player.attrib = NULL;

			if( !strcmp( extra->teamname, "TERRORIST" ))
				Ts_players.AddToTail( player );
			else if( !strcmp( extra->teamname, "CT" ))
				CTs_players.AddToTail( player );
			else
				spectators_players.AddToTail( player );
		}
	}

	// ========================================================
	// Sort
	// ========================================================

	if( Ts_players.Count())
		qsort( Ts_players.Base(), Ts_players.Count(), sizeof( player_t ), PlayerCompar );

	if( CTs_players.Count())
		qsort( CTs_players.Base(), CTs_players.Count(), sizeof( player_t ), PlayerCompar );

	// ========================================================
	// Build rows
	// ========================================================

	AddSeparator( TEAM_COLOR_NONE );
	AddTeamSection( TEAM_COLOR_T, "Terrorists", Ts_score, Ts_players );
	AddTeamSection( TEAM_COLOR_CT, "Counter-Terrorists", CTs_score, CTs_players );

	if( spectators_players.Count())
	{
		char title[80];
		FormatSectionTitle( title, sizeof( title ), L( "Cstrike_TitlesTXT_Spectators" ), spectators_players.Count());

		AddSpacer();
		AddSectionHeader( TEAM_COLOR_NONE, title, 0, false );
		AddSpacer();
		AddSeparator( TEAM_COLOR_NONE );
		AddSpacer();

		for( int i = 0; i < spectators_players.Count(); i++ )
			AddSpectatorRow( spectators_players[i] );
	}

	// ========================================================
	// Adaptive layout
	// ========================================================

	const int numFlexRows = Ts_players.Count() + CTs_players.Count() + spectators_players.Count();
	const bool layoutChanged = UpdateRowLayout( numFlexRows );

	model.Update();

	const bool rowsChanged = ( model.rows.Count() != m_iLastRowCount );
	if( rowsChanged )
		m_iLastRowCount = model.rows.Count();

	if( rowsChanged || layoutChanged )
		table.VidInit();

	DrawBackground();
	BaseClass::Draw();
}

// ============================================================
// Init
// ============================================================

void CMenuScoreboard::Init()
{
	if( WasInit())
		return;

	BaseClass::Init();

	m_iLastRowCount = -1;
	roundCornerSize = Size( 16, 16 );
	m_iBackgroundAlpha = 125;
	m_eCurrentFont = g_ScoreboardFontTiers[0].font;
	m_iCurrentRowHeight = SCOREBOARD_ROW_HEIGHT_MAX;

	Clear();

	table.iFlags |= QMF_INACTIVE;
	table.bShowScrollBar = false;
	table.bDrawStroke = false;
	table.iBackgroundColor = PackRGBA( 0, 0, 0, 0 );
	table.iHeaderColor = g_ColorYellow;

	// ========================================================
	// Columns
	// ========================================================

	table.SetupColumn( 0, "", SCOREBOARD_FRIEND_WIDTH, true );
	table.SetupColumn( 1, "", SCOREBOARD_AVATAR_WIDTH, true );
	table.SetupColumn( 2, serverName_buf, 0.50f );
	table.SetupColumn( 3, "", 0.10f );
	table.SetupColumn( 4, "HP", 0.05f );
	table.SetupColumn( 5, "MONEY", 0.10f );
	table.SetupColumn( 6, L( "Cstrike_TitlesTXT_SCORE" ), 0.10f );
	table.SetupColumn( 7, L( "Cstrike_TitlesTXT_DEATHS" ), 0.10f );
	table.SetupColumn( 8, L( "Cstrike_TitlesTXT_LATENCY" ), 0.10f );
	table.SetModel( &model );
	table.SetCharSize( m_eCurrentFont );
	table.bAllowSorting = false;
	table.DisableSorting();

	AddItem( table );
}

// ============================================================
// Geometry
// ============================================================

void CMenuScoreboard::VidInit()
{
	size.w = SCOREBOARD_WIDTH;
	size.h = SCOREBOARD_HEIGHT;
	pos.x = (( ScreenWidth - uiStatic.scaleX * 1024 ) / 2 ) / uiStatic.scaleX + ( 1024 - SCOREBOARD_WIDTH ) / 2;
	pos.y = ( 768 - SCOREBOARD_HEIGHT ) / 2;

	BaseClass::VidInit();

	roundCornerSize = Size( 16, 16 ).Scale();
	table.pos = Point( SCOREBOARD_TABLE_PADDING_X, SCOREBOARD_TABLE_PADDING_Y );
	table.size = Size( size.w - SCOREBOARD_TABLE_PADDING_X * 2, size.h - SCOREBOARD_TABLE_PADDING_Y * 2 );

	table.VidInit();
}

// ============================================================
// Public API
// ============================================================

void UI_VidInitScoreboard()
{
	if( !scoreboard )
		return;

	scoreboard->Init();
	scoreboard->VidInit();
}

void UI_DrawScoreboard()
{
	if( !scoreboard )
		return;

	if( !scoreboard->WasInit() )
	{
		scoreboard->Init();
		scoreboard->VidInit();
	}

	scoreboard->Draw();
}