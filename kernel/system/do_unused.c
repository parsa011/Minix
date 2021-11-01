/* This file provides a catch-all handler for unused kernel calls. A kernel 
 * call may be unused when it is not defined or when it is disabled in the
 * kernel's configuration.
 */
#include "../system.h"

/*===========================================================================*
 *			          do_unused				     *
 *===========================================================================*/
/* pointer to request message */
PUBLIC int do_unused(message *m)
{
  kprintf("SYSTEM: got unused request %d from %d", m->m_type, m->m_source);
  return(EBADREQUEST);			/* illegal message type */
}

