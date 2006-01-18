/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include "g_local.h"



/*
=================
Cmd_Players_f
=================
*/
/*void Cmd_Players_f (edict_t *ent)
{
	int		i;
	int		count;
	char	small[64];
	char	large[1280];
	int		index[256];

	count = 0;
	for (i = 0 ; i < maxplayers->value ; i++)
		if (game.players[i].pers.connected)
		{
			index[count] = i;
			count++;
		}

	// sort by frags
	qsort (index, count, sizeof(index[0]), PlayerSort);

	// print information
	large[0] = 0;

	for (i = 0 ; i < count ; i++)
	{
		Com_sprintf (small, sizeof(small), "%3i %s\n",
			game.players[index[i]].ps.stats[STAT_FRAGS],
			game.players[index[i]].pers.netname);
		if (strlen (small) + strlen(large) > sizeof(large) - 100 )
		{	// can't print all of them in one packet
			strcat (large, "...\n");
			break;
		}
		strcat (large, small);
	}

	gi.cprintf (ent, PRINT_HIGH, "%s\n%i players\n", large, count);
}*/


/*
==================
Cmd_Say_f
==================
*/
void Cmd_Say_f (player_t *player, qboolean arg0, qboolean team)
{
	int		j;
	char	*p;
	char	text[2048];

	if (gi.argc () < 2 && !arg0)
		return;

	if ( !team )
		Com_sprintf (text, sizeof(text), "%s: ", player->pers.netname);
	else
		Com_sprintf (text, sizeof(text), "%s (team): ", player->pers.netname);

	if (arg0)
	{
		strcat (text, gi.argv(0));
		strcat (text, " ");
		strcat (text, gi.args());
	}
	else
	{
		p = gi.args();

		if (*p == '"')
		{
			p++;
			p[strlen(p)-1] = 0;
		}
		strcat(text, p);
	}

	// don't let text be too long for malicious reasons
	if (strlen(text) > 150)
		text[150] = 0;

	strcat(text, "\n");

	if (dedicated->value)
		gi.cprintf(NULL, PRINT_CHAT, "%s", text);

	for (j = 0; j < game.maxplayers; j++)
	{
		if (!game.players[j].inuse)
			continue;
		if (team && game.players[j].pers.team != player->pers.team)
			continue;
		gi.cprintf(&game.players[j], PRINT_CHAT, "%s", text);
	}
}

#if 0
void Cmd_PlayerList_f(edict_t *ent)
{
	int i;
	char st[80];
	char text[1400];
	edict_t *e2;

	// connect time, ping, score, name
	*text = 0;
	for (i = 0, e2 = g_edicts + 1; i < maxplayers->value; i++, e2++) {
		if (!e2->inuse)
			continue;

		Com_sprintf(st, sizeof(st), "%02d:%02d %4d %3d %s%s\n",
			(level.framenum - e2->player->resp.enterframe) / 600,
			((level.framenum - e2->player->resp.enterframe) % 600)/10,
			e2->player->ping,
			e2->player->resp.score,
			e2->player->pers.netname,
			e2->player->resp.spectator ? " (spectator)" : "");
		if (strlen(text) + strlen(st) > sizeof(text) - 50) {
			sprintf(text+strlen(text), "And more...\n");
			gi.cprintf(ent, PRINT_HIGH, "%s", text);
			return;
		}
		strcat(text, st);
	}
	gi.cprintf(ent, PRINT_HIGH, "%s", text);
}
#endif


/*
=================
G_ClientCommand
=================
*/
void G_ClientCommand (player_t *player)
{
	char	*cmd;

	if (!player->inuse)
		return;		// not fully in game yet

	cmd = gi.argv(0);

//	if (Q_stricmp (cmd, "players") == 0)
//	{
//		Cmd_Players_f (ent);
//		return;
//	}
//	if (Q_stricmp(cmd, "playerlist") == 0)
//	{
//		Cmd_PlayerList_f(ent);
//		return;
//	}
	if (Q_stricmp (cmd, "say") == 0)
	{
		Cmd_Say_f (player, false, false);
		return;
	}
	if (Q_stricmp (cmd, "say_team") == 0)
	{
		Cmd_Say_f (player, false, true);
		return;
	}

	// anything that doesn't match a command will be a chat
	Cmd_Say_f (player, true, false);
}
