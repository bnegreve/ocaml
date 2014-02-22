/***********************************************************************/
/*                                                                     */
/*                                OCaml                                */
/*                                                                     */
/*            Xavier Leroy, projet Cristal, INRIA Rocquencourt         */
/*                                                                     */
/*  Copyright 1996 Institut National de Recherche en Informatique et   */
/*  en Automatique.  All rights reserved.  This file is distributed    */
/*  under the terms of the GNU Library General Public License, with    */
/*  the special exception on linking described in file ../../LICENSE.  */
/*                                                                     */
/***********************************************************************/

#include <signal.h>
#include "libgraph.h"
#include <alloc.h>
#include <signals.h>
#include <sys/types.h>
#include <sys/time.h>
#ifdef HAS_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <string.h>
#include <unistd.h>
#define NO_TIMEOUT -1

struct event_data {
  short kind;
  short mouse_x, mouse_y;
  unsigned char button;
  unsigned char key;
  int timeout; 
};

static struct event_data caml_gr_queue[SIZE_QUEUE];
static unsigned int caml_gr_head = 0;       /* position of next read */
static unsigned int caml_gr_tail = 0;       /* position of next write */

#define QueueIsEmpty (caml_gr_tail == caml_gr_head)

static void caml_gr_enqueue_event(int kind, int mouse_x, int mouse_y,
                             int button, int key)
{
  struct event_data * ev;

  ev = &(caml_gr_queue[caml_gr_tail]);
  ev->kind = kind;
  ev->mouse_x = mouse_x;
  ev->mouse_y = mouse_y;
  ev->button = (button != 0);
  ev->key = key;
  ev->timeout = NO_TIMEOUT; 
  caml_gr_tail = (caml_gr_tail + 1) % SIZE_QUEUE;
  /* If queue was full, it now appears empty; drop oldest entry from queue. */
  if (QueueIsEmpty) caml_gr_head = (caml_gr_head + 1) % SIZE_QUEUE;
}

#define BUTTON_STATE(state) \
  ((state) & (Button1Mask|Button2Mask|Button3Mask|Button4Mask|Button5Mask))

void caml_gr_handle_event(XEvent * event)
{
  switch (event->type) {

  case Expose:
    XCopyArea(caml_gr_display, caml_gr_bstore.win, caml_gr_window.win,
              caml_gr_window.gc,
              event->xexpose.x,
              event->xexpose.y + caml_gr_bstore.h - caml_gr_window.h,
              event->xexpose.width, event->xexpose.height,
              event->xexpose.x, event->xexpose.y);
    XFlush(caml_gr_display);
    break;

  case ConfigureNotify:
    caml_gr_window.w = event->xconfigure.width;
    caml_gr_window.h = event->xconfigure.height;
    if (caml_gr_window.w > caml_gr_bstore.w
        || caml_gr_window.h > caml_gr_bstore.h) {

      /* Allocate a new backing store large enough to accomodate
         both the old backing store and the current window. */
      struct canvas newbstore;
      newbstore.w = max(caml_gr_window.w, caml_gr_bstore.w);
      newbstore.h = max(caml_gr_window.h, caml_gr_bstore.h);
      newbstore.win =
        XCreatePixmap(caml_gr_display, caml_gr_window.win, newbstore.w,
                      newbstore.h,
                      XDefaultDepth(caml_gr_display, caml_gr_screen));
      newbstore.gc = XCreateGC(caml_gr_display, newbstore.win, 0, NULL);
      XSetBackground(caml_gr_display, newbstore.gc, caml_gr_white);
      XSetForeground(caml_gr_display, newbstore.gc, caml_gr_white);
      XFillRectangle(caml_gr_display, newbstore.win, newbstore.gc,
                     0, 0, newbstore.w, newbstore.h);
      XSetForeground(caml_gr_display, newbstore.gc, caml_gr_color);
      if (caml_gr_font != NULL)
        XSetFont(caml_gr_display, newbstore.gc, caml_gr_font->fid);

      /* Copy the old backing store into the new one */
      XCopyArea(caml_gr_display, caml_gr_bstore.win, newbstore.win,
                newbstore.gc,
                0, 0, caml_gr_bstore.w, caml_gr_bstore.h, 0,
                newbstore.h - caml_gr_bstore.h);

      /* Free the old backing store */
      XFreeGC(caml_gr_display, caml_gr_bstore.gc);
      XFreePixmap(caml_gr_display, caml_gr_bstore.win);

      /* Use the new backing store */
      caml_gr_bstore = newbstore;
      XFlush(caml_gr_display);
    }
    break;

  case MappingNotify:
    XRefreshKeyboardMapping(&(event->xmapping));
    break;

  case KeyPress:
    { KeySym thekey;
      char keytxt[256];
      int nchars;
      char * p;
      nchars = XLookupString(&(event->xkey), keytxt, sizeof(keytxt),
                             &thekey, 0);
      for (p = keytxt; nchars > 0; p++, nchars--)
        caml_gr_enqueue_event(event->type, event->xkey.x, event->xkey.y,
                         BUTTON_STATE(event->xkey.state), *p);
      break;
    }

  case ButtonPress:
  case ButtonRelease:
    caml_gr_enqueue_event(event->type, event->xbutton.x, event->xbutton.y,
                     event->type == ButtonPress, 0);
    break;

  case MotionNotify:
    caml_gr_enqueue_event(event->type, event->xmotion.x, event->xmotion.y,
                     BUTTON_STATE(event->xmotion.state), 0);
    break;
  }
}

static value caml_gr_wait_allocate_result(int mouse_x, int mouse_y, int button,
				      int keypressed, int key, int timeleft)
{
  value res = alloc_small(6, 0);
  Field(res, 0) = Val_int(mouse_x);
  Field(res, 1) = Val_int(mouse_y == -1 ? -1 : Wcvt(mouse_y));
  Field(res, 2) = Val_bool(button);
  Field(res, 3) = Val_bool(keypressed);
  Field(res, 4) = Val_int(key & 0xFF);
  Field(res, 5) = Val_bool((timeleft == 0 ? 1 : 0));
  return res;
}

static value caml_gr_wait_event_poll(void)
{
  int mouse_x, mouse_y, button, key, keypressed;
  Window rootwin, childwin;
  int root_x, root_y, win_x, win_y;
  unsigned int modifiers;
  unsigned int i;

  caml_process_pending_signals ();
  if (XQueryPointer(caml_gr_display, caml_gr_window.win,
                    &rootwin, &childwin,
                    &root_x, &root_y, &win_x, &win_y,
                    &modifiers)) {
    mouse_x = win_x;
    mouse_y = win_y;
  } else {
    mouse_x = -1;
    mouse_y = -1;
  }
  button = modifiers & (Button1Mask | Button2Mask | Button3Mask
                          | Button4Mask | Button5Mask);
  /* Look inside event queue for pending KeyPress events */
  key = 0;
  keypressed = False;
  for (i = caml_gr_head; i != caml_gr_tail; i = (i + 1) % SIZE_QUEUE) {
    if (caml_gr_queue[i].kind == KeyPress) {
      keypressed = True;
      key = caml_gr_queue[i].key;
      break;
    }
  }
  return
    caml_gr_wait_allocate_result(mouse_x, mouse_y, button, keypressed, key, NO_TIMEOUT);
}

static value caml_gr_wait_event_in_queue(long mask)
{
  struct event_data * ev;
  /* Pop events in queue until one matches mask. */
  while (caml_gr_head != caml_gr_tail) {
    ev = &(caml_gr_queue[caml_gr_head]);
    caml_gr_head = (caml_gr_head + 1) % SIZE_QUEUE;
    if ((ev->kind == KeyPress && (mask & KeyPressMask))
        || (ev->kind == ButtonPress && (mask & ButtonPressMask))
        || (ev->kind == ButtonRelease && (mask & ButtonReleaseMask))
        || (ev->kind == MotionNotify && (mask & PointerMotionMask)))
      return caml_gr_wait_allocate_result(ev->mouse_x, ev->mouse_y,
					  ev->button, ev->kind == KeyPress,
					  ev->key, NO_TIMEOUT);
  }
  return Val_false;
}

static struct timeval msec_to_timeval(int msec)
{
  struct timeval tv; 
  tv.tv_sec = msec / 1000;
  tv.tv_usec = (msec % 1000) * 1000; 
  return tv; 
}

static int timeval_to_msec(struct timeval tv)
{
  return tv.tv_sec * 1000 + tv.tv_usec; 
}


static value caml_gr_wait_event_blocking(long mask, int timeout)
{
  XEvent event;
  fd_set readfds;
  value res;
  struct timeval timeout_tv;
  int timeleft = timeout;
  
  /* First see if we have a matching event in the queue */
  res = caml_gr_wait_event_in_queue(mask);
  if (res != Val_false) return res;

  /* Increase the selected events if required */
  if ((mask & ~caml_gr_selected_events) != 0) {
    caml_gr_selected_events |= mask;
    XSelectInput(caml_gr_display, caml_gr_window.win, caml_gr_selected_events);
  }

  timeout_tv = msec_to_timeval(timeleft); 
  
  /* Replenish our event queue from that of X11 */
  caml_gr_ignore_sigio = True;
  while (1) {   
    timeleft = timeval_to_msec(timeout_tv); //FIXME: Linux only: see man select. 
    if (XCheckMaskEvent(caml_gr_display, -1 /*all events*/, &event)){
      /* One X event available: add it to our queue */
      caml_gr_handle_event(&event);
      /* See if we now have a matching event */
      res = caml_gr_wait_event_in_queue(mask);
      if (res != Val_false) break;
    } else if (timeleft == 0){ //FIXME: should enter the loop if timeleft == NOTIMEOUT
	/* No event available, no time left */
	res = caml_gr_wait_allocate_result(0,0,0,0,0,timeleft); 
	break;
    } else {
      /* No event available: block on input socket until one is */
      FD_ZERO(&readfds);
      FD_SET(ConnectionNumber(caml_gr_display), &readfds);
      enter_blocking_section();
      if(timeout == NO_TIMEOUT)
	select(FD_SETSIZE, &readfds, NULL, NULL, NULL);
      else 
	select(FD_SETSIZE, &readfds, NULL, NULL, &timeout_tv);	
      leave_blocking_section();
      caml_gr_check_open(); /* in case another thread closed the display */
    }
  }
  caml_gr_ignore_sigio = False;

  /* Return result */
  return res;
}

value caml_gr_wait_event(value eventlist, int timeout_) /* ML */
{
  int timeout = Int_val(timeout_); 
  int mask;
  Bool poll;

  caml_gr_check_open();
  mask = 0;
  poll = False;
  while (eventlist != Val_int(0)) {
    switch (Int_val(Field(eventlist, 0))) {
    case 0:                     /* Button_down */
      mask |= ButtonPressMask | OwnerGrabButtonMask; break;
    case 1:                     /* Button_up */
      mask |= ButtonReleaseMask | OwnerGrabButtonMask; break;
    case 2:                     /* Key_pressed */
      mask |= KeyPressMask; break;
    case 3:                     /* Mouse_motion */
      mask |= PointerMotionMask; break;
    case 4:                     /* Poll */
      poll = True; break;
    }
    eventlist = Field(eventlist, 1);
  }
  if (poll)
    return caml_gr_wait_event_poll();
  else
    return caml_gr_wait_event_blocking(mask, timeout);
}
