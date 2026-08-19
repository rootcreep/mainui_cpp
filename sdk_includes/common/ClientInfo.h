#ifndef CLIENTINFO_H
#define CLIENTINFO_H

#include "const.h"
#include <stdint.h>

#define CLIENTINFO_MAX_TEAM_NAME 16
#define CLIENTINFO_MAX_LOCATION_NAME 32

typedef struct hud_player_info_s
{
    char *name;
    short ping;
    byte thisplayer;

    byte spectator;
    byte packetloss;

    char *model;
    short topcolor;
    short bottomcolor;

    uint64_t m_nSteamID;
} hud_player_info_t;

struct extra_player_info_t
{
    short frags;
    short deaths;
    short team_id;
    qboolean has_c4;
    qboolean vip;

    // Same memory layout as Vector: 3 floats.
    float origin[3];

    int radarflashes;
    float radarflashtime;
    float radarflashtimedelta;
    bool nextflash;

    short playerclass;
    short teamnumber;
    char teamname[CLIENTINFO_MAX_TEAM_NAME];
    bool dead;
    float showhealth;
    int health;
    bool talking;
    char location[CLIENTINFO_MAX_LOCATION_NAME];
    int sb_health;
    int sb_account;
    qboolean has_defuse_kit;
};

struct team_info_t
{
    char name[CLIENTINFO_MAX_TEAM_NAME];
    short frags;
    short deaths;
    short ownteam;
    short players;
    int already_drawn;
    int scores_overriden;
    int sumping;
    int teamnumber;
};

struct hostage_info_t
{
    float origin[3];
    float radarflashtimedelta;
    float radarflashtime;
    bool dead;
    bool nextflash;
    int radarflashes;
};

typedef enum
{
	SBRK_PLAYER_UNREQUESTED,
	SBRK_PLAYER_PENDING,
	SBRK_PLAYER_READY,
	SBRK_PLAYER_UNAVAILABLE,
} sbrk_player_status_t;

typedef enum
{
	SBRK_PLAYER_RELATIONSHIP_NONE                  = 0,
	SBRK_PLAYER_RELATIONSHIP_FRIEND                = 1,
	SBRK_PLAYER_RELATIONSHIP_BLOCKED               = 2,
	SBRK_PLAYER_RELATIONSHIP_FRIENDSHIP_REQUESTED  = 3,
	SBRK_PLAYER_RELATIONSHIP_REQUESTING_FRIENDSHIP = 4,
} sbrk_player_relationship_t;

typedef struct
{
	uint64_t steamid;

	char name[128];

	byte relationship; // one of sbrk_player_relationship_t
	byte persona_state;
	uint32_t game_app_id;
	byte avatar_png[4096];
	uint32_t avatar_png_size;
	qboolean avatar_dirty;

	sbrk_player_status_t status;
	double request_time;
} sbrk_player_info_t;

#endif // CLIENTINFO_H