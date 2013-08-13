/*******************************************************************************
 * arch/arm/src/sama5/sam_usbhost.c
 *
 *   Copyright (C) 2013 Gregory Nutt. All rights reserved.
 *   Authors: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************/

/*******************************************************************************
 * Included Files
 *******************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <semaphore.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/usb/usb.h>
#include <nuttx/usb/ohci.h>
#include <nuttx/usb/usbhost.h>

#include <arch/irq.h>

#include <arch/board/board.h> /* May redefine PIO settings */

#include "up_arch.h"
#include "up_internal.h"

#include "chip.h"
#include "sam_periphclks.h"
#include "sam_usbhost.h"
#include "chip/sam_pmc.h"
#include "chip/sam_sfr.h"
#include "chip/sam_ohci.h"

/*******************************************************************************
 * Definitions
 *******************************************************************************/
/* Configuration ***************************************************************/

/* Fixed endpoint descriptor size.  The actual size required by the hardware is only
 * 16 bytes, however, we set aside an additional 16 bytes for for internal use by
 * the OHCI host driver.  16-bytes is set aside because the EDs must still be
 * aligned to 16-byte boundaries.
 */

#define SAM_ED_SIZE 32

/* Configurable number of user endpoint descriptors (EDs).  This number excludes
 * the control endpoint that is always allocated.
 */

#ifndef CONFIG_SAMA5_OHCI_NEDS
#  define CONFIG_SAMA5_OHCI_NEDS 2
#endif

/* Fixed transfer descriptor size.  The actual size required by the hardware is
 * only 16 bytes, however, we set aside an additional 16 bytes for for internal
 * use by the OHCI host driver.  16-bytes is set aside because the TDs must still
 * be aligned to 16-byte boundaries.
 */

#define SAM_TD_SIZE 32

/* Configurable number of user transfer descriptors (TDs).  */

#ifndef CONFIG_SAMA5_OHCI_NTDS
#  define CONFIG_SAMA5_OHCI_NTDS 3
#endif

#if CONFIG_SAMA5_OHCI_NTDS < 2
#  error Insufficent number of transfer descriptors (CONFIG_SAMA5_OHCI_NTDS < 2)
#endif

/* Configurable number of request/descriptor buffers (TDBUFFER) */

#ifndef CONFIG_SAMA5_OHCI_TDBUFFERS
#  define CONFIG_SAMA5_OHCI_TDBUFFERS 2
#endif

#if CONFIG_SAMA5_OHCI_TDBUFFERS < 2
#  error At least two TD buffers are required (CONFIG_SAMA5_OHCI_TDBUFFERS < 2)
#endif

/* Configurable size of one TD buffer */

#if CONFIG_SAMA5_OHCI_TDBUFFERS > 0 && !defined(CONFIG_SAMA5_OHCI_TDBUFSIZE)
#  define CONFIG_SAMA5_OHCI_TDBUFSIZE 128
#endif

#if (CONFIG_SAMA5_OHCI_TDBUFSIZE & 3) != 0
#  error "TD buffer size must be an even number of 32-bit words"
#endif

/* Total buffer size */

#define SAM_BUFALLOC (CONFIG_SAMA5_OHCI_TDBUFFERS * CONFIG_SAMA5_OHCI_TDBUFSIZE)

/* Debug */

#ifndef CONFIG_DEBUG
#  undef CONFIG_SAMA5_OHCI_REGDEBUG
#endif

/* OHCI Setup ******************************************************************/
/* Frame Interval / Periodic Start */

#define BITS_PER_FRAME          12000
#define FI                     (BITS_PER_FRAME-1)
#define FSMPS                  ((6 * (FI - 210)) / 7)
#define DEFAULT_FMINTERVAL     ((FSMPS << OHCI_FMINT_FSMPS_SHIFT) | FI)
#define DEFAULT_PERSTART       (((9 * BITS_PER_FRAME) / 10) - 1)

/* CLKCTRL enable bits */

#define SAM_CLKCTRL_ENABLES   (USBOTG_CLK_HOSTCLK|USBOTG_CLK_PORTSELCLK|USBOTG_CLK_AHBCLK)

/* Interrupt enable bits */

#ifdef CONFIG_DEBUG_USB
#  define SAM_DEBUG_INTS      (OHCI_INT_SO|OHCI_INT_RD|OHCI_INT_UE|OHCI_INT_OC)
#else
#  define SAM_DEBUG_INTS      0
#endif

#define SAM_NORMAL_INTS       (OHCI_INT_WDH|OHCI_INT_RHSC)
#define SAM_ALL_INTS          (SAM_NORMAL_INTS|SAM_DEBUG_INTS)

/* Periodic Intervals **********************************************************/
/* Periodic intervals 2, 4, 8, 16,and 32 supported */

#define MIN_PERINTERVAL 2
#define MAX_PERINTERVAL 32

/* Descriptors *****************************************************************/

/* TD delay interrupt value */

#define TD_DELAY(n) (uint32_t)((n) << GTD_STATUS_DI_SHIFT)

/*******************************************************************************
 * Private Types
 *******************************************************************************/
/* This structure retins the state of one root hub port */

struct sam_rhport_s
{
  /* Root hub port status */

  volatile bool connected;      /* Connected to device */
  volatile bool lowspeed;       /* Low speed device attached. */

  /* The bound device class driver */

  struct usbhost_class_s *class;
};

/* This structure retains the state of the USB host controller */

struct sam_ohci_s
{
  /* Common device fields.  This must be the first thing defined in the
   * structure so that it is possible to simply cast from struct usbhost_s
   * to structsam_usbhost_s.
   */

  struct usbhost_driver_s drvr;

  /* Driver status */

  volatile bool rhswait;        /* TRUE: Thread is waiting for Root Hub Status change */

#ifndef CONFIG_USBHOST_INT_DISABLE
  uint8_t ininterval;           /* Minimum periodic IN EP polling interval: 2, 4, 6, 16, or 32 */
  uint8_t outinterval;          /* Minimum periodic IN EP polling interval: 2, 4, 6, 16, or 32 */
#endif
  sem_t exclsem;                /* Support mutually exclusive access */
  sem_t rhssem;                 /* Semaphore to wait Writeback Done Head event */

  /* Root hub ports */

  struct sam_rhport_s rhport[SAM_USBHOST_NRHPORT];

  /* Debug stuff */

#ifdef CONFIG_SAMA5_SPI_REGDEBUG
   bool wrlast;                 /* Last was a write */
   uint32_t addresslast;        /* Last address */
   uint32_t valuelast;          /* Last value */
   int ntimes;                  /* Number of times */
#endif
};

/* The OCHI expects the size of an endpoint descriptor to be 16 bytes.
 * However, the size allocated for an endpoint descriptor is 32 bytes in
 * sam_ohciram.h.  This extra 16-bytes is used by the OHCI host driver in
 * order to maintain additional endpoint-specific data.
 */

struct sam_ed_s
{
  /* Hardware specific fields */

  struct ohci_ed_s hw;

  /* Software specific fields */

  uint8_t          xfrtype;   /* Transfer type.  See SB_EP_ATTR_XFER_* in usb.h */
  uint8_t          interval;  /* Periodic EP polling interval: 2, 4, 6, 16, or 32 */
  volatile uint8_t tdstatus;  /* TD control status bits from last Writeback Done Head event */
  volatile bool    wdhwait;   /* TRUE: Thread is waiting for WDH interrupt */
  sem_t            wdhsem;    /* Semaphore used to wait for Writeback Done Head event */
                              /* Unused bytes follow, depending on the size of sem_t */
};

/* The OCHI expects the size of an transfer descriptor to be 16 bytes.
 * However, the size allocated for an endpoint descriptor is 32 bytes in
 * RAM.  This extra 16-bytes is used by the OHCI host driver in order to
 * maintain additional endpoint-specific data.
 */

struct sam_gtd_s
{
  /* Hardware specific fields */

  struct ohci_gtd_s hw;

  /* Software specific fields */

  struct sam_ed_s *ed;      /* Pointer to parent ED */
  uint8_t          pad[12];
};

/* The following is used to manage lists of free EDs, TDs, and TD buffers */

struct sam_list_s
{
  struct sam_list_s *flink; /* Link to next buffer in the list */
                            /* Variable length buffer data follows */
};

/*******************************************************************************
 * Private Function Prototypes
 *******************************************************************************/

/* Register operations ********************************************************/

#ifdef CONFIG_SAMA5_OHCI_REGDEBUG
static void sam_printreg(uint32_t addr, uint32_t val, bool iswrite);
static void sam_checkreg(uint32_t addr, uint32_t val, bool iswrite);
static uint32_t sam_getreg(uint32_t addr);
static void sam_putreg(uint32_t val, uint32_t addr);
#else
# define sam_getreg(addr)     getreg32(addr)
# define sam_putreg(val,addr) putreg32(val,addr)
#endif

/* Semaphores ******************************************************************/

static void sam_takesem(sem_t *sem);
#define sam_givesem(s) sem_post(s);

/* Byte stream access helper functions *****************************************/

static inline uint16_t sam_getle16(const uint8_t *val);
#if 0 /* Not used */
static void sam_putle16(uint8_t *dest, uint16_t val);
#endif

/* OHCI memory pool helper functions *******************************************/

static inline void sam_edfree(struct sam_ed_s *ed);
static struct sam_gtd_s *sam_tdalloc(void);
static void sam_tdfree(struct sam_gtd_s *buffer);
static uint8_t *sam_tballoc(void);
static void sam_tbfree(uint8_t *buffer);

/* ED list helper functions ****************************************************/

static inline int sam_addbulked(struct sam_ohci_s *priv,
                                struct sam_ed_s *ed);
static inline int sam_rembulked(struct sam_ohci_s *priv,
                                struct sam_ed_s *ed);

#if !defined(CONFIG_USBHOST_INT_DISABLE) || !defined(CONFIG_USBHOST_ISOC_DISABLE)
static unsigned int sam_getinterval(uint8_t interval);
static void sam_setinttab(uint32_t value, unsigned int interval, unsigned int offset);
#endif

static inline int sam_addinted(struct sam_ohci_s *priv,
                               const FAR struct usbhost_epdesc_s *epdesc,
                               struct sam_ed_s *ed);
static inline int sam_reminted(struct sam_ohci_s *priv,
                               struct sam_ed_s *ed);

static inline int sam_addisoced(struct sam_ohci_s *priv,
                                const FAR struct usbhost_epdesc_s *epdesc,
                                struct sam_ed_s *ed);
static inline int sam_remisoced(struct sam_ohci_s *priv,
                                struct sam_ed_s *ed);

/* Descriptor helper functions *************************************************/

static int sam_enqueuetd(struct sam_ohci_s *priv,
                         struct sam_ed_s *ed, uint32_t dirpid,
                         uint32_t toggle, volatile uint8_t *buffer,
                         size_t buflen);
static int sam_ctrltd(struct sam_ohci_s *priv, uint32_t dirpid,
                      uint8_t *buffer, size_t buflen);

/* Interrupt handling **********************************************************/

static void sam_rhsc_interrupt(struct sam_ohci_s *priv);
static void sam_wdh_interrupt(struct sam_ohci_s *priv);
static int sam_ohci_interrupt(int irq, FAR void *context);

/* USB host controller operations **********************************************/

static int sam_wait(FAR struct usbhost_driver_s *drvr, FAR const bool *connected);
static int sam_enumerate(FAR struct usbhost_driver_s *drvr, int rhpndx);
static int sam_ep0configure(FAR struct usbhost_driver_s *drvr, uint8_t funcaddr,
                            uint16_t maxpacketsize);
static int sam_epalloc(FAR struct usbhost_driver_s *drvr,
                       const FAR struct usbhost_epdesc_s *epdesc, usbhost_ep_t *ep);
static int sam_epfree(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep);
static int sam_alloc(FAR struct usbhost_driver_s *drvr,
                     FAR uint8_t **buffer, FAR size_t *maxlen);
static int sam_free(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer);
static int sam_ioalloc(FAR struct usbhost_driver_s *drvr,
                       FAR uint8_t **buffer, size_t buflen);
static int sam_iofree(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer);
static int sam_ctrlin(FAR struct usbhost_driver_s *drvr,
                      FAR const struct usb_ctrlreq_s *req,
                      FAR uint8_t *buffer);
static int sam_ctrlout(FAR struct usbhost_driver_s *drvr,
                       FAR const struct usb_ctrlreq_s *req,
                       FAR const uint8_t *buffer);
static int sam_transfer(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep,
                        FAR uint8_t *buffer, size_t buflen);
static void sam_disconnect(FAR struct usbhost_driver_s *drvr);

/* Initialization **************************************************************/

static inline void sam_ep0init(struct sam_ohci_s *priv);

/*******************************************************************************
 * Private Data
 *******************************************************************************/

/* In this driver implementation, support is provided for only a single a single
 * USB device.  All status information can be simply retained in a single global
 * instance.
 */

static struct sam_ohci_s g_usbhost =
{
  .drvr             =
    {
      .wait         = sam_wait,
      .enumerate    = sam_enumerate,
      .ep0configure = sam_ep0configure,
      .epalloc      = sam_epalloc,
      .epfree       = sam_epfree,
      .alloc        = sam_alloc,
      .free         = sam_free,
      .ioalloc      = sam_ioalloc,
      .iofree       = sam_iofree,
      .ctrlin       = sam_ctrlin,
      .ctrlout      = sam_ctrlout,
      .transfer     = sam_transfer,
      .disconnect   = sam_disconnect,
    },
};

/* This is a free list of EDs and TD buffers */

static struct sam_list_s *g_edfree; /* List of unused EDs */
static struct sam_list_s *g_tdfree; /* List of unused TDs */
static struct sam_list_s *g_tbfree; /* List of unused transfer buffers */

/* Allocated descriptor memory. These must all be properly aligned
 * and must be positioned in a DMA-able memory region.
 */

/* This must be aligned to a 256-byte boundary */

static struct ohci_hcca_s g_hcca __attribute__ ((aligned (256)));

/* These must be aligned to 8-byte boundaries (we do 16-byte alignment). */

static struct sam_gtd_s   g_tdtail __attribute__ ((aligned (16)));
static struct sam_ed_s    g_edctrl __attribute__ ((aligned (16)));

/* Pools of free descriptors and buffers.  These will all be linked
 * into the free lists declared above.
 */

static struct sam_ed_s    g_edalloc[CONFIG_SAMA5_OHCI_NEDS]
                          __attribute__ ((aligned (16)));
static struct sam_gtd_s   g_tdalloc[CONFIG_SAMA5_OHCI_NTDS]
                          __attribute__ ((aligned (16)));
static uint8_t            g_bufalloc[SAM_BUFALLOC]
                          __attribute__ ((aligned (16)));

/*******************************************************************************
 * Public Data
 *******************************************************************************/

/*******************************************************************************
 * Private Functions
 *******************************************************************************/

/*******************************************************************************
 * Name: sam_printreg
 *
 * Description:
 *   Print the contents of an SAMA5 OHCI register operation
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_OHCI_REGDEBUG
static void sam_printreg(uint32_t addr, uint32_t val, bool iswrite)
{
  lldbg("%08x%s%08x\n", addr, iswrite ? "<-" : "->", val);
}
#endif

/*******************************************************************************
 * Name: sam_checkreg
 *
 * Description:
 *   Get the contents of an SAMA5 OHCI register
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_OHCI_REGDEBUG
static void sam_checkreg(uint32_t addr, uint32_t val, bool iswrite)
{
  static uint32_t prevaddr = 0;
  static uint32_t preval = 0;
  static uint32_t count = 0;
  static bool     prevwrite = false;

  /* Is this the same value that we read from/wrote to the same register last time?
   * Are we polling the register?  If so, suppress the output.
   */

  if (addr == prevaddr && val == preval && prevwrite == iswrite)
    {
      /* Yes.. Just increment the count */

      count++;
    }
  else
    {
      /* No this is a new address or value or operation. Were there any
       * duplicate accesses before this one?
       */

      if (count > 0)
        {
          /* Yes.. Just one? */

          if (count == 1)
            {
              /* Yes.. Just one */

              sam_printreg(prevaddr, preval, prevwrite);
            }
          else
            {
              /* No.. More than one. */

              lldbg("[repeats %d more times]\n", count);
            }
        }

      /* Save the new address, value, count, and operation for next time */

      prevaddr  = addr;
      preval    = val;
      count     = 0;
      prevwrite = iswrite;

      /* Show the new regisgter access */

      sam_printreg(addr, val, iswrite);
    }
}
#endif

/*******************************************************************************
 * Name: sam_getreg
 *
 * Description:
 *   Get the contents of an SAMA5 register
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_OHCI_REGDEBUG
static uint32_t sam_getreg(uint32_t addr)
{
  /* Read the value from the register */

  uint32_t val = getreg32(addr);

  /* Check if we need to print this value */

  sam_checkreg(addr, val, false);
  return val;
}
#endif

/*******************************************************************************
 * Name: sam_putreg
 *
 * Description:
 *   Set the contents of an SAMA5 register to a value
 *
 *******************************************************************************/

#ifdef CONFIG_SAMA5_OHCI_REGDEBUG
static void sam_putreg(uint32_t val, uint32_t addr)
{
  /* Check if we need to print this value */

  sam_checkreg(addr, val, true);

  /* Write the value */

  putreg32(val, addr);
}
#endif

/****************************************************************************
 * Name: sam_takesem
 *
 * Description:
 *   This is just a wrapper to handle the annoying behavior of semaphore
 *   waits that return due to the receipt of a signal.
 *
 *******************************************************************************/

static void sam_takesem(sem_t *sem)
{
  /* Take the semaphore (perhaps waiting) */

  while (sem_wait(sem) != 0)
    {
      /* The only case that an error should occr here is if the wait was
       * awakened by a signal.
       */

      ASSERT(errno == EINTR);
    }
}

/****************************************************************************
 * Name: sam_getle16
 *
 * Description:
 *   Get a (possibly unaligned) 16-bit little endian value.
 *
 *******************************************************************************/

static inline uint16_t sam_getle16(const uint8_t *val)
{
  return (uint16_t)val[1] << 8 | (uint16_t)val[0];
}

/****************************************************************************
 * Name: sam_putle16
 *
 * Description:
 *   Put a (possibly unaligned) 16-bit little endian value.
 *
 *******************************************************************************/

#if 0 /* Not used */
static void sam_putle16(uint8_t *dest, uint16_t val)
{
  dest[0] = val & 0xff; /* Little endian means LS byte first in byte stream */
  dest[1] = val >> 8;
}
#endif

/*******************************************************************************
 * Name: sam_edfree
 *
 * Description:
 *   Return an endpoint descriptor to the free list
 *
 *******************************************************************************/

static inline void sam_edfree(struct sam_ed_s *ed)
{
  struct sam_list_s *entry = (struct sam_list_s *)ed;

  /* Put the ED back into the free list */

  entry->flink = g_edfree;
  g_edfree     = entry;
}

/*******************************************************************************
 * Name: sam_tdalloc
 *
 * Description:
 *   Allocate an transfer descriptor from the free list
 *
 * Assumptions:
 *   - Never called from an interrupt handler.
 *   - Protected from conconcurrent access to the TD pool by the interrupt
 *     handler
 *   - Protection from re-entrance must be assured by the caller
 *
 *******************************************************************************/

static struct sam_gtd_s *sam_tdalloc(void)
{
  struct sam_gtd_s *ret;
  irqstate_t flags;

  /* Disable interrupts momentarily so that sam_tdfree is not called from the
   * interrupt handler.
   */

  flags = irqsave();
  ret   = (struct sam_gtd_s *)g_tdfree;
  if (ret)
    {
      g_tdfree = ((struct sam_list_s*)ret)->flink;
    }

  irqrestore(flags);
  return ret;
}

/*******************************************************************************
 * Name: sam_tdfree
 *
 * Description:
 *   Return an transfer descriptor to the free list
 *
 * Assumptions:
 *   - Only called from the WDH interrupt handler (and during initialization).
 *   - Interrupts are disabled in any case.
 *
 *******************************************************************************/

static void sam_tdfree(struct sam_gtd_s *td)
{
  struct sam_list_s *tdfree = (struct sam_list_s *)td;

  /* This should not happen but just to be safe, don't free the common, pre-
   * allocated tail TD.
   */

 if (tdfree != NULL && td != &g_tdtail)
    {
      tdfree->flink           = g_tdfree;
      g_tdfree                = tdfree;
    }
}

/*******************************************************************************
 * Name: sam_tballoc
 *
 * Description:
 *   Allocate an request/descriptor transfer buffer from the free list
 *
 * Assumptions:
 *   - Never called from an interrupt handler.
 *   - Protection from re-entrance must be assured by the caller
 *
 *******************************************************************************/

static uint8_t *sam_tballoc(void)
{
  uint8_t *ret = (uint8_t *)g_tbfree;
  if (ret)
    {
      g_tbfree = ((struct sam_list_s*)ret)->flink;
    }

  return ret;
}

/*******************************************************************************
 * Name: sam_tbfree
 *
 * Description:
 *   Return an request/descriptor transfer buffer to the free list
 *
 *******************************************************************************/

static void sam_tbfree(uint8_t *buffer)
{
  struct sam_list_s *tbfree = (struct sam_list_s *)buffer;

  if (tbfree)
    {
      tbfree->flink = g_tbfree;
      g_tbfree      = tbfree;
    }
}

/*******************************************************************************
 * Name: sam_addbulked
 *
 * Description:
 *   Helper function to add an ED to the bulk list.
 *
 *******************************************************************************/

static inline int sam_addbulked(struct sam_ohci_s *priv,
                                struct sam_ed_s *ed)
{
#ifndef CONFIG_USBHOST_BULK_DISABLE
  uint32_t regval;

  /* Add the new bulk ED to the head of the bulk list */

  ed->hw.nexted = sam_getreg(SAM_USBHOST_BULKHEADED);
  sam_putreg((uint32_t)ed, SAM_USBHOST_BULKHEADED);

  /* BulkListEnable. This bit is set to enable the processing of the
   * Bulk list.  Note: once enabled, it remains.  We really should
   * never modify the bulk list while BLE is set.
   */

  regval  = sam_getreg(SAM_USBHOST_CTRL);
  regval |= OHCI_CTRL_BLE;
  sam_putreg(regval, SAM_USBHOST_CTRL);
  return OK;
#else
  return -ENOSYS;
#endif
}

/*******************************************************************************
 * Name: sam_rembulked
 *
 * Description:
 *   Helper function remove an ED from the bulk list.
 *
 *******************************************************************************/

static inline int sam_rembulked(struct sam_ohci_s *priv,
                                struct sam_ed_s *ed)
{
#ifndef CONFIG_USBHOST_BULK_DISABLE
  struct sam_ed_s *curr;
  struct sam_ed_s *prev;
  uint32_t         regval;

  /* Find the ED in the bulk list.  NOTE: We really should never be mucking
   * with the bulk list while BLE is set.
   */

  for (curr = (struct sam_ed_s *)sam_getreg(SAM_USBHOST_BULKHEADED),
       prev = NULL;
       curr && curr != ed;
       prev = curr, curr = (struct sam_ed_s *)curr->hw.nexted);

  /* Hmmm.. It would be a bug if we do not find the ED in the bulk list. */

  DEBUGASSERT(curr != NULL);

  /* Remove the ED from the bulk list */

  if (curr != NULL)
    {
      /* Is this ED the first on in the bulk list? */

      if (prev == NULL)
        {
          /* Yes... set the head of the bulk list to skip over this ED */

          sam_putreg(ed->hw.nexted, SAM_USBHOST_BULKHEADED);

          /* If the bulk list is now empty, then disable it */

          regval  = sam_getreg(SAM_USBHOST_CTRL);
          regval &= ~OHCI_CTRL_BLE;
          sam_putreg(regval, SAM_USBHOST_CTRL);
        }
      else
        {
          /* No.. set the forward link of the previous ED in the list
           * skip over this ED.
           */

          prev->hw.nexted = ed->hw.nexted;
        }
    }

  return OK;
#else
  return -ENOSYS;
#endif
}

/*******************************************************************************
 * Name: sam_getinterval
 *
 * Description:
 *   Convert the endpoint polling interval into a HCCA table increment
 *
 *******************************************************************************/

#if !defined(CONFIG_USBHOST_INT_DISABLE) || !defined(CONFIG_USBHOST_ISOC_DISABLE)
static unsigned int sam_getinterval(uint8_t interval)
{
  /* The bInterval field of the endpoint descriptor contains the polling interval
   * for interrupt and isochronous endpoints. For other types of endpoint, this
   * value should be ignored. bInterval is provided in units of 1MS frames.
   */

  if (interval < 3)
    {
      return 2;
    }
  else if (interval < 7)
    {
      return 4;
    }
  else if (interval < 15)
    {
      return 8;
    }
  else if (interval < 31)
    {
      return 16;
    }
  else
    {
      return 32;
    }
}
#endif

/*******************************************************************************
 * Name: sam_setinttab
 *
 * Description:
 *   Set the interrupt table to the selected value using the provided interval
 *   and offset.
 *
 *******************************************************************************/

#if !defined(CONFIG_USBHOST_INT_DISABLE) || !defined(CONFIG_USBHOST_ISOC_DISABLE)
static void sam_setinttab(uint32_t value, unsigned int interval, unsigned int offset)
{
  unsigned int i;
  for (i = offset; i < HCCA_INTTBL_WSIZE; i += interval)
    {
      g_hcca.inttbl[i] = value;
    }
}
#endif

/*******************************************************************************
 * Name: sam_addinted
 *
 * Description:
 *   Helper function to add an ED to the HCCA interrupt table.
 *
 *   To avoid reshuffling the table so much and to keep life simple in general,
 *    the following rules are applied:
 *
 *     1. IN EDs get the even entries, OUT EDs get the odd entries.
 *     2. Add IN/OUT EDs are scheduled together at the minimum interval of all
 *        IN/OUT EDs.
 *
 *   This has the following consequences:
 *
 *     1. The minimum support polling rate is 2MS, and
 *     2. Some devices may get polled at a much higher rate than they request.
 *
 *******************************************************************************/

static inline int sam_addinted(struct sam_ohci_s *priv,
                                 const FAR struct usbhost_epdesc_s *epdesc,
                                 struct sam_ed_s *ed)
{
#ifndef CONFIG_USBHOST_INT_DISABLE
  unsigned int interval;
  unsigned int offset;
  uint32_t head;
  uint32_t regval;

  /* Disable periodic list processing.  Does this take effect immediately?  Or
   * at the next SOF... need to check.
   */

  regval  = sam_getreg(SAM_USBHOST_CTRL);
  regval &= ~OHCI_CTRL_PLE;
  sam_putreg(regval, SAM_USBHOST_CTRL);

  /* Get the quanitized interval value associated with this ED and save it
   * in the ED.
   */

  interval     = sam_getinterval(epdesc->interval);
  ed->interval = interval;
  uvdbg("interval: %d->%d\n", epdesc->interval, interval);

  /* Get the offset associated with the ED direction. IN EDs get the even
   * entries, OUT EDs get the odd entries.
   *
   * Get the new, minimum interval. Add IN/OUT EDs are scheduled together
   * at the minimum interval of all IN/OUT EDs.
   */

  if (epdesc->in)
    {
      offset = 0;
      if (priv->ininterval > interval)
        {
          priv->ininterval = interval;
        }
      else
        {
          interval = priv->ininterval;
        }
    }
  else
    {
      offset = 1;
      if (priv->outinterval > interval)
        {
          priv->outinterval = interval;
        }
      else
        {
          interval = priv->outinterval;
        }
    }
  uvdbg("min interval: %d offset: %d\n", interval, offset);

  /* Get the head of the first of the duplicated entries.  The first offset
   * entry is always guaranteed to contain the common ED list head.
   */

  head = g_hcca.inttbl[offset];

  /* Clear all current entries in the interrupt table for this direction */

  sam_setinttab(0, 2, offset);

  /* Add the new ED before the old head of the periodic ED list and set the
   * new ED as the head ED in all of the appropriate entries of the HCCA
   * interrupt table.
   */

  ed->hw.nexted = head;
  sam_setinttab((uint32_t)ed, interval, offset);
  uvdbg("head: %08x next: %08x\n", ed, head);

  /* Re-enabled periodic list processing */

  regval  = sam_getreg(SAM_USBHOST_CTRL);
  regval |= OHCI_CTRL_PLE;
  sam_putreg(regval, SAM_USBHOST_CTRL);
  return OK;
#else
  return -ENOSYS;
#endif
}

/*******************************************************************************
 * Name: sam_reminted
 *
 * Description:
 *   Helper function to remove an ED from the HCCA interrupt table.
 *
 *   To avoid reshuffling the table so much and to keep life simple in general,
 *    the following rules are applied:
 *
 *     1. IN EDs get the even entries, OUT EDs get the odd entries.
 *     2. Add IN/OUT EDs are scheduled together at the minimum interval of all
 *        IN/OUT EDs.
 *
 *   This has the following consequences:
 *
 *     1. The minimum support polling rate is 2MS, and
 *     2. Some devices may get polled at a much higher rate than they request.
 *
 *******************************************************************************/

static inline int sam_reminted(struct sam_ohci_s *priv,
                                 struct sam_ed_s *ed)
{
#ifndef CONFIG_USBHOST_INT_DISABLE
  struct sam_ed_s *head;
  struct sam_ed_s *curr;
  struct sam_ed_s *prev;
  unsigned int     interval;
  unsigned int     offset;
  uint32_t         regval;

  /* Disable periodic list processing.  Does this take effect immediately?  Or
   * at the next SOF... need to check.
   */

  regval  = sam_getreg(SAM_USBHOST_CTRL);
  regval &= ~OHCI_CTRL_PLE;
  sam_putreg(regval, SAM_USBHOST_CTRL);

  /* Get the offset associated with the ED direction. IN EDs get the even
   * entries, OUT EDs get the odd entries.
   */

  if ((ed->hw.ctrl & ED_CONTROL_D_MASK) == ED_CONTROL_D_IN)
    {
      offset = 0;
    }
  else
    {
      offset = 1;
    }

  /* Get the head of the first of the duplicated entries.  The first offset
   * entry is always guaranteed to contain the common ED list head.
   */

  head = (struct sam_ed_s *)g_hcca.inttbl[offset];
  uvdbg("ed: %08x head: %08x next: %08x offset: %d\n",
        ed, head, head ? head->hw.nexted : 0, offset);

  /* Find the ED to be removed in the ED list */

  for (curr = head, prev = NULL;
       curr && curr != ed;
       prev = curr, curr = (struct sam_ed_s *)curr->hw.nexted);

  /* Hmmm.. It would be a bug if we do not find the ED in the bulk list. */

  DEBUGASSERT(curr != NULL);
  if (curr != NULL)
    {
      /* Clear all current entries in the interrupt table for this direction */

      sam_setinttab(0, 2, offset);

      /* Remove the ED from the list..  Is this ED the first on in the list? */

      if (prev == NULL)
        {
          /* Yes... set the head of the bulk list to skip over this ED */

          head = (struct sam_ed_s *)ed->hw.nexted;
        }
      else
        {
          /* No.. set the forward link of the previous ED in the list
           * skip over this ED.
           */

          prev->hw.nexted = ed->hw.nexted;
        }
        uvdbg("ed: %08x head: %08x next: %08x\n",
              ed, head, head ? head->hw.nexted : 0);

      /* Calculate the new minimum interval for this list */

      interval = MAX_PERINTERVAL;
      for (curr = head; curr; curr = (struct sam_ed_s *)curr->hw.nexted)
        {
          if (curr->interval < interval)
            {
              interval = curr->interval;
            }
        }
      uvdbg("min interval: %d offset: %d\n", interval, offset);

      /* Save the new minimum interval */

      if ((ed->hw.ctrl && ED_CONTROL_D_MASK) == ED_CONTROL_D_IN)
        {
          priv->ininterval  = interval;
        }
      else
        {
          priv->outinterval = interval;
        }

      /* Set the head ED in all of the appropriate entries of the HCCA interrupt
       * table (head might be NULL).
       */

      sam_setinttab((uint32_t)head, interval, offset);
    }

  /* Re-enabled periodic list processing */

  if (head != NULL)
    {
      regval  = sam_getreg(SAM_USBHOST_CTRL);
      regval |= OHCI_CTRL_PLE;
      sam_putreg(regval, SAM_USBHOST_CTRL);
    }

  return OK;
#else
  return -ENOSYS;
#endif
}

/*******************************************************************************
 * Name: sam_addisoced
 *
 * Description:
 *   Helper functions to add an ED to the periodic table.
 *
 *******************************************************************************/

static inline int sam_addisoced(struct sam_ohci_s *priv,
                                const FAR struct usbhost_epdesc_s *epdesc,
                                struct sam_ed_s *ed)
{
#ifndef CONFIG_USBHOST_ISOC_DISABLE
#  warning "Isochronous endpoints not yet supported"
#endif
  return -ENOSYS;

}

/*******************************************************************************
 * Name: sam_remisoced
 *
 * Description:
 *   Helper functions to remove an ED from the periodic table.
 *
 *******************************************************************************/

static inline int sam_remisoced(struct sam_ohci_s *priv,
                                struct sam_ed_s *ed)
{
#ifndef CONFIG_USBHOST_ISOC_DISABLE
#  warning "Isochronous endpoints not yet supported"
#endif
  return -ENOSYS;
}

/*******************************************************************************
 * Name: sam_enqueuetd
 *
 * Description:
 *   Enqueue a transfer descriptor.  Notice that this function only supports
 *   queue on TD per ED.
 *
 *******************************************************************************/

static int sam_enqueuetd(struct sam_ohci_s *priv,
                         struct sam_ed_s *ed, uint32_t dirpid,
                         uint32_t toggle, volatile uint8_t *buffer,
                         size_t buflen)
{
  struct sam_gtd_s *td;
  int ret = -ENOMEM;

  /* Allocate a TD from the free list */

  td = sam_tdalloc();
  if (td != NULL)
    {
      /* Initialize the allocated TD and link it before the common tail TD. */

      td->hw.ctrl        = (GTD_STATUS_R | dirpid | TD_DELAY(0) | toggle | GTD_STATUS_CC_MASK);
      g_tdtail.hw.ctrl   = 0;
      td->hw.cbp         = (uint32_t)buffer;
      g_tdtail.hw.cbp    = 0;
      td->hw.nexttd      = (uint32_t)&g_tdtail;
      g_tdtail.hw.nexttd = 0;
      td->hw.be          = (uint32_t)(buffer + (buflen - 1));
      g_tdtail.hw.be     = 0;

      /* Configure driver-only fields in the extended TD structure */

      td->ed             = ed;

      /* Link the td to the head of the ED's TD list */

      ed->hw.headp       = (uint32_t)td | ((ed->hw.headp) & ED_HEADP_C);
      ed->hw.tailp       = (uint32_t)&g_tdtail;

      ret                = OK;
    }

  return ret;
}

/*******************************************************************************
 * Name: sam_wdhwait
 *
 * Description:
 *   Set the request for the Writeback Done Head event well BEFORE enabling the
 *   transfer (as soon as we are absolutely committed to the to avoid transfer).
 *   We do this to minimize race conditions.  This logic would have to be expanded
 *   if we want to have more than one packet in flight at a time!
 *
 *******************************************************************************/

static int sam_wdhwait(struct sam_ohci_s *priv, struct sam_ed_s *ed)
{
  irqstate_t flags = irqsave();
  int        ret   = -ENODEV;

  /* Is the device still connected? */

#if 0 /* REVISIT */
  if (priv->connected)
#endif
    {
      /* Yes.. then set wdhwait to indicate that we expect to be informed when
       * either (1) the device is disconnected, or (2) the transfer completed.
       */

      ed->wdhwait = true;
      ret         = OK;
    }

  irqrestore(flags);
  return ret;
}

/*******************************************************************************
 * Name: sam_ctrltd
 *
 * Description:
 *   Process a IN or OUT request on the control endpoint.  This function
 *   will enqueue the request and wait for it to complete.  Only one transfer
 *   may be queued; Neither these methods nor the transfer() method can be
 *   called again until the control transfer functions returns.
 *
 *   These are blocking methods; these functions will not return until the
 *   control transfer has completed.
 *
 *******************************************************************************/

static int sam_ctrltd(struct sam_ohci_s *priv, uint32_t dirpid, uint8_t *buffer,
                      size_t buflen)
{
  uint32_t toggle;
  uint32_t regval;
  int ret;

  /* Set the request for the Writeback Done Head event well BEFORE enabling the
   * transfer.
   */

  ret = sam_wdhwait(priv, &g_edctrl);
  if (ret != OK)
    {
      udbg("ERROR: Device disconnected\n");
      return ret;
    }

  /* Configure the toggle field in the TD */

  if (dirpid == GTD_STATUS_DP_SETUP)
    {
      toggle = GTD_STATUS_T_DATA0;
    }
  else
    {
      toggle = GTD_STATUS_T_DATA1;
    }

  /* Then enqueue the transfer */

  g_edctrl.tdstatus = TD_CC_NOERROR;
  ret = sam_enqueuetd(priv, &g_edctrl, dirpid, toggle, buffer, buflen);
  if (ret == OK)
    {
      /* Set ControlListFilled.  This bit is used to indicate whether there are
       * TDs on the Control list.
       */

      regval = sam_getreg(SAM_USBHOST_CMDST);
      regval |= OHCI_CMDST_CLF;
      sam_putreg(regval, SAM_USBHOST_CMDST);

      /* Wait for the Writeback Done Head interrupt */

      sam_takesem(&g_edctrl.wdhsem);

      /* Check the TD completion status bits */

      if (g_edctrl.tdstatus == TD_CC_NOERROR)
        {
          ret = OK;
        }
      else
        {
          uvdbg("Bad TD completion status: %d\n", g_edctrl.tdstatus);
          ret = -EIO;
        }
    }

  /* Make sure that there is no outstanding request on this endpoint */

  g_edctrl.wdhwait = false;
  return ret;
}

/*******************************************************************************
 * Name: sam_rhsc_interrupt
 *
 * Description:
 *   OHCI root hub status change interrupt handler
 *
 *******************************************************************************/

static void sam_rhsc_interrupt(struct sam_ohci_s *priv)
{
  struct sam_rhport_s *rhport;
  uint32_t regaddr;
  uint32_t rhportst;
  int rhpndx;

  /* Handle root hub status change on each root port */

  for (rhpndx = 0; rhpndx < SAM_USBHOST_NRHPORT; rhpndx++)
    {
      rhport   = &priv->rhport[rhpndx];

      regaddr  = SAM_USBHOST_RHPORTST(rhpndx+1);
      rhportst = sam_getreg(regaddr);

      ullvdbg("RHPORTST%d: %08x\n", rhpndx + 1, rhportst);

      if ((rhportst & OHCI_RHPORTST_CSC) != 0)
        {
          uint32_t rhstatus = sam_getreg(SAM_USBHOST_RHSTATUS);
          ullvdbg("Connect Status Change, RHSTATUS: %08x\n", rhstatus);

          /* If DRWE is set, Connect Status Change indicates a remote
           * wake-up event
           */

          if (rhstatus & OHCI_RHSTATUS_DRWE)
            {
              ullvdbg("DRWE: Remote wake-up\n");
            }

          /* Otherwise... Not a remote wake-up event */

          else
            {
              /* Check current connect status */

              if ((rhportst & OHCI_RHPORTST_CCS) != 0)
                {
                  /* Connected ... Did we just become connected? */

                  if (!rhport->connected)
                    {
                      /* Yes.. connected. */

                      rhport->connected = true;

                      ullvdbg("RHPort%d connected, rhswait: %d\n",
                              rhpndx + 1, priv->rhswait);

                      /* Notify any waiters */

                      if (priv->rhswait)
                        {
                          sam_givesem(&priv->rhssem);
                          priv->rhswait = false;
                        }
                    }
                  else
                    {
                      ulldbg("Spurious status change (connected)\n");
                    }

                  /* The LSDA (Low speed device attached) bit is valid
                   * when CCS == 1.
                   */

                  rhport->lowspeed = (rhportst & OHCI_RHPORTST_LSDA) != 0;
                  ullvdbg("Speed: %s\n", rhport->lowspeed ? "LOW" : "FULL");
                }

              /* Check if we are now disconnected */

              else if (rhport->connected)
                {
                  /* Yes.. disconnect the device */

                  ullvdbg("RHport%d disconnected\n", rhpndx+1);
                  rhport->connected = false;
                  rhport->lowspeed  = false;

                  /* Are we bound to a class instance? */

                  if (rhport->class)
                    {
                      /* Yes.. Disconnect the class */

                      CLASS_DISCONNECTED(rhport->class);
                      rhport->class = NULL;
                    }

                  /* Notify any waiters for the Root Hub Status change
                   * event.
                   */

                  if (priv->rhswait)
                    {
                      sam_givesem(&priv->rhssem);
                      priv->rhswait = false;
                    }
                }
              else
                {
                   ulldbg("Spurious status change (disconnected)\n");
                }
            }

          /* Clear the status change interrupt */

          sam_putreg(OHCI_RHPORTST_CSC, regaddr);
        }

      /* Check for port reset status change */

      if ((rhportst & OHCI_RHPORTST_PRSC) != 0)
        {
          /* Release the RH port from reset */

          sam_putreg(OHCI_RHPORTST_PRSC, regaddr);
        }
    }
}

/*******************************************************************************
 * Name: sam_wdh_interrupt
 *
 * Description:
 *   OHCI write done head interrupt handler
 *
 *******************************************************************************/

static void sam_wdh_interrupt(struct sam_ohci_s *priv)
{
  struct sam_gtd_s *td;
  struct sam_gtd_s *next;

  /* The host controller just wrote the list of finished TDs into the HCCA
   * done head.  This may include multiple packets that were transferred
   * in the preceding frame.
   *
   * Remove the TD(s) from the Writeback Done Head in the HCCA and return
   * them to the free list.  Note that this is safe because the hardware
   * will not modify the writeback done head again until the WDH bit is
   * cleared in the interrupt status register.
   */

  td = (struct sam_gtd_s *)g_hcca.donehead;
  g_hcca.donehead = 0;

  /* Process each TD in the write done list */

  for (; td; td = next)
    {
      /* Get the ED in which this TD was enqueued */

      struct sam_ed_s *ed = td->ed;
      DEBUGASSERT(ed != NULL);

      /* Save the condition code from the (single) TD status/control
       * word.
       */

      ed->tdstatus = (td->hw.ctrl & GTD_STATUS_CC_MASK) >> GTD_STATUS_CC_SHIFT;

#ifdef CONFIG_DEBUG_USB
      if (ed->tdstatus != TD_CC_NOERROR)
        {
          /* The transfer failed for some reason... dump some diagnostic info. */

          ulldbg("ERROR: ED xfrtype: %d TD CTRL: %08x/CC: %d\n",
                 ed->xfrtype, td->hw.ctrl, ed->tdstatus);
        }
#endif

      /* Return the TD to the free list */

      next = (struct sam_gtd_s *)td->hw.nexttd;
      sam_tdfree(td);

      /* And wake up the thread waiting for the WDH event */

      if (ed->wdhwait)
        {
          sam_givesem(&ed->wdhsem);
          ed->wdhwait = false;
        }
    }
}

/*******************************************************************************
 * Name: sam_ohci_interrupt
 *
 * Description:
 *   OHCI interrupt handler
 *
 *******************************************************************************/

static int sam_ohci_interrupt(int irq, FAR void *context)
{
  struct sam_ohci_s *priv = &g_usbhost;
  uint32_t intst;
  uint32_t pending;
  uint32_t regval;

  /* Read Interrupt Status and mask out interrupts that are not enabled. */

  intst  = sam_getreg(SAM_USBHOST_INTST);
  regval = sam_getreg(SAM_USBHOST_INTEN);
  ullvdbg("INST: %08x INTEN: %08x\n", intst, regval);

  pending = intst & regval;
  if (pending != 0)
    {
      /* Root hub status change interrupt */

      if ((pending & OHCI_INT_RHSC) != 0)
        {
          /* Handle root hub status change on each root port */

          ullvdbg("Root Hub Status Change\n");
          sam_rhsc_interrupt(priv);
        }

      /* Writeback Done Head interrupt */

      if ((pending & OHCI_INT_WDH) != 0)
        {
          /* The host controller just wrote the list of finished TDs into the HCCA
           * done head.  This may include multiple packets that were transferred
           * in the preceding frame.
           */

          ullvdbg("Writeback Done Head interrupt\n");
          sam_wdh_interrupt(priv);
        }

#ifdef CONFIG_DEBUG_USB
      if ((pending & SAM_DEBUG_INTS) != 0)
        {
          if ((pending & OHCI_INT_UE) != 0)
            {
              /* An unrecoverable error occurred.  Unrecoverable errors
               * are usually the consequence of bad descriptor contents
               * or DMA errors.
               *
               * Treat this like a normal write done head interrupt.  We
               * just want to see if there is any status information writen
               * to the descriptors (and the normal write done head
               * interrupt will not be occurring).
               */

              ulldbg("ERROR: Unrecoverable error. INTST: %08x\n", intst);
              sam_wdh_interrupt(priv);
            }
          else
            {
              ulldbg("ERROR: Unhandled interrupts INTST: %08x\n", intst);
            }
        }
#endif

      /* Clear interrupt status register */

      sam_putreg(intst, SAM_USBHOST_INTST);
    }

  return OK;
}

/*******************************************************************************
 * USB Host Controller Operations
 *******************************************************************************/

/*******************************************************************************
 * Name: sam_wait
 *
 * Description:
 *   Wait for a device to be connected or disconnected to/from a root hub port.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call
 *      to the class create() method.
 *   connected - A pointer to an array of 3 boolean values corresponding to
 *      root hubs 1, 2, and 3.  For each boolean value: TRUE: Wait for a device
 *      to be connected on the root hub; FALSE: wait for device to be
 *      disconnected from the root hub.
 *
 * Returned Values:
 *   And index [0, 1, or 2} corresponding to the root hub port number {1, 2,
 *   or 3} is returned when a device is connected or disconnected. This
 *   function will not return until either (1) a device is connected or
 *   disconnected to/from any root hub port or until (2) some failure occurs.
 *   On a failure, a negated errno value is returned indicating the nature of
 *   the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int sam_wait(FAR struct usbhost_driver_s *drvr, FAR const bool *connected)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  irqstate_t flags;
  int rhpndx;

  /* Loop until a change in the connection state changes on one of the root hub
   * ports or until an error occurs.
   */

  flags = irqsave();
  for (;;)
    {
      /* Check for a change in the connection state on any root hub port */

      for (rhpndx = 0; rhpndx < SAM_USBHOST_NRHPORT; rhpndx++)
        {
          /* Has the connection state changed on the RH port? */

          if (priv->rhport[rhpndx].connected != connected[rhpndx])
            {
              /* Yes.. Return the RH port number */

              irqrestore(flags);

              udbg("RHPort%d connected: %s\n",
                   rhpndx + 1,
                   priv->rhport[rhpndx].connected ? "YES" : "NO");

              return rhpndx;
            }
        }

      /* No changes on any port. Wait for a connection/disconnection event
       * and check again
       */

      priv->rhswait = true;
      sam_takesem(&priv->rhssem);
    }
}

/*******************************************************************************
 * Name: sam_enumerate
 *
 * Description:
 *   Enumerate the connected device.  As part of this enumeration process,
 *   the driver will (1) get the device's configuration descriptor, (2)
 *   extract the class ID info from the configuration descriptor, (3) call
 *   usbhost_findclass() to find the class that supports this device, (4)
 *   call the create() method on the struct usbhost_registry_s interface
 *   to get a class instance, and finally (5) call the configdesc() method
 *   of the struct usbhost_class_s interface.  After that, the class is in
 *   charge of the sequence of operations.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   rphndx - Root hub port index.  0-(n-1) corresponds to root hub port 1-n.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int sam_enumerate(FAR struct usbhost_driver_s *drvr, int rhpndx)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  struct sam_rhport_s *rhport;
  uint32_t regaddr;

  DEBUGASSERT(priv && rhpndx >= 0 && rhpndx < SAM_USBHOST_NRHPORT);
  rhport = &priv->rhport[rhpndx];

  /* Are we connected to a device?  The caller should have called the wait()
   * method first to be assured that a device is connected.
   */

  while (!rhport->connected)
    {
      /* No, return an error */

      udbg("Not connected\n");
      return -ENODEV;
    }

  /* USB 2.0 spec says at least 50ms delay before port reset */

  up_mdelay(100);

  /* Put the root hub port in reset (the SAMA5 supports three downstream ports) */

  regaddr = SAM_USBHOST_RHPORTST(rhpndx+1);
  sam_putreg(OHCI_RHPORTST_PRS, regaddr);

  /* Wait for the port reset to complete */

  while ((sam_getreg(regaddr) & OHCI_RHPORTST_PRS) != 0);

  /* Release RH port 1 from reset and wait a bit */

  sam_putreg(OHCI_RHPORTST_PRSC, regaddr);
  up_mdelay(200);

  /* Let the common usbhost_enumerate do all of the real work.  Note that the
   * FunctionAddress (USB address) is set to the root hub port number for now.
   */

  uvdbg("Enumerate the device\n");
  return usbhost_enumerate(drvr, rhpndx+1, &rhport->class);
}

/************************************************************************************
 * Name: sam_ep0configure
 *
 * Description:
 *   Configure endpoint 0.  This method is normally used internally by the
 *   enumerate() method but is made available at the interface to support
 *   an external implementation of the enumeration logic.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   funcaddr - The USB address of the function containing the endpoint that EP0
 *     controls.  A funcaddr of zero will be received if no address is yet assigned
 *     to the device.
 *   maxpacketsize - The maximum number of bytes that can be sent to or
 *    received from the endpoint in a single data packet
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int sam_ep0configure(FAR struct usbhost_driver_s *drvr, uint8_t funcaddr,
                            uint16_t maxpacketsize)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  struct sam_rhport_s *rhport;

  DEBUGASSERT(priv &&
              funcaddr >= 0 && funcaddr <= SAM_USBHOST_NRHPORT &&
              maxpacketsize < 2048);

  rhport = &priv->rhport[funcaddr - 1];

  /* We must have exclusive access to EP0 and the control list */

  sam_takesem(&priv->exclsem);

  /* Set the EP0 ED control word */

  g_edctrl.hw.ctrl = (uint32_t)funcaddr << ED_CONTROL_FA_SHIFT |
                     (uint32_t)maxpacketsize << ED_CONTROL_MPS_SHIFT;

  if (rhport->lowspeed)
   {
     g_edctrl.hw.ctrl |= ED_CONTROL_S;
   }

  /* Set the transfer type to control */

  g_edctrl.xfrtype = USB_EP_ATTR_XFER_CONTROL;
  sam_givesem(&priv->exclsem);

  uvdbg("EP0 CTRL: %08x\n", g_edctrl.hw.ctrl);
  return OK;
}

/************************************************************************************
 * Name: sam_epalloc
 *
 * Description:
 *   Allocate and configure one endpoint.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   epdesc - Describes the endpoint to be allocated.
 *   ep - A memory location provided by the caller in which to receive the
 *      allocated endpoint desciptor.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int sam_epalloc(FAR struct usbhost_driver_s *drvr,
                       const FAR struct usbhost_epdesc_s *epdesc, usbhost_ep_t *ep)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  struct sam_ed_s   *ed;
  int                rhpndx = (int)epdesc->funcaddr - 1;
  int                ret  = -ENOMEM;

  /* Sanity check.  NOTE that this method should only be called if a device is
   * connected (because we need a valid low speed indication).
   */

  DEBUGASSERT(priv && epdesc && ep &&
              rhpndx >= 0 && rhpndx < SAM_USBHOST_NRHPORT &&
              priv->rhport[rhpndx].connected);

  /* We must have exclusive access to the ED pool, the bulk list, the periodic list
   * and the interrupt table.
   */

  sam_takesem(&priv->exclsem);

  /* Take the next ED from the beginning of the free list (if the list is
   * non-empty.
   */

  if (g_edfree)
    {
      /* Remove the ED from the freelist */

      ed       = (struct sam_ed_s *)g_edfree;
      g_edfree = ((struct sam_list_s*)ed)->flink;

      /* Configure the endpoint descriptor. */

      memset((void*)ed, 0, sizeof(struct sam_ed_s));
      ed->hw.ctrl = (uint32_t)(epdesc->funcaddr)     << ED_CONTROL_FA_SHIFT |
                    (uint32_t)(epdesc->addr)         << ED_CONTROL_EN_SHIFT |
                    (uint32_t)(epdesc->mxpacketsize) << ED_CONTROL_MPS_SHIFT;

      /* Get the direction of the endpoint */

      if (epdesc->in)
        {
          ed->hw.ctrl |= ED_CONTROL_D_IN;
        }
      else
        {
          ed->hw.ctrl |= ED_CONTROL_D_OUT;
        }

      /* Check for a low-speed device */

      if (priv->rhport[rhpndx].lowspeed)
        {
          ed->hw.ctrl |= ED_CONTROL_S;
        }

      /* Set the transfer type */

      ed->xfrtype = epdesc->xfrtype;

      /* Special Case isochronous transfer types */

#if 0 /* Isochronous transfers not yet supported */
      if (ed->xfrtype == USB_EP_ATTR_XFER_ISOC)
        {
          ed->hw.ctrl |= ED_CONTROL_F;
        }
#endif
      uvdbg("EP%d CTRL: %08x\n", epdesc->addr, ed->hw.ctrl);

      /* Initialize the semaphore that is used to wait for the endpoint
       * WDH event.
       */

      sem_init(&ed->wdhsem, 0, 0);

      /* Link the common tail TD to the ED's TD list */

      ed->hw.headp = (uint32_t)&g_tdtail;
      ed->hw.tailp = (uint32_t)&g_tdtail;

      /* Now add the endpoint descriptor to the appropriate list */

      switch (ed->xfrtype)
        {
        case USB_EP_ATTR_XFER_BULK:
          ret = sam_addbulked(priv, ed);
          break;

        case USB_EP_ATTR_XFER_INT:
          ret = sam_addinted(priv, epdesc, ed);
          break;

        case USB_EP_ATTR_XFER_ISOC:
          ret = sam_addisoced(priv, epdesc, ed);
          break;

        case USB_EP_ATTR_XFER_CONTROL:
        default:
          ret = -EINVAL;
          break;
        }

      /* Was the ED successfully added? */

      if (ret != OK)
        {
          /* No.. destroy it and report the error */

          udbg("ERROR: Failed to queue ED for transfer type: %d\n", ed->xfrtype);
          sem_destroy(&ed->wdhsem);
          sam_edfree(ed);
        }
      else
        {
          /* Yes.. return an opaque reference to the ED */

          *ep = (usbhost_ep_t)ed;
        }
    }

  sam_givesem(&priv->exclsem);
  return ret;
}

/************************************************************************************
 * Name: sam_epfree
 *
 * Description:
 *   Free and endpoint previously allocated by DRVR_EPALLOC.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   ep - The endpint to be freed.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int sam_epfree(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  struct sam_ed_s      *ed   = (struct sam_ed_s *)ep;
  int                     ret;

  /* There should not be any pending, real TDs linked to this ED */

  DEBUGASSERT(ed && (ed->hw.headp & ED_HEADP_ADDR_MASK) == (uint32_t)&g_tdtail);

  /* We must have exclusive access to the ED pool, the bulk list, the periodic list
   * and the interrupt table.
   */

  sam_takesem(&priv->exclsem);

  /* Remove the ED to the correct list depending on the trasfer type */

  switch (ed->xfrtype)
    {
    case USB_EP_ATTR_XFER_BULK:
      ret = sam_rembulked(priv, ed);
      break;

    case USB_EP_ATTR_XFER_INT:
      ret = sam_reminted(priv, ed);
      break;

    case USB_EP_ATTR_XFER_ISOC:
      ret = sam_remisoced(priv, ed);
      break;

    case USB_EP_ATTR_XFER_CONTROL:
    default:
      ret = -EINVAL;
      break;
    }

  /* Destroy the semaphore */

  sem_destroy(&ed->wdhsem);

  /* Put the ED back into the free list */

  sam_edfree(ed);
  sam_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: sam_alloc
 *
 * Description:
 *   Some hardware supports special memory in which request and descriptor data can
 *   be accessed more efficiently.  This method provides a mechanism to allocate
 *   the request/descriptor memory.  If the underlying hardware does not support
 *   such "special" memory, this functions may simply map to kmalloc.
 *
 *   This interface was optimized under a particular assumption.  It was assumed
 *   that the driver maintains a pool of small, pre-allocated buffers for descriptor
 *   traffic.  NOTE that size is not an input, but an output:  The size of the
 *   pre-allocated buffer is returned.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of a memory location provided by the caller in which to
 *     return the allocated buffer memory address.
 *   maxlen - The address of a memory location provided by the caller in which to
 *     return the maximum size of the allocated buffer memory.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int sam_alloc(FAR struct usbhost_driver_s *drvr,
                     FAR uint8_t **buffer, FAR size_t *maxlen)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  DEBUGASSERT(priv && buffer && maxlen);
  int ret = -ENOMEM;

  /* We must have exclusive access to the transfer buffer pool */

  sam_takesem(&priv->exclsem);

  *buffer = sam_tballoc();
  if (*buffer)
    {
      *maxlen = CONFIG_SAMA5_OHCI_TDBUFSIZE;
      ret = OK;
    }

  sam_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: sam_free
 *
 * Description:
 *   Some hardware supports special memory in which request and descriptor data
 *   can be accessed more efficiently.  This method provides a mechanism to
 *   free that request/descriptor memory.  If the underlying hardware does not
 *   support such "special" memory, this functions may simply map to kfree().
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call
 *      to the class create() method.
 *   buffer - The address of the allocated buffer memory to be freed.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int sam_free(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  DEBUGASSERT(buffer);

  /* We must have exclusive access to the transfer buffer pool */

  sam_takesem(&priv->exclsem);
  sam_tbfree(buffer);
  sam_givesem(&priv->exclsem);
  return OK;
}

/************************************************************************************
 * Name: sam_ioalloc
 *
 * Description:
 *   Some hardware supports special memory in which larger IO buffers can
 *   be accessed more efficiently.  This method provides a mechanism to allocate
 *   the request/descriptor memory.  If the underlying hardware does not support
 *   such "special" memory, this functions may simply map to kmalloc.
 *
 *   This interface differs from DRVR_ALLOC in that the buffers are variable-sized.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of a memory location provided by the caller in which to
 *     return the allocated buffer memory address.
 *   buflen - The size of the buffer required.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int sam_ioalloc(FAR struct usbhost_driver_s *drvr, FAR uint8_t **buffer,
                       size_t buflen)
{
  DEBUGASSERT(drvr && buffer);

  /* kumalloc() should return user accessible, DMA-able memory */

  *buffer = kumalloc(buflen);
  return *buffer ? OK : -ENOMEM;
}

/************************************************************************************
 * Name: sam_iofree
 *
 * Description:
 *   Some hardware supports special memory in which IO data can  be accessed more
 *   efficiently.  This method provides a mechanism to free that IO buffer
 *   memory.  If the underlying hardware does not support such "special" memory,
 *   this functions may simply map to kfree().
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   buffer - The address of the allocated buffer memory to be freed.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   This function will *not* be called from an interrupt handler.
 *
 ************************************************************************************/

static int sam_iofree(FAR struct usbhost_driver_s *drvr, FAR uint8_t *buffer)
{
  DEBUGASSERT(drvr && buffer);

  /* kufree is all that is required */

  kufree(buffer);
  return OK;
}

/*******************************************************************************
 * Name: sam_ctrlin and sam_ctrlout
 *
 * Description:
 *   Process a IN or OUT request on the control endpoint.  These methods
 *   will enqueue the request and wait for it to complete.  Only one transfer may
 *   be queued; Neither these methods nor the transfer() method can be called
 *   again until the control transfer functions returns.
 *
 *   These are blocking methods; these functions will not return until the
 *   control transfer has completed.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   req - Describes the request to be sent.  This request must lie in memory
 *      created by DRVR_ALLOC.
 *   buffer - A buffer used for sending the request and for returning any
 *     responses.  This buffer must be large enough to hold the length value
 *     in the request description. buffer must have been allocated using DRVR_ALLOC
 *
 *   NOTE: On an IN transaction, req and buffer may refer to the same allocated
 *   memory.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int sam_ctrlin(FAR struct usbhost_driver_s *drvr,
                      FAR const struct usb_ctrlreq_s *req,
                      FAR uint8_t *buffer)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  uint16_t len;
  int  ret;

  DEBUGASSERT(drvr && req);
  uvdbg("type: %02x req: %02x value: %02x%02x index: %02x%02x len: %02x%02x\n",
        req->type, req->req, req->value[1], req->value[0],
        req->index[1], req->index[0], req->len[1], req->len[0]);

  /* We must have exclusive access to EP0 and the control list */

  sam_takesem(&priv->exclsem);

  len = sam_getle16(req->len);
  ret = sam_ctrltd(priv, GTD_STATUS_DP_SETUP, (uint8_t*)req, USB_SIZEOF_CTRLREQ);
  if (ret == OK)
    {
      if (len)
        {
          ret = sam_ctrltd(priv, GTD_STATUS_DP_IN, buffer, len);
        }

      if (ret == OK)
        {
          ret = sam_ctrltd(priv, GTD_STATUS_DP_OUT, NULL, 0);
        }
    }

  sam_givesem(&priv->exclsem);
  return ret;
}

static int sam_ctrlout(FAR struct usbhost_driver_s *drvr,
                       FAR const struct usb_ctrlreq_s *req,
                       FAR const uint8_t *buffer)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  uint16_t len;
  int ret;

  DEBUGASSERT(drvr && req);
  uvdbg("type: %02x req: %02x value: %02x%02x index: %02x%02x len: %02x%02x\n",
        req->type, req->req, req->value[1], req->value[0],
        req->index[1], req->index[0], req->len[1], req->len[0]);

  /* We must have exclusive access to EP0 and the control list */

  sam_takesem(&priv->exclsem);

  len = sam_getle16(req->len);
  ret = sam_ctrltd(priv, GTD_STATUS_DP_SETUP, (uint8_t*)req, USB_SIZEOF_CTRLREQ);
  if (ret == OK)
    {
      if (len)
        {
          ret = sam_ctrltd(priv, GTD_STATUS_DP_OUT, (uint8_t*)buffer, len);
        }

      if (ret == OK)
        {
          ret = sam_ctrltd(priv, GTD_STATUS_DP_IN, NULL, 0);
        }
    }

  sam_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: sam_transfer
 *
 * Description:
 *   Process a request to handle a transfer descriptor.  This method will
 *   enqueue the transfer request and return immediately.  Only one transfer may be
 *   queued; Neither this method nor the ctrlin or ctrlout methods can be called
 *   again until this function returns.
 *
 *   This is a blocking method; this functions will not return until the
 *   transfer has completed.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *   ep - The IN or OUT endpoint descriptor for the device endpoint on which to
 *      perform the transfer.
 *   buffer - A buffer containing the data to be sent (OUT endpoint) or received
 *     (IN endpoint).  buffer must have been allocated using DRVR_ALLOC
 *   buflen - The length of the data to be sent or received.
 *
 * Returned Values:
 *   On success, zero (OK) is returned. On a failure, a negated errno value is
 *   returned indicating the nature of the failure:
 *
 *     EAGAIN - If devices NAKs the transfer (or NYET or other error where
 *              it may be appropriate to restart the entire transaction).
 *     EPERM  - If the endpoint stalls
 *     EIO    - On a TX or data toggle error
 *     EPIPE  - Overrun errors
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Called from a single thread so no mutual exclusion is required.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static int sam_transfer(FAR struct usbhost_driver_s *drvr, usbhost_ep_t ep,
                        FAR uint8_t *buffer, size_t buflen)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  struct sam_ed_s *ed = (struct sam_ed_s *)ep;
  uint32_t dirpid;
  uint32_t regval;
#if SAM_IOBUFFERS > 0
  uint8_t *origbuf = NULL;
#endif
  bool in;
  int ret;

  DEBUGASSERT(priv && ed && buffer && buflen > 0);

  in = (ed->hw.ctrl  & ED_CONTROL_D_MASK) == ED_CONTROL_D_IN;
  uvdbg("EP%d %s toggle: %d maxpacket: %d buflen: %d\n",
        (ed->hw.ctrl  & ED_CONTROL_EN_MASK) >> ED_CONTROL_EN_SHIFT,
        in ? "IN" : "OUT",
        (ed->hw.headp & ED_HEADP_C) != 0 ? 1 : 0,
        (ed->hw.ctrl  & ED_CONTROL_MPS_MASK) >> ED_CONTROL_MPS_SHIFT,
        buflen);

  /* We must have exclusive access to the endpoint, the TD pool, the I/O buffer
   * pool, the bulk and interrupt lists, and the HCCA interrupt table.
   */

  sam_takesem(&priv->exclsem);

  /* Set the request for the Writeback Done Head event well BEFORE enabling the
   * transfer.
   */

  ret = sam_wdhwait(priv, ed);
  if (ret != OK)
    {
      udbg("ERROR: Device disconnected\n");
      goto errout;
    }

  /* Get the direction of the endpoint */

  if (in)
    {
      dirpid = GTD_STATUS_DP_IN;
    }
  else
    {
      dirpid = GTD_STATUS_DP_OUT;
    }

  /* Then enqueue the transfer */

  ed->tdstatus = TD_CC_NOERROR;
  ret = sam_enqueuetd(priv, ed, dirpid, GTD_STATUS_T_TOGGLE, buffer, buflen);
  if (ret == OK)
    {
      /* BulkListFilled. This bit is used to indicate whether there are any
       * TDs on the Bulk list.
       */

      regval  = sam_getreg(SAM_USBHOST_CMDST);
      regval |= OHCI_CMDST_BLF;
      sam_putreg(regval, SAM_USBHOST_CMDST);

      /* Wait for the Writeback Done Head interrupt */

      sam_takesem(&ed->wdhsem);

      /* Check the TD completion status bits */

      if (ed->tdstatus == TD_CC_NOERROR)
        {
          ret = OK;
        }
      else
        {
          uvdbg("Bad TD completion status: %d\n", ed->tdstatus);
          ret = -EIO;
        }
    }

errout:
  /* Make sure that there is no outstanding request on this endpoint */

  ed->wdhwait = false;
  sam_givesem(&priv->exclsem);
  return ret;
}

/*******************************************************************************
 * Name: sam_disconnect
 *
 * Description:
 *   Called by the class when an error occurs and driver has been disconnected.
 *   The USB host driver should discard the handle to the class instance (it is
 *   stale) and not attempt any further interaction with the class driver instance
 *   (until a new instance is received from the create() method).  The driver
 *   should not called the class' disconnected() method.
 *
 * Input Parameters:
 *   drvr - The USB host driver instance obtained as a parameter from the call to
 *      the class create() method.
 *
 * Returned Values:
 *   None
 *
 * Assumptions:
 *   - Only a single class bound to a single device is supported.
 *   - Never called from an interrupt handler.
 *
 *******************************************************************************/

static void sam_disconnect(FAR struct usbhost_driver_s *drvr)
{
  struct sam_ohci_s *priv = (struct sam_ohci_s *)drvr;
  DEBUGASSERT(priv);

  priv->rhport[funcaddr - 1].class = NULL;
}

/*******************************************************************************
 * Initialization
 *******************************************************************************/
/*******************************************************************************
 * Name: sam_ep0init
 *
 * Description:
 *   Initialize ED for EP0, add it to the control ED list, and enable control
 *   transfers.
 *
 * Input Parameters:
 *   priv - private driver state instance.
 *
 * Returned Values:
 *   None
 *
 *******************************************************************************/

static inline void sam_ep0init(struct sam_ohci_s *priv)
{
  uint32_t regval;

  /* Set up some default values */

  (void)sam_ep0configure(&priv->drvr, 1, 8);

  /* Initialize the common tail TD. */

  memset(&g_tdtail, 0, sizeof(struct sam_gtd_s));
  g_tdtail.ed          = &g_edctrl;

  /* Link the common tail TD to the ED's TD list */

  memset(&g_edctrl, 0, sizeof(struct sam_ed_s));
  g_edctrl.hw.headp    = (uint32_t)&g_tdtail;
  g_edctrl.hw.tailp    = (uint32_t)&g_tdtail;

  /* Set the head of the control list to the EP0 ED (this would have to
   * change if we want more than on control EP queued at a time).
   */

  sam_putreg((uint32_t)&g_edctrl, SAM_USBHOST_CTRLHEADED);

  /* ControlListEnable.  This bit is set to enable the processing of the
   * Control list.  Note: once enabled, it remains enabled and we may even
   * complete list processing before we get the bit set.  We really
   * should never modify the control list while CLE is set.
   */

  regval = sam_getreg(SAM_USBHOST_CTRL);
  regval |= OHCI_CTRL_CLE;
  sam_putreg(regval, SAM_USBHOST_CTRL);
}

/*******************************************************************************
 * Public Functions
 *******************************************************************************/

/*******************************************************************************
 * Name: sam_ohci_initialize
 *
 * Description:
 *   Initialize USB OHCI host controller hardware.
 *
 * Input Parameters:
 *   controller -- If the device supports more than one OHCI interface, then
 *     this identifies which controller is being intialized.  Normally, this
 *     is just zero.
 *
 * Returned Value:
 *   And instance of the USB host interface.  The controlling task should
 *   use this interface to (1) call the wait() method to wait for a device
 *   to be connected, and (2) call the enumerate() method to bind the device
 *   to a class driver.
 *
 * Assumptions:
 * - This function should called in the initialization sequence in order
 *   to initialize the USB device functionality.
 * - Class drivers should be initialized prior to calling this function.
 *   Otherwise, there is a race condition if the device is already connected.
 *
 *******************************************************************************/

FAR struct usbhost_driver_s *sam_ohci_initialize(int controller)
{
  struct sam_ohci_s *priv = &g_usbhost;
  uint32_t regval;
  uint8_t *buffer;
  irqstate_t flags;
  int i;

  /* Sanity checks.  NOTE: If certain OS features are enabled, it may be
   * necessary to increase the size of SAM_ED/TD_SIZE in sam_ohciram.h
   */

  DEBUGASSERT(controller == 0);
  DEBUGASSERT(sizeof(struct sam_ed_s)  <= SAM_ED_SIZE);
  DEBUGASSERT(sizeof(struct sam_gtd_s) <= SAM_TD_SIZE);

  /* Initialize the state data structure */

  sem_init(&priv->rhssem,  0, 0);
  sem_init(&priv->exclsem, 0, 1);

#ifndef CONFIG_USBHOST_INT_DISABLE
  priv->ininterval  = MAX_PERINTERVAL;
  priv->outinterval = MAX_PERINTERVAL;
#endif

  /* For OHCI Full-speed operations only, the user has to perform the
   * following:
   *
   *   1) Enable UHP peripheral clock, bit (1 << AT91C_ID_UHPHS) in PMC_PCER
   *      register.
   *   2) Select PLLACK as Input clock of OHCI part, USBS bit in PMC_USB
   *      register.
   *   3) Program the OHCI clocks (UHP48M and UHP12M) with USBDIV field in
   *      PMC_USB register. USBDIV value is calculated regarding the PLLACK
   *      value and USB Full-speed accuracy.
   *   4) Enable the OHCI clocks, UHP bit in PMC_SCER register.
   *
   * Steps 1 and 4 are done here.  2 and 3 are already performed by
   * sam_clockconfig().
   */

  /* Enable UHP peripheral clocking */

  flags   = irqsave();
  sam_uhphs_enableclk();

  /* Enable OHCI clocks */

  regval = getreg32(SAM_PMC_SCER);
  regval |= PMC_UHP;
  putreg32(regval, SAM_PMC_SCER);
  irqrestore(flags);

  /* Note that no pin pinconfiguration is required.  All USB HS pins have
   * dedicated function
   */

  udbg("Initializing Host Stack\n");

  /* Initialize all the TDs, EDs and HCCA to 0 */

  memset((void*)&g_hcca,   0, sizeof(struct ohci_hcca_s));
  memset((void*)&g_tdtail, 0, sizeof(struct ohci_gtd_s));
  memset((void*)&g_edctrl, 0, sizeof(struct sam_ed_s));
  sem_init(&g_edctrl.wdhsem, 0, 0);

  /* Initialize user-configurable EDs */

  for (i = 0; i < CONFIG_SAMA5_OHCI_NEDS; i++)
    {
      /* Put the ED in a free list */

      sam_edfree(&g_edalloc[i]);
    }

  /* Initialize user-configurable TDs */

  for (i = 0; i < CONFIG_SAMA5_OHCI_NTDS; i++)
    {
      /* Put the TD in a free list */

      sam_tdfree(&g_tdalloc[i]);
    }

  /* Initialize user-configurable request/descriptor transfer buffers */

  buffer = g_bufalloc;
  for (i = 0; i < CONFIG_SAMA5_OHCI_TDBUFFERS; i++)
    {
      /* Put the TD buffer in a free list */

      sam_tbfree(buffer);
      buffer += CONFIG_SAMA5_OHCI_TDBUFSIZE;
    }

  /* Wait 50MS then perform hardware reset */

  up_mdelay(50);

  sam_putreg(0, SAM_USBHOST_CTRL);        /* Hardware reset */
  sam_putreg(0, SAM_USBHOST_CTRLHEADED);  /* Initialize control list head to Zero */
  sam_putreg(0, SAM_USBHOST_BULKHEADED);  /* Initialize bulk list head to Zero */

  /* Software reset */

  sam_putreg(OHCI_CMDST_HCR, SAM_USBHOST_CMDST);

  /* Write Fm interval (FI), largest data packet counter (FSMPS), and
   * periodic start.
   */

  sam_putreg(DEFAULT_FMINTERVAL, SAM_USBHOST_FMINT);
  sam_putreg(DEFAULT_PERSTART, SAM_USBHOST_PERSTART);

  /* Put HC in operational state */

  regval  = sam_getreg(SAM_USBHOST_CTRL);
  regval &= ~OHCI_CTRL_HCFS_MASK;
  regval |= OHCI_CTRL_HCFS_OPER;
  sam_putreg(regval, SAM_USBHOST_CTRL);

  /* Set global power in HcRhStatus */

  sam_putreg(OHCI_RHSTATUS_SGP, SAM_USBHOST_RHSTATUS);

  /* Set HCCA base address */

  sam_putreg((uint32_t)&g_hcca, SAM_USBHOST_HCCA);

  /* Set up EP0 */

  sam_ep0init(priv);

  /* Clear pending interrupts */

  regval = sam_getreg(SAM_USBHOST_INTST);
  sam_putreg(regval, SAM_USBHOST_INTST);

  /* Enable OHCI interrupts */

  sam_putreg((SAM_ALL_INTS|OHCI_INT_MIE), SAM_USBHOST_INTEN);

  /* Attach USB host controller interrupt handler */

  if (irq_attach(SAM_IRQ_UHPHS, sam_ohci_interrupt) != 0)
    {
      udbg("Failed to attach IRQ\n");
      return NULL;
    }

  /* If there is a USB device in the slot at power up, then we will not
   * get the status change interrupt to signal us that the device is
   * connected.  We need to set the initial connected state accordingly.
   */

  for (i = 0; i < SAM_USBHOST_NRHPORT; i++)
    {
      regval                    = sam_getreg(SAM_USBHOST_RHPORTST(i));
      priv->rhport[i].connected = ((regval & OHCI_RHPORTST_CCS) != 0);

      uvdbg("RHPort%d Device connected: %s\n",
            i+1, priv->rhport[i].connected ? "YES" : "NO");
    }

  /* Drive Vbus +5V (the smoke test).  Should be done elsewhere in OTG
   * mode.
   */

  sam_usbhost_vbusdrive(SAM_OHCI_IFACE, true);

  /* Enable interrupts at the interrupt controller */

  up_enable_irq(SAM_IRQ_UHPHS); /* enable USB interrupt */
  uvdbg("USB OHCI Initialized\n");

  return &priv->drvr;
}
