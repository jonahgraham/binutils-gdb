/* Serial interface for raw TCP connections transported on websockets with emscripten

 Copyright (C) 1992-2016 Free Software Foundation, Inc.

 This file is part of GDB.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "serial.h"
#include "ser-base.h"
#include "ser-tcp.h"
#include "gdbcmd.h"
#include "cli/cli-decode.h"
#include "cli/cli-setshow.h"
#include "filestuff.h"

#include <sys/types.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>  /* For FIONBIO.  */
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>  /* For FIONBIO.  */
#endif

#include "gdb_sys_time.h"

#ifdef USE_WIN32API
#include <winsock2.h>
#ifndef ETIMEDOUT
#define ETIMEDOUT WSAETIMEDOUT
#endif
#define close(fd) closesocket (fd)
#define ioctl ioctlsocket
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#endif

#include <signal.h>
#include "gdb_select.h"

#ifndef HAVE_SOCKLEN_T
typedef int socklen_t;
#endif

#include <emscripten/websocket.h>

#define PRINTF(...) //printf (__VA_ARGS__)

void
_initialize_ser_enscripten (void);

#define MAX_SOCKETS 10 // TODO make variable
static struct ws_state
{
  struct serial *scb;
  EMSCRIPTEN_WEBSOCKET_T ws;
  int opened;
  uint8_t *buffer;
  uint32_t buffer_alloc;
  uint32_t buffer_read;
  uint32_t buffer_write;
} sockets[MAX_SOCKETS];
static size_t nsockets = 0;

static EM_BOOL
onopen (int eventType, const EmscriptenWebSocketOpenEvent *websocketEvent,
	void *userData)
{
  struct ws_state *ws_state = (struct ws_state*) userData;
  PRINTF ("onopen\n");
  ws_state->opened = 1;
//
//  EMSCRIPTEN_RESULT result;
//  result = emscripten_websocket_send_utf8_text (websocketEvent->socket, "hoge");
//  if (result)
//    {
//      PRINTF ("Failed to emscripten_websocket_send_utf8_text(): %d\n", result);
//    }
  return EM_TRUE;
}

static EM_BOOL
onerror (int eventType, const EmscriptenWebSocketErrorEvent *websocketEvent,
	 void *userData)
{
  struct ws_state *ws_state = (struct ws_state*) userData;
  PRINTF ("onerror\n");

  return EM_TRUE;
}

static EM_BOOL
onclose (int eventType, const EmscriptenWebSocketCloseEvent *websocketEvent,
	 void *userData)
{
  struct ws_state *ws_state = (struct ws_state*) userData;
  PRINTF ("onclose\n");

  return EM_TRUE;
}

static EM_BOOL
onmessage (int eventType, const EmscriptenWebSocketMessageEvent *websocketEvent,
	   void *userData)
{
  struct ws_state *ws_state = (struct ws_state*) userData;
  PRINTF ("onmessage\n");
  if (websocketEvent->isText)
    {
      // For only ascii chars.
      PRINTF ("message: %s\n", websocketEvent->data);
    }

  if (ws_state->buffer_write + websocketEvent->numBytes
      > ws_state->buffer_alloc)
    {
      ws_state->buffer = xrealloc (
	  ws_state->buffer,
	  ws_state->buffer_write + websocketEvent->numBytes + 1000);
    }
  memcpy (&ws_state->buffer[ws_state->buffer_write], websocketEvent->data,
	  websocketEvent->numBytes);
  ws_state->buffer_write += websocketEvent->numBytes;

  return EM_TRUE;
}

static int
ws_open (struct serial *scb, const char *name)
{
  if (!emscripten_websocket_is_supported ())
    {
      PRINTF ("Failed to open: !emscripten_websocket_is_supported\n");
      return -1;
    }
  if (nsockets >= MAX_SOCKETS)
    {
      PRINTF (
	  "Out of handles for opening websockets. Rewrite sockets to be dynamic\n");
      return -1;
    }

  EmscriptenWebSocketCreateAttributes ws_attrs =
    { name, NULL, EM_TRUE };

  /* FIXME: this is a Bad Idea (tm)!  One should *never* invent file
   handles, since they might be already used by other files/devices.
   The Right Way to do this is to create a real handle by dup()'ing
   some existing one.  */
  scb->fd = 20 + nsockets;
  sockets[nsockets].scb = scb;
  sockets[nsockets].opened = 0;
  sockets[nsockets].buffer = xcalloc (1000, 1);
  sockets[nsockets].buffer_alloc = 1000;
  sockets[nsockets].buffer_read = 0;
  sockets[nsockets].buffer_write = 0;

  EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new (&ws_attrs);

  sockets[nsockets].ws = ws;

  emscripten_websocket_set_onopen_callback(ws, &sockets[nsockets], onopen);
  emscripten_websocket_set_onerror_callback(ws, &sockets[nsockets], onerror);
  emscripten_websocket_set_onclose_callback(ws, &sockets[nsockets], onclose);
  emscripten_websocket_set_onmessage_callback(ws, &sockets[nsockets],
					      onmessage);
  while (!sockets[nsockets].opened)
    {
      emscripten_sleep (100);
    }

  nsockets++;
  return 0;
}

static struct ws_state*
get_ws_state (struct serial *scb)
{
  if (scb->fd < 20 || scb->fd > 20 + MAX_SOCKETS)
    {
      PRINTF ("Unexpectedly fd value %d\n", scb->fd);
    }

  return &sockets[scb->fd - 20];
}

static void
ws_close (struct serial *scb)
{
  if (scb->fd == -1)
    return;

  struct ws_state *ws_state = get_ws_state (scb);
  // XXX: Close first?
  emscripten_websocket_delete (ws_state->ws);
  scb->fd = -1;
}

/* Read a character with user-specified timeout.  TIMEOUT is number of seconds
 to wait, or -1 to wait forever.  Use timeout of 0 to effect a poll.  Returns
 char if successful.  Returns -2 if timeout expired, EOF if line dropped
 dead, or -3 for any other error (see errno in that case).  */
static int
ws_readchar (struct serial *scb, int timeout)
{
  PRINTF ("ws_readchar with timeout of %d\n", timeout);
  struct ws_state *ws_state = get_ws_state (scb);
  int countdown = timeout * 10;
  while (ws_state->buffer_read == ws_state->buffer_write)
    {
      if (timeout == 0)
	{
	  // just polling, since no data return immediately
	  return -2;
	}
      else if (timeout < 0)
	{
	  // wait forever
	  emscripten_sleep (100);
	}
      else
	{
	  countdown--;
	  if (countdown < 0)
	    return -2;
	  emscripten_sleep (100);
	}
    }
  // TODO once we have read a bit, dispose of old read data so it doesn't accumulate forever!
  return ws_state->buffer[ws_state->buffer_read++];
}

static int
ws_write (struct serial *scb, const void *buf, size_t count)
{
  PRINTF ("ws_write with buf len %d\n", (int) count);
  struct ws_state *ws_state = get_ws_state (scb);
  emscripten_websocket_send_binary (ws_state->ws, buf, count);
  return 0;
}

static int
ws_flush_input (struct serial *scb)
{
  PRINTF ("ws_flush_input\n");
  struct ws_state *ws_state = get_ws_state (scb);
  ws_state->buffer_read = ws_state->buffer_write = 0;
  return 0;

}

static int
ws_flush_output (struct serial *scb)
{
  return 0;
}

static void
ws_raw (struct serial *scb)
{
  /* Always in raw mode.  */
}

static int
ws_sendbreak (struct serial *scb)
{
  PRINTF ("dos_sendbreak\n");
  return 0;
}

static const struct serial_ops ws_ops =
  { "ws", //
      ws_open, //
      ws_close, //
      NULL, //
      ws_readchar, //
      ws_write, //
      ws_flush_output, //
      ws_flush_input, //
      ws_sendbreak, //
      ws_raw, //
      ser_base_get_tty_state, // base does noop
      ser_base_copy_tty_state, // base does noop
      ser_base_set_tty_state, // base does noop
      ser_base_print_tty_state, // base does noop
      ser_base_noflush_set_tty_state, // base does noop
      ser_base_setbaudrate, // base does noop
      ser_base_setstopbits,  // base does noop
      ser_base_setparity,  // base does noop
      ser_base_drain_output,  // base does noop
      NULL, // async
      NULL, // read_prim
      NULL // write_prim
    };

void
_initialize_ser_enscripten (void)
{
  serial_add_interface (&ws_ops);
}
