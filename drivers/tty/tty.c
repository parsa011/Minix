/* This file contains the terminal driver, both for the IBM console and regular
 * ASCII terminals.  It handles only the device-independent part of a TTY, the
 * device dependent parts are in console.c, rs232.c, etc.  This file contains
 * two main entry points, tty_task() and tty_wakeup(), and several minor entry
 * points for use by the device-dependent code.
 *
 * The device-independent part accepts "keyboard" input from the device-
 * dependent part, performs input processing (special key interpretation),
 * and sends the input to a process reading from the TTY.  Output to a TTY
 * is sent to the device-dependent code for output processing and "screen"
 * display.  Input processing is done by the device by calling 'in_process'
 * on the input characters, output processing may be done by the device itself
 * or by calling 'out_process'.  The TTY takes care of input queuing, the
 * device does the output queuing.  If a device receives an external signal,
 * like an interrupt, then it causes tty_wakeup() to be run by the CLOCK task
 * to, you guessed it, wake up the TTY to check if input or output can
 * continue.
 *
 * The valid messages and their parameters are:
 *
 *   HARD_INT:       output has been completed or input has arrived
 *   SYS_SIG:      e.g., MINIX wants to shutdown; run code to cleanly stop
 *   DEV_READ:       a process wants to read from a terminal
 *   DEV_WRITE:      a process wants to write on a terminal
 *   DEV_IOCTL:      a process wants to change a terminal's parameters
 *   DEV_OPEN:       a tty line has been opened
 *   DEV_CLOSE:      a tty line has been closed
 *   DEV_SELECT:     start select notification request
 *   DEV_STATUS:     FS wants to know status for SELECT or REVIVE
 *   CANCEL:         terminate a previous incomplete system call immediately
 *
 *    m_type      TTY_LINE   PROC_NR    COUNT   TTY_SPEK  TTY_FLAGS  ADDRESS
 * ---------------------------------------------------------------------------
 * | HARD_INT    |         |         |         |         |         |         |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | SYS_SIG     | sig set |         |         |         |         |         |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | DEV_READ    |minor dev| proc nr |  count  |         O_NONBLOCK| buf ptr |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | DEV_WRITE   |minor dev| proc nr |  count  |         |         | buf ptr |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | DEV_IOCTL   |minor dev| proc nr |func code|erase etc|  flags  |         |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | DEV_OPEN    |minor dev| proc nr | O_NOCTTY|         |         |         |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | DEV_CLOSE   |minor dev| proc nr |         |         |         |         |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | DEV_STATUS  |         |         |         |         |         |         |
 * |-------------+---------+---------+---------+---------+---------+---------|
 * | CANCEL      |minor dev| proc nr |         |         |         |         |
 * ---------------------------------------------------------------------------
 *
 * Changes:
 *   Jan 20, 2004   moved TTY driver to user-space  (Jorrit N. Herder)
 *   Sep 20, 2004   local timer management/ sync alarms  (Jorrit N. Herder)
 *   Jul 13, 2004   support for function key observers  (Jorrit N. Herder)  
 */

#include "../drivers.h"
#include "../drivers.h"
#include <termios.h>
#include <sys/ioc_tty.h>
#include <signal.h>
#include <minix/callnr.h>
#include <minix/keymap.h>
#include "tty.h"

#include <sys/time.h>
#include <sys/select.h>

extern int irq_hook_id;

unsigned long kbd_irq_set = 0;
unsigned long rs_irq_set = 0;

/* Address of a tty structure. */
#define tty_addr(line)	(&tty_table[line])

/* Macros for magic tty types. */
#define isconsole(tp)	((tp) < tty_addr(NR_CONS))
#define ispty(tp)	((tp) >= tty_addr(NR_CONS+NR_RS_LINES))

/* Macros for magic tty structure pointers. */
#define FIRST_TTY	tty_addr(0)
#define END_TTY		tty_addr(sizeof(tty_table) / sizeof(tty_table[0]))

/* A device exists if at least its 'devread' function is defined. */
#define tty_active(tp)	((tp)->tty_devread != NULL)

/* RS232 lines or pseudo terminals can be completely configured out. */
#if NR_RS_LINES == 0
#define rs_init(tp)	((void) 0)
#endif
#if NR_PTYS == 0
#define pty_init(tp)	((void) 0)
#define do_pty(tp, mp)	((void) 0)
#endif

FORWARD _PROTOTYPE( void tty_timed_out, (timer_t *tp)			);
FORWARD _PROTOTYPE( void expire_timers, (void)				);
FORWARD _PROTOTYPE( void settimer, (tty_t *tty_ptr, int enable)		);
FORWARD _PROTOTYPE( void do_cancel, (tty_t *tp, message *m_ptr)		);
FORWARD _PROTOTYPE( void do_ioctl, (tty_t *tp, message *m_ptr)		);
FORWARD _PROTOTYPE( void do_open, (tty_t *tp, message *m_ptr)		);
FORWARD _PROTOTYPE( void do_close, (tty_t *tp, message *m_ptr)		);
FORWARD _PROTOTYPE( void do_read, (tty_t *tp, message *m_ptr)		);
FORWARD _PROTOTYPE( void do_write, (tty_t *tp, message *m_ptr)		);
FORWARD _PROTOTYPE( void do_select, (tty_t *tp, message *m_ptr)		);
FORWARD _PROTOTYPE( void do_status, (message *m_ptr)			);
FORWARD _PROTOTYPE( void in_transfer, (tty_t *tp)			);
FORWARD _PROTOTYPE( int tty_echo, (tty_t *tp, int ch)			);
FORWARD _PROTOTYPE( void rawecho, (tty_t *tp, int ch)			);
FORWARD _PROTOTYPE( int back_over, (tty_t *tp)				);
FORWARD _PROTOTYPE( void reprint, (tty_t *tp)				);
FORWARD _PROTOTYPE( void dev_ioctl, (tty_t *tp)				);
FORWARD _PROTOTYPE( void setattr, (tty_t *tp)				);
FORWARD _PROTOTYPE( void tty_icancel, (tty_t *tp)			);
FORWARD _PROTOTYPE( void tty_init, (void)				);

/* Default attributes. */
PRIVATE struct termios termios_defaults = {
  TINPUT_DEF, TOUTPUT_DEF, TCTRL_DEF, TLOCAL_DEF, TSPEED_DEF, TSPEED_DEF,
  {
	TEOF_DEF, TEOL_DEF, TERASE_DEF, TINTR_DEF, TKILL_DEF, TMIN_DEF,
	TQUIT_DEF, TTIME_DEF, TSUSP_DEF, TSTART_DEF, TSTOP_DEF,
	TREPRINT_DEF, TLNEXT_DEF, TDISCARD_DEF,
  },
};
PRIVATE struct winsize winsize_defaults;	/* = all zeroes */

/* Global variables for the TTY task (declared extern in tty.h). */
PUBLIC tty_t tty_table[NR_CONS+NR_RS_LINES+NR_PTYS];
PUBLIC int ccurrent;			/* currently active console */
PUBLIC timer_t *tty_timers;		/* queue of TTY timers */
PUBLIC clock_t tty_next_timeout;	/* time that the next alarm is due */
PUBLIC struct machine machine;		/* kernel environment variables */

/*===========================================================================*
 *				tty_task				     *
 *===========================================================================*/
PUBLIC void main(void)
{
/* Main routine of the terminal task. */

  message tty_mess;		/* buffer for all incoming messages */
  unsigned line;
  int s;
  char *types[] = {"task","driver","server", "user"};
  register struct proc *rp;
  register tty_t *tp;

  /* Initialize the TTY driver. */
  tty_init();

  /* Get kernel environment (protected_mode, pc_at and ega are needed). */ 
  if (OK != (s=sys_getmachine(&machine))) {
    panic("TTY","Couldn't obtain kernel environment.", s);
  }

  /* Final one-time keyboard initialization. */
  kb_init_once();

  printf("\n");

  while (TRUE) {

	/* Check for and handle any events on any of the ttys. */
	for (tp = FIRST_TTY; tp < END_TTY; tp++) {
		if (tp->tty_events) handle_events(tp);
	}

	/* Get a request message. */
	receive(ANY, &tty_mess);

	/* First handle all kernel notification types that the TTY supports. 
	 *  - An alarm went off, expire all timers and handle the events. 
	 *  - A hardware interrupt also is an invitation to check for events. 
	 *  - A new kernel message is available for printing.
	 *  - Reset the console on system shutdown. 
	 * Then see if this message is different from a normal device driver
	 * request and should be handled separately. These extra functions
	 * do not operate on a device, in constrast to the driver requests. 
	 */
	switch (tty_mess.m_type) { 
	case SYN_ALARM: 		/* fall through */
		expire_timers();	/* run watchdogs of expired timers */
		continue;		/* contine to check for events */
	case HARD_INT: {		/* hardware interrupt notification */
		if (tty_mess.NOTIFY_ARG & kbd_irq_set)
			kbd_interrupt(&tty_mess);/* fetch chars from keyboard */
#if NR_RS_LINES > 0
		if (tty_mess.NOTIFY_ARG & rs_irq_set)
			rs_interrupt(&tty_mess);/* serial I/O */
#endif
		expire_timers();	/* run watchdogs of expired timers */
		continue;		/* contine to check for events */
	}
	case SYS_SIG: {			/* system signal */
		sigset_t sigset = (sigset_t) tty_mess.NOTIFY_ARG;

		if (sigismember(&sigset, SIGKSTOP)) {
			cons_stop();		/* switch to primary console */
			if (irq_hook_id != -1) {
				sys_irqdisable(&irq_hook_id);
				sys_irqrmpolicy(KEYBOARD_IRQ, &irq_hook_id);
			}
		} 
		if (sigismember(&sigset, SIGTERM)) cons_stop();	
		if (sigismember(&sigset, SIGKMESS)) do_new_kmess(&tty_mess);
		continue;
	}
	case PANIC_DUMPS:		/* allow panic dumps */
		cons_stop();		/* switch to primary console */
		do_panic_dumps(&tty_mess);	
		continue;
	case DIAGNOSTICS: 		/* a server wants to print some */
		do_diagnostics(&tty_mess);
		continue;
	case FKEY_CONTROL:		/* (un)register a fkey observer */
		do_fkey_ctl(&tty_mess);
		continue;
	default:			/* should be a driver request */
		;			/* do nothing; end switch */
	}

	/* Only device requests should get to this point. All requests, 
	 * except DEV_STATUS, have a minor device number. Check this
	 * exception and get the minor device number otherwise.
	 */
	if (tty_mess.m_type == DEV_STATUS) {
		do_status(&tty_mess);
		continue;
	}
	line = tty_mess.TTY_LINE;
	if ((line - CONS_MINOR) < NR_CONS) {
		tp = tty_addr(line - CONS_MINOR);
	} else if (line == LOG_MINOR) {
		tp = tty_addr(0);
	} else if ((line - RS232_MINOR) < NR_RS_LINES) {
		tp = tty_addr(line - RS232_MINOR + NR_CONS);
	} else if ((line - TTYPX_MINOR) < NR_PTYS) {
		tp = tty_addr(line - TTYPX_MINOR + NR_CONS + NR_RS_LINES);
	} else if ((line - PTYPX_MINOR) < NR_PTYS) {
		tp = tty_addr(line - PTYPX_MINOR + NR_CONS + NR_RS_LINES);
		if (tty_mess.m_type != DEV_IOCTL) {
			do_pty(tp, &tty_mess);
			continue;
		}
	} else {
		tp = NULL;
	}

	/* If the device doesn't exist or is not configured return ENXIO. */
	if (tp == NULL || ! tty_active(tp)) {
		printf("Warning, TTY got illegal request %d from %d\n",
			tty_mess.m_type, tty_mess.m_source);
		tty_reply(TASK_REPLY, tty_mess.m_source,
						tty_mess.PROC_NR, ENXIO);
		continue;
	}

	/* Execute the requested device driver function. */
	switch (tty_mess.m_type) {
	    case DEV_READ:	 do_read(tp, &tty_mess);	  break;
	    case DEV_WRITE:	 do_write(tp, &tty_mess);	  break;
	    case DEV_IOCTL:	 do_ioctl(tp, &tty_mess);	  break;
	    case DEV_OPEN:	 do_open(tp, &tty_mess);	  break;
	    case DEV_CLOSE:	 do_close(tp, &tty_mess);	  break;
	    case DEV_SELECT:	 do_select(tp, &tty_mess);	  break;
	    case CANCEL:	 do_cancel(tp, &tty_mess);	  break;
	    default:		
		printf("Warning, TTY got unexpected request %d from %d\n",
			tty_mess.m_type, tty_mess.m_source);
	    tty_reply(TASK_REPLY, tty_mess.m_source,
						tty_mess.PROC_NR, EINVAL);
	}
  }
}

/*===========================================================================*
 *				do_status				     *
 *===========================================================================*/
PRIVATE void do_status(message *m_ptr)
{
  register struct tty *tp;
  int event_found;
  int status;
  int ops;
  
  /* Check for select or revive events on any of the ttys. If we found an, 
   * event return a single status message for it. The FS will make another 
   * call to see if there is more.
   */
  event_found = 0;
  for (tp = FIRST_TTY; tp < END_TTY; tp++) {
	if ((ops = select_try(tp, tp->tty_select_ops)) && 
			tp->tty_select_proc == m_ptr->m_source) {

		/* I/O for a selected minor device is ready. */
		m_ptr->m_type = DEV_IO_READY;
		m_ptr->DEV_MINOR = tp->tty_index;
		m_ptr->DEV_SEL_OPS = ops;

		tp->tty_select_ops &= ~ops;	/* unmark select event */
  		event_found = 1;
		break;
	}
	else if (tp->tty_inrevived && tp->tty_incaller == m_ptr->m_source) {
		
		/* Suspended request finished. Send a REVIVE. */
		m_ptr->m_type = DEV_REVIVE;
  		m_ptr->REP_PROC_NR = tp->tty_inproc;
  		m_ptr->REP_STATUS = tp->tty_incum;

		tp->tty_inleft = tp->tty_incum = 0;
		tp->tty_inrevived = 0;		/* unmark revive event */
  		event_found = 1;
  		break;
	}
	else if (tp->tty_outrevived && tp->tty_outcaller == m_ptr->m_source) {
		
		/* Suspended request finished. Send a REVIVE. */
		m_ptr->m_type = DEV_REVIVE;
  		m_ptr->REP_PROC_NR = tp->tty_outproc;
  		m_ptr->REP_STATUS = tp->tty_outcum;

		tp->tty_outcum = 0;
		tp->tty_outrevived = 0;		/* unmark revive event */
  		event_found = 1;
  		break;
	}
  }

#if NR_PTYS > 0
  if (!event_found)
  	event_found = pty_status(m_ptr);
#endif

  if (! event_found) {
	/* No events of interest were found. Return an empty message. */
  	m_ptr->m_type = DEV_NO_STATUS;
  }

  /* Almost done. Send back the reply message to the caller. */
  if ((status = send(m_ptr->m_source, m_ptr)) != OK) {
	panic("TTY","send in do_status failed, status\n", status);
  }
}

/*===========================================================================*
 *				do_read					     *
 *===========================================================================*/
/* pointer to tty struct */
/* pointer to message sent to the task */
PRIVATE void do_read(register tty_t *tp, register message *m_ptr)
{
/* A process wants to read from a terminal. */
  int r, status;
  phys_bytes phys_addr;

  /* Check if there is already a process hanging in a read, check if the
   * parameters are correct, do I/O.
   */
  if (tp->tty_inleft > 0) {
	r = EIO;
  } else
  if (m_ptr->COUNT <= 0) {
	r = EINVAL;
  } else
  if (sys_umap(m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS, m_ptr->COUNT,
		&phys_addr) != OK) {
	r = EFAULT;
  } else {
	/* Copy information from the message to the tty struct. */
	tp->tty_inrepcode = TASK_REPLY;
	tp->tty_incaller = m_ptr->m_source;
	tp->tty_inproc = m_ptr->PROC_NR;
	tp->tty_in_vir = (vir_bytes) m_ptr->ADDRESS;
	tp->tty_inleft = m_ptr->COUNT;

	if (!(tp->tty_termios.c_lflag & ICANON)
					&& tp->tty_termios.c_cc[VTIME] > 0) {
		if (tp->tty_termios.c_cc[VMIN] == 0) {
			/* MIN & TIME specify a read timer that finishes the
			 * read in TIME/10 seconds if no bytes are available.
			 */
			settimer(tp, TRUE);
			tp->tty_min = 1;
		} else {
			/* MIN & TIME specify an inter-byte timer that may
			 * have to be cancelled if there are no bytes yet.
			 */
			if (tp->tty_eotct == 0) {
				settimer(tp, FALSE);
				tp->tty_min = tp->tty_termios.c_cc[VMIN];
			}
		}
	}

	/* Anything waiting in the input buffer? Clear it out... */
	in_transfer(tp);
	/* ...then go back for more. */
	handle_events(tp);
	if (tp->tty_inleft == 0)  {
  		if (tp->tty_select_ops)
  			select_retry(tp);
		return;			/* already done */
	}

	/* There were no bytes in the input queue available, so either suspend
	 * the caller or break off the read if nonblocking.
	 */
	if (m_ptr->TTY_FLAGS & O_NONBLOCK) {
		r = EAGAIN;				/* cancel the read */
		tp->tty_inleft = tp->tty_incum = 0;
	} else {
		r = SUSPEND;				/* suspend the caller */
		tp->tty_inrepcode = REVIVE;
	}
  }
  tty_reply(TASK_REPLY, m_ptr->m_source, m_ptr->PROC_NR, r);
  if (tp->tty_select_ops)
  	select_retry(tp);
}

/*===========================================================================*
 *				do_write				     *
 *===========================================================================*/
/* pointer to message sent to the task */
PRIVATE void do_write(register tty_t *tp, register message *m_ptr)
{
/* A process wants to write on a terminal. */
  int r;
  phys_bytes phys_addr;

  /* Check if there is already a process hanging in a write, check if the
   * parameters are correct, do I/O.
   */
  if (tp->tty_outleft > 0) {
	r = EIO;
  } else
  if (m_ptr->COUNT <= 0) {
	r = EINVAL;
  } else
  if (sys_umap(m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS, m_ptr->COUNT,
		&phys_addr) != OK) {
	r = EFAULT;
  } else {
	/* Copy message parameters to the tty structure. */
	tp->tty_outrepcode = TASK_REPLY;
	tp->tty_outcaller = m_ptr->m_source;
	tp->tty_outproc = m_ptr->PROC_NR;
	tp->tty_out_vir = (vir_bytes) m_ptr->ADDRESS;
	tp->tty_outleft = m_ptr->COUNT;

	/* Try to write. */
	handle_events(tp);
	if (tp->tty_outleft == 0) 
		return;	/* already done */

	/* None or not all the bytes could be written, so either suspend the
	 * caller or break off the write if nonblocking.
	 */
	if (m_ptr->TTY_FLAGS & O_NONBLOCK) {		/* cancel the write */
		r = tp->tty_outcum > 0 ? tp->tty_outcum : EAGAIN;
		tp->tty_outleft = tp->tty_outcum = 0;
	} else {
		r = SUSPEND;				/* suspend the caller */
		tp->tty_outrepcode = REVIVE;
	}
  }
  tty_reply(TASK_REPLY, m_ptr->m_source, m_ptr->PROC_NR, r);
}

/*===========================================================================*
 *				do_ioctl				     *
 *===========================================================================*/
/* pointer to message sent to task */
PRIVATE void do_ioctl(register tty_t *tp, message *m_ptr)
{
/* Perform an IOCTL on this terminal. Posix termios calls are handled
 * by the IOCTL system call
 */

  int r;
  union {
	int i;
  } param;
  size_t size;

  /* Size of the ioctl parameter. */
  switch (m_ptr->TTY_REQUEST) {
    case TCGETS:        /* Posix tcgetattr function */
    case TCSETS:        /* Posix tcsetattr function, TCSANOW option */ 
    case TCSETSW:       /* Posix tcsetattr function, TCSADRAIN option */
    case TCSETSF:	/* Posix tcsetattr function, TCSAFLUSH option */
        size = sizeof(struct termios);
        break;

    case TCSBRK:        /* Posix tcsendbreak function */
    case TCFLOW:        /* Posix tcflow function */
    case TCFLSH:        /* Posix tcflush function */
    case TIOCGPGRP:     /* Posix tcgetpgrp function */
    case TIOCSPGRP:	/* Posix tcsetpgrp function */
        size = sizeof(int);
        break;

    case TIOCGWINSZ:    /* get window size (not Posix) */
    case TIOCSWINSZ:	/* set window size (not Posix) */
        size = sizeof(struct winsize);
        break;

    case KIOCSMAP:	/* load keymap (Minix extension) */
        size = sizeof(keymap_t);
        break;

    case TIOCSFON:	/* load font (Minix extension) */
        size = sizeof(u8_t [8192]);
        break;

    case TCDRAIN:	/* Posix tcdrain function -- no parameter */
    default:		size = 0;
  }

  r = OK;
  switch (m_ptr->TTY_REQUEST) {
    case TCGETS:
	/* Get the termios attributes. */
	r = sys_vircopy(SELF, D, (vir_bytes) &tp->tty_termios,
		m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS, 
		(vir_bytes) size);
	break;

    case TCSETSW:
    case TCSETSF:
    case TCDRAIN:
	if (tp->tty_outleft > 0) {
		/* Wait for all ongoing output processing to finish. */
		tp->tty_iocaller = m_ptr->m_source;
		tp->tty_ioproc = m_ptr->PROC_NR;
		tp->tty_ioreq = m_ptr->REQUEST;
		tp->tty_iovir = (vir_bytes) m_ptr->ADDRESS;
		r = SUSPEND;
		break;
	}
	if (m_ptr->TTY_REQUEST == TCDRAIN) break;
	if (m_ptr->TTY_REQUEST == TCSETSF) tty_icancel(tp);
	/*FALL THROUGH*/
    case TCSETS:
	/* Set the termios attributes. */
	r = sys_vircopy( m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS,
		SELF, D, (vir_bytes) &tp->tty_termios, (vir_bytes) size);
	if (r != OK) break;
	setattr(tp);
	break;

    case TCFLSH:
	r = sys_vircopy( m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS,
		SELF, D, (vir_bytes) &param.i, (vir_bytes) size);
	if (r != OK) break;
	switch (param.i) {
	    case TCIFLUSH:	tty_icancel(tp);		 	    break;
	    case TCOFLUSH:	(*tp->tty_ocancel)(tp, 0);		    break;
	    case TCIOFLUSH:	tty_icancel(tp); (*tp->tty_ocancel)(tp, 0); break;
	    default:		r = EINVAL;
	}
	break;

    case TCFLOW:
	r = sys_vircopy( m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS,
		SELF, D, (vir_bytes) &param.i, (vir_bytes) size);
	if (r != OK) break;
	switch (param.i) {
	    case TCOOFF:
	    case TCOON:
		tp->tty_inhibited = (param.i == TCOOFF);
		tp->tty_events = 1;
		break;
	    case TCIOFF:
		(*tp->tty_echo)(tp, tp->tty_termios.c_cc[VSTOP]);
		break;
	    case TCION:
		(*tp->tty_echo)(tp, tp->tty_termios.c_cc[VSTART]);
		break;
	    default:
		r = EINVAL;
	}
	break;

    case TCSBRK:
	if (tp->tty_break != NULL) (*tp->tty_break)(tp,0);
	break;

    case TIOCGWINSZ:
	r = sys_vircopy(SELF, D, (vir_bytes) &tp->tty_winsize,
		m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS, 
		(vir_bytes) size);
	break;

    case TIOCSWINSZ:
	r = sys_vircopy( m_ptr->PROC_NR, D, (vir_bytes) m_ptr->ADDRESS,
		SELF, D, (vir_bytes) &tp->tty_winsize, (vir_bytes) size);
	/* SIGWINCH... */
	break;

    case KIOCSMAP:
	/* Load a new keymap (only /dev/console). */
	if (isconsole(tp)) r = kbd_loadmap(m_ptr);
	break;

    case TIOCSFON:
	/* Load a font into an EGA or VGA card (hs@hck.hr) */
	if (isconsole(tp)) r = con_loadfont(m_ptr);
	break;

/* These Posix functions are allowed to fail if _POSIX_JOB_CONTROL is 
 * not defined.
 */
    case TIOCGPGRP:     
    case TIOCSPGRP:	
    default:
	r = ENOTTY;
  }

  /* Send the reply. */
  tty_reply(TASK_REPLY, m_ptr->m_source, m_ptr->PROC_NR, r);
}

/*===========================================================================*
 *				do_open					     *
 *===========================================================================*/
/* pointer to message sent to task */
PRIVATE void do_open(register tty_t *tp, message *m_ptr)
{
/* A tty line has been opened.  Make it the callers controlling tty if
 * O_NOCTTY is *not* set and it is not the log device.  1 is returned if
 * the tty is made the controlling tty, otherwise OK or an error code.
 */
  int r = OK;

  if (m_ptr->TTY_LINE == LOG_MINOR) {
	/* The log device is a write-only diagnostics device. */
	if (m_ptr->COUNT & R_BIT) r = EACCES;
  } else {
	if (!(m_ptr->COUNT & O_NOCTTY)) {
		tp->tty_pgrp = m_ptr->PROC_NR;
		r = 1;
	}
	tp->tty_openct++;
  }
  tty_reply(TASK_REPLY, m_ptr->m_source, m_ptr->PROC_NR, r);
}

/*===========================================================================*
 *				do_close				     *
 *===========================================================================*/
/* pointer to message sent to task */
PRIVATE void do_close(register tty_t *tp, message *m_ptr)
{
/* A tty line has been closed.  Clean up the line if it is the last close. */

  if (m_ptr->TTY_LINE != LOG_MINOR && --tp->tty_openct == 0) {
	tp->tty_pgrp = 0;
	tty_icancel(tp);
	(*tp->tty_ocancel)(tp, 0);
	(*tp->tty_close)(tp, 0);
	tp->tty_termios = termios_defaults;
	tp->tty_winsize = winsize_defaults;
	setattr(tp);
  }
  tty_reply(TASK_REPLY, m_ptr->m_source, m_ptr->PROC_NR, OK);
}

/*===========================================================================*
 *				do_cancel				     *
 *===========================================================================*/
/* pointer to message sent to task */
PRIVATE void do_cancel(register tty_t *tp, message *m_ptr)
{
/* A signal has been sent to a process that is hanging trying to read or write.
 * The pending read or write must be finished off immediately.
 */

  int proc_nr;
  int mode;

  /* Check the parameters carefully, to avoid cancelling twice. */
  proc_nr = m_ptr->PROC_NR;
  mode = m_ptr->COUNT;
  if ((mode & R_BIT) && tp->tty_inleft != 0 && proc_nr == tp->tty_inproc) {
	/* Process was reading when killed.  Clean up input. */
	tty_icancel(tp);
	tp->tty_inleft = tp->tty_incum = 0;
  }
  if ((mode & W_BIT) && tp->tty_outleft != 0 && proc_nr == tp->tty_outproc) {
	/* Process was writing when killed.  Clean up output. */
	(*tp->tty_ocancel)(tp, 0);
	tp->tty_outleft = tp->tty_outcum = 0;
  }
  if (tp->tty_ioreq != 0 && proc_nr == tp->tty_ioproc) {
	/* Process was waiting for output to drain. */
	tp->tty_ioreq = 0;
  }
  tp->tty_events = 1;
  tty_reply(TASK_REPLY, m_ptr->m_source, proc_nr, EINTR);
}

PUBLIC int select_try(struct tty *tp, int ops)
{
	int ready_ops = 0;

	/* Special case. If line is hung up, no operations will block.
	 * (and it can be seen as an exceptional condition.)
	 */
	if (tp->tty_termios.c_ospeed == B0) {
		ready_ops |= ops;
	}

	if (ops & SEL_RD) {
		/* will i/o not block on read? */
		if (tp->tty_inleft > 0) {
			ready_ops |= SEL_RD;	/* EIO - no blocking */
		} else if (tp->tty_incount > 0) {
			/* Is a regular read possible? tty_incount
			 * says there is data. But a read will only succeed
			 * in canonical mode if a newline has been seen.
			 */
			if (!(tp->tty_termios.c_lflag & ICANON) ||
				tp->tty_eotct > 0) {
				ready_ops |= SEL_RD;
			}
		}
	}

	if (ops & SEL_WR)  {
  		if (tp->tty_outleft > 0)  ready_ops |= SEL_WR;
		else if ((*tp->tty_devwrite)(tp, 1)) ready_ops |= SEL_WR;
	}

	return ready_ops;
}

PUBLIC int select_retry(struct tty *tp)
{
	if (select_try(tp, tp->tty_select_ops))
		notify(tp->tty_select_proc);
	return OK;
}

/*===========================================================================*
 *				handle_events				     *
 *===========================================================================*/
/* TTY to check for events. */
PUBLIC void handle_events(tty_t *tp)
{
/* Handle any events pending on a TTY.  These events are usually device
 * interrupts.
 *
 * Two kinds of events are prominent:
 *	- a character has been received from the console or an RS232 line.
 *	- an RS232 line has completed a write request (on behalf of a user).
 * The interrupt handler may delay the interrupt message at its discretion
 * to avoid swamping the TTY task.  Messages may be overwritten when the
 * lines are fast or when there are races between different lines, input
 * and output, because MINIX only provides single buffering for interrupt
 * messages (in proc.c).  This is handled by explicitly checking each line
 * for fresh input and completed output on each interrupt.
 */
  char *buf;
  unsigned count;
  int status;

  do {
	tp->tty_events = 0;

	/* Read input and perform input processing. */
	(*tp->tty_devread)(tp, 0);

	/* Perform output processing and write output. */
	(*tp->tty_devwrite)(tp, 0);

	/* Ioctl waiting for some event? */
	if (tp->tty_ioreq != 0) dev_ioctl(tp);
  } while (tp->tty_events);

  /* Transfer characters from the input queue to a waiting process. */
  in_transfer(tp);

  /* Reply if enough bytes are available. */
  if (tp->tty_incum >= tp->tty_min && tp->tty_inleft > 0) {
	if (tp->tty_inrepcode == REVIVE) {
		notify(tp->tty_incaller);
		tp->tty_inrevived = 1;
	} else {
		tty_reply(tp->tty_inrepcode, tp->tty_incaller, 
			tp->tty_inproc, tp->tty_incum);
		tp->tty_inleft = tp->tty_incum = 0;
	}
  }
  if (tp->tty_select_ops)
  	select_retry(tp);
#if NR_PTYS > 0
  if (ispty(tp))
  	select_retry_pty(tp);
#endif
}

/*===========================================================================*
 *				in_transfer				     *
 *===========================================================================*/
/* pointer to terminal to read from */
PRIVATE void in_transfer(register tty_t *tp)
{
/* Transfer bytes from the input queue to a process reading from a terminal. */

  int ch;
  int count;
  char buf[64], *bp;

  /* Force read to succeed if the line is hung up, looks like EOF to reader. */
  if (tp->tty_termios.c_ospeed == B0) tp->tty_min = 0;

  /* Anything to do? */
  if (tp->tty_inleft == 0 || tp->tty_eotct < tp->tty_min) return;

  bp = buf;
  while (tp->tty_inleft > 0 && tp->tty_eotct > 0) {
	ch = *tp->tty_intail;

	if (!(ch & IN_EOF)) {
		/* One character to be delivered to the user. */
		*bp = ch & IN_CHAR;
		tp->tty_inleft--;
		if (++bp == bufend(buf)) {
			/* Temp buffer full, copy to user space. */
			sys_vircopy(SELF, D, (vir_bytes) buf, 
				tp->tty_inproc, D, tp->tty_in_vir,
				(vir_bytes) buflen(buf));
			tp->tty_in_vir += buflen(buf);
			tp->tty_incum += buflen(buf);
			bp = buf;
		}
	}

	/* Remove the character from the input queue. */
	if (++tp->tty_intail == bufend(tp->tty_inbuf))
		tp->tty_intail = tp->tty_inbuf;
	tp->tty_incount--;
	if (ch & IN_EOT) {
		tp->tty_eotct--;
		/* Don't read past a line break in canonical mode. */
		if (tp->tty_termios.c_lflag & ICANON) tp->tty_inleft = 0;
	}
  }

  if (bp > buf) {
	/* Leftover characters in the buffer. */
	count = bp - buf;
	sys_vircopy(SELF, D, (vir_bytes) buf, 
		tp->tty_inproc, D, tp->tty_in_vir, (vir_bytes) count);
	tp->tty_in_vir += count;
	tp->tty_incum += count;
  }

  /* Usually reply to the reader, possibly even if incum == 0 (EOF). */
  if (tp->tty_inleft == 0) {
	if (tp->tty_inrepcode == REVIVE) {
		notify(tp->tty_incaller);
		tp->tty_inrevived = 1;
	} else {
		tty_reply(tp->tty_inrepcode, tp->tty_incaller, 
			tp->tty_inproc, tp->tty_incum);
		tp->tty_inleft = tp->tty_incum = 0;
	}
  }
}

/*===========================================================================*
 *				in_process				     *
 *===========================================================================*/
/* terminal on which character has arrived */
/* buffer with input characters */
/* number of input characters */
PUBLIC int in_process(register tty_t *tp, char *buf, int count)
{
/* Characters have just been typed in.  Process, save, and echo them.  Return
 * the number of characters processed.
 */

  int ch, sig, ct;
  int timeset = FALSE;
  static unsigned char csize_mask[] = { 0x1F, 0x3F, 0x7F, 0xFF };

  for (ct = 0; ct < count; ct++) {
	/* Take one character. */
	ch = *buf++ & BYTE;

	/* Strip to seven bits? */
	if (tp->tty_termios.c_iflag & ISTRIP) ch &= 0x7F;

	/* Input extensions? */
	if (tp->tty_termios.c_lflag & IEXTEN) {

		/* Previous character was a character escape? */
		if (tp->tty_escaped) {
			tp->tty_escaped = NOT_ESCAPED;
			ch |= IN_ESC;	/* protect character */
		}

		/* LNEXT (^V) to escape the next character? */
		if (ch == tp->tty_termios.c_cc[VLNEXT]) {
			tp->tty_escaped = ESCAPED;
			rawecho(tp, '^');
			rawecho(tp, '\b');
			continue;	/* do not store the escape */
		}

		/* REPRINT (^R) to reprint echoed characters? */
		if (ch == tp->tty_termios.c_cc[VREPRINT]) {
			reprint(tp);
			continue;
		}
	}

	/* _POSIX_VDISABLE is a normal character value, so better escape it. */
	if (ch == _POSIX_VDISABLE) ch |= IN_ESC;

	/* Map CR to LF, ignore CR, or map LF to CR. */
	if (ch == '\r') {
		if (tp->tty_termios.c_iflag & IGNCR) continue;
		if (tp->tty_termios.c_iflag & ICRNL) ch = '\n';
	} else
	if (ch == '\n') {
		if (tp->tty_termios.c_iflag & INLCR) ch = '\r';
	}

	/* Canonical mode? */
	if (tp->tty_termios.c_lflag & ICANON) {

		/* Erase processing (rub out of last character). */
		if (ch == tp->tty_termios.c_cc[VERASE]) {
			(void) back_over(tp);
			if (!(tp->tty_termios.c_lflag & ECHOE)) {
				(void) tty_echo(tp, ch);
			}
			continue;
		}

		/* Kill processing (remove current line). */
		if (ch == tp->tty_termios.c_cc[VKILL]) {
			while (back_over(tp)) {}
			if (!(tp->tty_termios.c_lflag & ECHOE)) {
				(void) tty_echo(tp, ch);
				if (tp->tty_termios.c_lflag & ECHOK)
					rawecho(tp, '\n');
			}
			continue;
		}

		/* EOF (^D) means end-of-file, an invisible "line break". */
		if (ch == tp->tty_termios.c_cc[VEOF]) ch |= IN_EOT | IN_EOF;

		/* The line may be returned to the user after an LF. */
		if (ch == '\n') ch |= IN_EOT;

		/* Same thing with EOL, whatever it may be. */
		if (ch == tp->tty_termios.c_cc[VEOL]) ch |= IN_EOT;
	}

	/* Start/stop input control? */
	if (tp->tty_termios.c_iflag & IXON) {

		/* Output stops on STOP (^S). */
		if (ch == tp->tty_termios.c_cc[VSTOP]) {
			tp->tty_inhibited = STOPPED;
			tp->tty_events = 1;
			continue;
		}

		/* Output restarts on START (^Q) or any character if IXANY. */
		if (tp->tty_inhibited) {
			if (ch == tp->tty_termios.c_cc[VSTART]
					|| (tp->tty_termios.c_iflag & IXANY)) {
				tp->tty_inhibited = RUNNING;
				tp->tty_events = 1;
				if (ch == tp->tty_termios.c_cc[VSTART])
					continue;
			}
		}
	}

	if (tp->tty_termios.c_lflag & ISIG) {
		/* Check for INTR (^?) and QUIT (^\) characters. */
		if (ch == tp->tty_termios.c_cc[VINTR]
					|| ch == tp->tty_termios.c_cc[VQUIT]) {
			sig = SIGINT;
			if (ch == tp->tty_termios.c_cc[VQUIT]) sig = SIGQUIT;
			sigchar(tp, sig);
			(void) tty_echo(tp, ch);
			continue;
		}
	}

	/* Is there space in the input buffer? */
	if (tp->tty_incount == buflen(tp->tty_inbuf)) {
		/* No space; discard in canonical mode, keep in raw mode. */
		if (tp->tty_termios.c_lflag & ICANON) continue;
		break;
	}

	if (!(tp->tty_termios.c_lflag & ICANON)) {
		/* In raw mode all characters are "line breaks". */
		ch |= IN_EOT;

		/* Start an inter-byte timer? */
		if (!timeset && tp->tty_termios.c_cc[VMIN] > 0
				&& tp->tty_termios.c_cc[VTIME] > 0) {
			settimer(tp, TRUE);
			timeset = TRUE;
		}
	}

	/* Perform the intricate function of echoing. */
	if (tp->tty_termios.c_lflag & (ECHO|ECHONL)) ch = tty_echo(tp, ch);

	/* Save the character in the input queue. */
	*tp->tty_inhead++ = ch;
	if (tp->tty_inhead == bufend(tp->tty_inbuf))
		tp->tty_inhead = tp->tty_inbuf;
	tp->tty_incount++;
	if (ch & IN_EOT) tp->tty_eotct++;

	/* Try to finish input if the queue threatens to overflow. */
	if (tp->tty_incount == buflen(tp->tty_inbuf)) in_transfer(tp);
  }
  return ct;
}

/*===========================================================================*
 *				echo					     *
 *===========================================================================*/
/* terminal on which to echo */
/* pointer to character to echo */
PRIVATE int tty_echo(register tty_t *tp, register int ch)
{
/* Echo the character if echoing is on.  Some control characters are echoed
 * with their normal effect, other control characters are echoed as "^X",
 * normal characters are echoed normally.  EOF (^D) is echoed, but immediately
 * backspaced over.  Return the character with the echoed length added to its
 * attributes.
 */
  int len, rp;

  ch &= ~IN_LEN;
  if (!(tp->tty_termios.c_lflag & ECHO)) {
	if (ch == ('\n' | IN_EOT) && (tp->tty_termios.c_lflag
					& (ICANON|ECHONL)) == (ICANON|ECHONL))
		(*tp->tty_echo)(tp, '\n');
	return(ch);
  }

  /* "Reprint" tells if the echo output has been messed up by other output. */
  rp = tp->tty_incount == 0 ? FALSE : tp->tty_reprint;

  if ((ch & IN_CHAR) < ' ') {
	switch (ch & (IN_ESC|IN_EOF|IN_EOT|IN_CHAR)) {
	    case '\t':
		len = 0;
		do {
			(*tp->tty_echo)(tp, ' ');
			len++;
		} while (len < TAB_SIZE && (tp->tty_position & TAB_MASK) != 0);
		break;
	    case '\r' | IN_EOT:
	    case '\n' | IN_EOT:
		(*tp->tty_echo)(tp, ch & IN_CHAR);
		len = 0;
		break;
	    default:
		(*tp->tty_echo)(tp, '^');
		(*tp->tty_echo)(tp, '@' + (ch & IN_CHAR));
		len = 2;
	}
  } else
  if ((ch & IN_CHAR) == '\177') {
	/* A DEL prints as "^?". */
	(*tp->tty_echo)(tp, '^');
	(*tp->tty_echo)(tp, '?');
	len = 2;
  } else {
	(*tp->tty_echo)(tp, ch & IN_CHAR);
	len = 1;
  }
  if (ch & IN_EOF) while (len > 0) { (*tp->tty_echo)(tp, '\b'); len--; }

  tp->tty_reprint = rp;
  return(ch | (len << IN_LSHIFT));
}

/*===========================================================================*
 *				rawecho					     *
 *===========================================================================*/
PRIVATE void rawecho(register tty_t *tp, int ch)
{
/* Echo without interpretation if ECHO is set. */
  int rp = tp->tty_reprint;
  if (tp->tty_termios.c_lflag & ECHO) (*tp->tty_echo)(tp, ch);
  tp->tty_reprint = rp;
}

/*===========================================================================*
 *				back_over				     *
 *===========================================================================*/
PRIVATE int back_over(register tty_t *tp)
{
/* Backspace to previous character on screen and erase it. */
  u16_t *head;
  int len;

  if (tp->tty_incount == 0) return(0);	/* queue empty */
  head = tp->tty_inhead;
  if (head == tp->tty_inbuf) head = bufend(tp->tty_inbuf);
  if (*--head & IN_EOT) return(0);		/* can't erase "line breaks" */
  if (tp->tty_reprint) reprint(tp);		/* reprint if messed up */
  tp->tty_inhead = head;
  tp->tty_incount--;
  if (tp->tty_termios.c_lflag & ECHOE) {
	len = (*head & IN_LEN) >> IN_LSHIFT;
	while (len > 0) {
		rawecho(tp, '\b');
		rawecho(tp, ' ');
		rawecho(tp, '\b');
		len--;
	}
  }
  return(1);				/* one character erased */
}

/*===========================================================================*
 *				reprint					     *
 *===========================================================================*/
/* pointer to tty struct */
PRIVATE void reprint(register tty_t *tp)
{
/* Restore what has been echoed to screen before if the user input has been
 * messed up by output, or if REPRINT (^R) is typed.
 */
  int count;
  u16_t *head;

  tp->tty_reprint = FALSE;

  /* Find the last line break in the input. */
  head = tp->tty_inhead;
  count = tp->tty_incount;
  while (count > 0) {
	if (head == tp->tty_inbuf) head = bufend(tp->tty_inbuf);
	if (head[-1] & IN_EOT) break;
	head--;
	count--;
  }
  if (count == tp->tty_incount) return;		/* no reason to reprint */

  /* Show REPRINT (^R) and move to a new line. */
  (void) tty_echo(tp, tp->tty_termios.c_cc[VREPRINT] | IN_ESC);
  rawecho(tp, '\r');
  rawecho(tp, '\n');

  /* Reprint from the last break onwards. */
  do {
	if (head == bufend(tp->tty_inbuf)) head = tp->tty_inbuf;
	*head = tty_echo(tp, *head);
	head++;
	count++;
  } while (count < tp->tty_incount);
}

/*===========================================================================*
 *				out_process				     *
 *===========================================================================*/
/* start/pos/end of circular buffer */
/* # input chars / input chars used */
/* max output chars / output chars used */
PUBLIC void out_process(tty_t *tp, char *bstart, char *bpos, char *bend, int *icount, int *ocount)
{
/* Perform output processing on a circular buffer.  *icount is the number of
 * bytes to process, and the number of bytes actually processed on return.
 * *ocount is the space available on input and the space used on output.
 * (Naturally *icount < *ocount.)  The column position is updated modulo
 * the TAB size, because we really only need it for tabs.
 */

  int tablen;
  int ict = *icount;
  int oct = *ocount;
  int pos = tp->tty_position;

  while (ict > 0) {
	switch (*bpos) {
	case '\7':
		break;
	case '\b':
		pos--;
		break;
	case '\r':
		pos = 0;
		break;
	case '\n':
		if ((tp->tty_termios.c_oflag & (OPOST|ONLCR))
							== (OPOST|ONLCR)) {
			/* Map LF to CR+LF if there is space.  Note that the
			 * next character in the buffer is overwritten, so
			 * we stop at this point.
			 */
			if (oct >= 2) {
				*bpos = '\r';
				if (++bpos == bend) bpos = bstart;
				*bpos = '\n';
				pos = 0;
				ict--;
				oct -= 2;
			}
			goto out_done;	/* no space or buffer got changed */
		}
		break;
	case '\t':
		/* Best guess for the tab length. */
		tablen = TAB_SIZE - (pos & TAB_MASK);

		if ((tp->tty_termios.c_oflag & (OPOST|XTABS))
							== (OPOST|XTABS)) {
			/* Tabs must be expanded. */
			if (oct >= tablen) {
				pos += tablen;
				ict--;
				oct -= tablen;
				do {
					*bpos = ' ';
					if (++bpos == bend) bpos = bstart;
				} while (--tablen != 0);
			}
			goto out_done;
		}
		/* Tabs are output directly. */
		pos += tablen;
		break;
	default:
		/* Assume any other character prints as one character. */
		pos++;
	}
	if (++bpos == bend) bpos = bstart;
	ict--;
	oct--;
  }
out_done:
  tp->tty_position = pos & TAB_MASK;

  *icount -= ict;	/* [io]ct are the number of chars not used */
  *ocount -= oct;	/* *[io]count are the number of chars that are used */
}

/*===========================================================================*
 *				dev_ioctl				     *
 *===========================================================================*/
PRIVATE void dev_ioctl(tty_t *tp)
{
/* The ioctl's TCSETSW, TCSETSF and TCDRAIN wait for output to finish to make
 * sure that an attribute change doesn't affect the processing of current
 * output.  Once output finishes the ioctl is executed as in do_ioctl().
 */
  int result;

  if (tp->tty_outleft > 0) return;		/* output not finished */

  if (tp->tty_ioreq != TCDRAIN) {
	if (tp->tty_ioreq == TCSETSF) tty_icancel(tp);
	result = sys_vircopy(tp->tty_ioproc, D, tp->tty_iovir,
			SELF, D, (vir_bytes) &tp->tty_termios,
			(vir_bytes) sizeof(tp->tty_termios));
	setattr(tp);
  }
  tp->tty_ioreq = 0;
  tty_reply(REVIVE, tp->tty_iocaller, tp->tty_ioproc, result);
}

/*===========================================================================*
 *				setattr					     *
 *===========================================================================*/
PRIVATE void setattr(tty_t *tp)
{
/* Apply the new line attributes (raw/canonical, line speed, etc.) */
  u16_t *inp;
  int count;

  if (!(tp->tty_termios.c_lflag & ICANON)) {
	/* Raw mode; put a "line break" on all characters in the input queue.
	 * It is undefined what happens to the input queue when ICANON is
	 * switched off, a process should use TCSAFLUSH to flush the queue.
	 * Keeping the queue to preserve typeahead is the Right Thing, however
	 * when a process does use TCSANOW to switch to raw mode.
	 */
	count = tp->tty_eotct = tp->tty_incount;
	inp = tp->tty_intail;
	while (count > 0) {
		*inp |= IN_EOT;
		if (++inp == bufend(tp->tty_inbuf)) inp = tp->tty_inbuf;
		--count;
	}
  }

  /* Inspect MIN and TIME. */
  settimer(tp, FALSE);
  if (tp->tty_termios.c_lflag & ICANON) {
	/* No MIN & TIME in canonical mode. */
	tp->tty_min = 1;
  } else {
	/* In raw mode MIN is the number of chars wanted, and TIME how long
	 * to wait for them.  With interesting exceptions if either is zero.
	 */
	tp->tty_min = tp->tty_termios.c_cc[VMIN];
	if (tp->tty_min == 0 && tp->tty_termios.c_cc[VTIME] > 0)
		tp->tty_min = 1;
  }

  if (!(tp->tty_termios.c_iflag & IXON)) {
	/* No start/stop output control, so don't leave output inhibited. */
	tp->tty_inhibited = RUNNING;
	tp->tty_events = 1;
  }

  /* Setting the output speed to zero hangs up the phone. */
  if (tp->tty_termios.c_ospeed == B0) sigchar(tp, SIGHUP);

  /* Set new line speed, character size, etc at the device level. */
  (*tp->tty_ioctl)(tp, 0);
}

/*===========================================================================*
 *				tty_reply				     *
 *===========================================================================*/
/* TASK_REPLY or REVIVE */
/* destination address for the reply */
/* to whom should the reply go? */
/* reply code */
PUBLIC void tty_reply(int code, int replyee, int proc_nr, int status)
{
/* Send a reply to a process that wanted to read or write data. */
  message tty_mess;

  tty_mess.m_type = code;
  tty_mess.REP_PROC_NR = proc_nr;
  tty_mess.REP_STATUS = status;

  if ((status = send(replyee, &tty_mess)) != OK) {
	panic("TTY","tty_reply failed, status\n", status);
  }
}

/*===========================================================================*
 *				sigchar					     *
 *===========================================================================*/
/* SIGINT, SIGQUIT, SIGKILL or SIGHUP */
PUBLIC void sigchar(register tty_t *tp, int sig)
{
/* Process a SIGINT, SIGQUIT or SIGKILL char from the keyboard or SIGHUP from
 * a tty close, "stty 0", or a real RS-232 hangup.  MM will send the signal to
 * the process group (INT, QUIT), all processes (KILL), or the session leader
 * (HUP).
 */
  int status;

  if (tp->tty_pgrp != 0) 
      if (OK != (status = sys_kill(tp->tty_pgrp, sig)))
        panic("TTY","Error, call to sys_kill failed", status);

  if (!(tp->tty_termios.c_lflag & NOFLSH)) {
	tp->tty_incount = tp->tty_eotct = 0;	/* kill earlier input */
	tp->tty_intail = tp->tty_inhead;
	(*tp->tty_ocancel)(tp, 0);			/* kill all output */
	tp->tty_inhibited = RUNNING;
	tp->tty_events = 1;
  }
}

/*===========================================================================*
 *				tty_icancel				     *
 *===========================================================================*/
PRIVATE void tty_icancel(register tty_t *tp)
{
/* Discard all pending input, tty buffer or device. */

  tp->tty_incount = tp->tty_eotct = 0;
  tp->tty_intail = tp->tty_inhead;
  (*tp->tty_icancel)(tp, 0);
}

/*===========================================================================*
 *				tty_init				     *
 *===========================================================================*/
PRIVATE void tty_init()
{
/* Initialize tty structure and call device initialization routines. */

  register tty_t *tp;
  int s;
  struct sigaction sigact;

  /* Initialize the terminal lines. */
  for (tp = FIRST_TTY,s=0; tp < END_TTY; tp++,s++) {

  	tp->tty_index = s;

  	tmr_inittimer(&tp->tty_tmr);

  	tp->tty_intail = tp->tty_inhead = tp->tty_inbuf;
  	tp->tty_min = 1;
  	tp->tty_termios = termios_defaults;
  	tp->tty_icancel = tp->tty_ocancel = tp->tty_ioctl = tp->tty_close =
								tty_devnop;
  	if (tp < tty_addr(NR_CONS)) {
		scr_init(tp);
  		tp->tty_minor = CONS_MINOR + s;
  	} else
  	if (tp < tty_addr(NR_CONS+NR_RS_LINES)) {
		rs_init(tp);
  		tp->tty_minor = RS232_MINOR + s-NR_CONS;
  	} else {
		pty_init(tp);
		tp->tty_minor = s - (NR_CONS+NR_RS_LINES) + TTYPX_MINOR;
  	}
  }
}

/*===========================================================================*
 *				tty_timed_out				     *
 *===========================================================================*/
PRIVATE void tty_timed_out(timer_t *tp)
{
/* This timer has expired. Set the events flag, to force processing. */
  tty_t *tty_ptr;
  tty_ptr = &tty_table[tmr_arg(tp)->ta_int];
  tty_ptr->tty_min = 0;			/* force read to succeed */
  tty_ptr->tty_events = 1;		
}

/*===========================================================================*
 *				expire_timers			    	     *
 *===========================================================================*/
PRIVATE void expire_timers(void)
{
/* A synchronous alarm message was received. Check if there are any expired 
 * timers. Possibly set the event flag and reschedule another alarm.  
 */
  clock_t now;				/* current time */
  int s;

  /* Get the current time to compare the timers against. */
  if ((s=getuptime(&now)) != OK)
 	panic("TTY","Couldn't get uptime from clock.", s);

  /* Scan the queue of timers for expired timers. This dispatch the watchdog
   * functions of expired timers. Possibly a new alarm call must be scheduled.
   */
  tmrs_exptimers(&tty_timers, now, NULL);
  if (tty_timers == NULL) tty_next_timeout = TMR_NEVER;
  else {  					  /* set new sync alarm */
  	tty_next_timeout = tty_timers->tmr_exp_time;
  	if ((s=sys_setalarm(tty_next_timeout, 1)) != OK)
 		panic("TTY","Couldn't set synchronous alarm.", s);
  }
}

/*===========================================================================*
 *				settimer				     *
 *===========================================================================*/
/* line to set or unset a timer on */
/* set timer if true, otherwise unset */
PRIVATE void settimer(tty_t *tty_ptr, int enable)
{
  clock_t now;				/* current time */
  clock_t exp_time;
  int s;

  /* Get the current time to calculate the timeout time. */
  if ((s=getuptime(&now)) != OK)
 	panic("TTY","Couldn't get uptime from clock.", s);
  if (enable) {
  	exp_time = now + tty_ptr->tty_termios.c_cc[VTIME] * (HZ/10);
 	/* Set a new timer for enabling the TTY events flags. */
 	tmrs_settimer(&tty_timers, &tty_ptr->tty_tmr, 
 		exp_time, tty_timed_out, NULL);  
  } else {
  	/* Remove the timer from the active and expired lists. */
  	tmrs_clrtimer(&tty_timers, &tty_ptr->tty_tmr, NULL);
  }
  
  /* Now check if a new alarm must be scheduled. This happens when the front
   * of the timers queue was disabled or reinserted at another position, or
   * when a new timer was added to the front.
   */
  if (tty_timers == NULL) tty_next_timeout = TMR_NEVER;
  else if (tty_timers->tmr_exp_time != tty_next_timeout) { 
  	tty_next_timeout = tty_timers->tmr_exp_time;
  	if ((s=sys_setalarm(tty_next_timeout, 1)) != OK)
 		panic("TTY","Couldn't set synchronous alarm.", s);
  }
}

/*===========================================================================*
 *				tty_devnop				     *
 *===========================================================================*/
PUBLIC int tty_devnop(tty_t *tp, int try)
{
  /* Some functions need not be implemented at the device level. */
}

/*===========================================================================*
 *				do_select				     *
 *===========================================================================*/
/* pointer to tty struct */
/* pointer to message sent to the task */
PRIVATE void do_select(register tty_t *tp, register message *m_ptr)
{
	int ops, ready_ops = 0, watch;

	ops = m_ptr->PROC_NR & (SEL_RD|SEL_WR|SEL_ERR);
	watch = (m_ptr->PROC_NR & SEL_NOTIFY) ? 1 : 0;

	ready_ops = select_try(tp, ops);

	if (!ready_ops && ops && watch) {
		tp->tty_select_ops |= ops;
		tp->tty_select_proc = m_ptr->m_source;
	}

        tty_reply(TASK_REPLY, m_ptr->m_source, m_ptr->PROC_NR, ready_ops);

        return;
}
