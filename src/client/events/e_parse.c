/**
 * @file cl_parse_events.c
 * @section eventformat Event format identifiers
 * id	| type		| length (bytes)
 *======================================
 * c	| char		| 1
 * b	| byte		| 1
 * s	| short		| 2
 * l	| long		| 4
 * p	| pos		| 6 (map boundaries - (-MAX_WORLD_WIDTH) - (MAX_WORLD_WIDTH))
 * g	| gpos		| 3
 * d	| dir		| 1
 * a	| angle		| 1
 * &	| string	| x
 * !	| do not read the next id | 1
 * *	| pascal string type - SIZE+DATA, SIZE can be read from va_arg
 *		| 2 + sizeof(DATA)
 * @endsection
 */

/*
Copyright (C) 1997-2008 UFO:AI Team

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

#include "../client.h"
#include "e_parse.h"
#include "e_time.h"
#include "e_main.h"

#include "../cl_le.h"
#include "../cl_screen.h"
#include "../cl_game.h"
#include "../cl_team.h"
#include "../cl_particle.h"
#include "../cl_actor.h"
#include "../cl_view.h"
#include "../cl_hud.h"
#include "../menu/m_main.h"
#include "../renderer/r_mesh_anim.h"

cvar_t *cl_log_battlescape_events;

typedef struct evTimes_s {
	int eType;					/**< event type to handle */
	struct dbuffer *msg;		/**< the parsed network channel data */
} evTimes_t;

/**********************************************************
 * General battlescape event functions
 **********************************************************/

/**
 * @sa CL_ExecuteBattlescapeEvent
 */
static void CL_LogEvent (const eventRegister_t *eventData)
{
	qFILE f;

	if (!cl_log_battlescape_events->integer)
		return;

	FS_OpenFile("events.log", &f, FILE_APPEND);
	if (!f.f)
		return;
	else {
		struct tm *t;
		char tbuf[32];
		time_t aclock;

		time(&aclock);
		t = localtime(&aclock);

		Com_sprintf(tbuf, sizeof(tbuf), "%4i/%02i/%02i %02i:%02i:%02i", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);

		FS_Printf(&f, "%s - %s: %10i %s\n", tbuf, cl.configstrings[CS_MAPTITLE], cl.time, eventData->name);
		FS_CloseFile(&f);
	}
}

/**
 * @brief Checks if a given battlescape event is ok to run now.
 *  Uses the check_func pointer in the event struct.
 * @param now The current time.
 * @param data The event to check.
 * @return true if it's ok to run or there is no check function, false otherwise.
 */
static qboolean CL_CheckBattlescapeEvent (int now, void *data)
{
	evTimes_t *event = (evTimes_t *)data;
	const eventRegister_t *eventData = CL_GetEvent(event->eType);

	if (eventData->eventCheck == NULL)
		return qtrue;

	return eventData->eventCheck(eventData, event->msg);
}

/**
 * @sa CL_ScheduleEvent
 */
static void CL_ExecuteBattlescapeEvent (int now, void *data)
{
	evTimes_t *event = (evTimes_t *)data;
	const eventRegister_t *eventData = CL_GetEvent(event->eType);

	if (event->eType <= EV_START_DONE || cls.state == ca_active) {
		Com_DPrintf(DEBUG_EVENTSYS, "event(dispatching at %d): %s %p\n", now, eventData->name, event);

		CL_LogEvent(eventData);

		if (!eventData->eventCallback)
			Com_Error(ERR_DROP, "Event %i doesn't have a callback", event->eType);

		eventData->eventCallback(eventData, event->msg);
	} else {
		Com_DPrintf(DEBUG_EVENTSYS, "event(not executed): %s %p\n", eventData->name, event);
	}

	free_dbuffer(event->msg);
	Mem_Free(event);
}

/**
 * @brief Called in case a svc_event was send via the network buffer
 * @sa CL_ParseServerMessage
 * @param[in] msg The client stream message buffer to read from
 */
void CL_ParseEvent (struct dbuffer *msg)
{
	qboolean now;
	const eventRegister_t *eventData;
	int eType = NET_ReadByte(msg);
	if (eType == 0)
		return;

	/* check instantly flag */
	if (eType & EVENT_INSTANTLY) {
		now = qtrue;
		eType &= ~EVENT_INSTANTLY;
	} else
		now = qfalse;

	/* check if eType is valid */
	if (eType < 0 || eType >= EV_NUM_EVENTS)
		Com_Error(ERR_DROP, "CL_ParseEvent: invalid event %i", eType);

	eventData = CL_GetEvent(eType);
	if (!eventData->eventCallback)
		Com_Error(ERR_DROP, "CL_ParseEvent: no handling function for event %i", eType);

	if (now) {
		/* log and call function */
		CL_LogEvent(eventData);
		Com_DPrintf(DEBUG_EVENTSYS, "event(now): %s\n", eventData->name);
		eventData->eventCallback(eventData, msg);
	} else {
		evTimes_t *cur = Mem_PoolAlloc(sizeof(*cur), cl_genericPool, 0);
		static int lastFrame = 0;
		const int delta = cl.time - lastFrame;
		int when;

		/* copy the buffer as first action, the event time functions can modify the buffer already */
		cur->msg = dbuffer_dup(msg);
		cur->eType = eType;

		/* timestamp (msec) that is used to determine when the event should be executed */
		when = CL_GetEventTime(cur->eType, msg, delta);
		Schedule_Event(when, &CL_ExecuteBattlescapeEvent, &CL_CheckBattlescapeEvent, cur);

		lastFrame = cl.time;

		Com_DPrintf(DEBUG_EVENTSYS, "event(at %d): %s %p\n", when, eventData->name, cur);
	}
}
