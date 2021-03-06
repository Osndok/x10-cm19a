/* Copyright (C) 2005-2006 Michael LeMay
 * All rights reserved
 * 
 * Licensed under the Academic Free License version 2.1
 *
 * Special thanks to Neil Cherry of the Linux-HA project for giving me some code to jump off of (http://mywebpages.comcast.net/ncherry/common/cm19a/cm19a.html)
 *
 * This driver was based on usb-skeleton.c, and misc/legousbtower.c.
 *
 * Communications driver for X10 CM19A RF home automation transceiver
 *
 * Supported devices:
 * - X10 CM19A
 */

#ifndef X10_CM19A_C
#define X10_CM19A_C

#include <linux/config.h>
#include <linux/version.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/pagemap.h>
#include <linux/usb.h>
#include <asm/io.h>
#include <linux/smp_lock.h>
#include <asm/semaphore.h>
#include <asm/page.h>
#include <asm/uaccess.h>
#include <linux/poll.h>

typedef int bool;

/* Debugging levels */
#define DBG_ERROR 0
#define DBG_WARN  1
#define DBG_INFO  2
#define DBG_MINOR 3

/* Maximum debugging level to actually display */
#define DBG_MAX DBG_MINOR

#define MSG_PFX "(USB X10 CM19A) "
#define debug(sev, format, args...) \
  do { if (sev <= DBG_MAX) printk(KERN_DEBUG MSG_PFX format "\n", ## args); } while (0)

/**
 * Contains data structures related to X10 CM19A RF transceiver
 */
struct x10_cm19a_device {
  /** Synchronizes access to structure */
  struct semaphore mutex;

  /** Device minor number */
  int minor;

  /** Low-level USB device */
  struct usb_device * usbDev;
  /** Low-level USB interface */
  struct usb_interface * usbIntf;

  char * readBuf;
  /** Read buffer length */
  size_t readBufLen;
  /** Amount that is available for immediate return */
  size_t readPktLen;
  /** Read buffer lock */
  spinlock_t readBufLock;
  /** Packet timeout jiffies */
  int pktTimeoutJiffies;
  /** Last arrival time for read operation */
  unsigned long	readLastArrival;

  /** Address of interrupt input endpoint */
  __u8 intrInEpAddr;
  /** Interrupt input URB */
  struct urb * intrInUrb;
  /** Interrupt input interval */
  int intrInInterval;
  /** Interrupt input currently connected */
  int intrInConnected;
  /** Interrupt input completed */
  int intrInDone;
  /** Queue lock for interrupt in transfers */
  wait_queue_head_t intrInWait;
  /** USB input buffer */
  unsigned char * intrInBuf;
  /** Size of input buffer */
  size_t intrInSz;
  /** Amount of data read */
  size_t intrInCnt;
  /** Spinlock */
  spinlock_t intrInLock;
  /** Packet timeout */
  int intrInTimeoutJiffies;
  /** Time of arrival for last packet read */
  unsigned long intrInLastArrival;

  /** Address of interrupt output endpoint */
  __u8 intrOutEpAddr;
  /** Interrupt output URB */
  struct urb * intrOutUrb;
  /** Queue lock for interrupt out transfers */
  wait_queue_head_t intrOutWait;
  /** Interrupt output buffer */
  unsigned char * intrOutBuf;
  /** Interrupt output interval */
  int intrOutInterval;
  /** Interrupt output busy */
  int intrOutBusy;

  /** Kernel reference */
  struct kref kernRef;

  /** Driver loaded */
  int present;

  /** Device file opened */
  int devfOpened;

  /** Index in device array */
  int idx;
};

/** Locate structure containing cdev */
#define find_x10_cm19a_device(kref) container_of(kref, struct x10_cm19a_device, kernRef)

#define DEV_LABEL "X10 CM19A USB RF Transceiver"
#define SHORT_DEV_LABEL "x10-cm19a"

#define DEF_MAX_TSCVR_CNT 4
#define MAX_MAX_TSCVR_CNT 256

/** Maximum number of simultaneously-connected transceivers supported by driver */
static int MaxTscvrCnt = DEF_MAX_TSCVR_CNT;

/** Size of read buffer */
static int ReadBufSz = 480;

/** Size of write buffer */
static int WriteBufSz = 480;

/** Read timeout */
static int ReadTimeout = 0;

/** Read packet timeout */
static int PktTimeout = 50;

static struct x10_cm19a_device * Tscvrs = NULL;

MODULE_PARM(ReadTimeout, "i");
MODULE_PARM_DESC(ReadTimeout, "Number of jiffies to wait while reading commands from transceiver.  Default is 0, no timeout.");

MODULE_PARM(MaxTscvrCnt, "i");
MODULE_PARM_DESC(MaxTscvrCnt, "Maximum number of simultaneously connected transceivers to support.  Default is 4.");

static struct usb_driver X10Cm19ADriver;

/* prevent races between devf_open() and disconnect */
static DECLARE_MUTEX(disconnMutex);

/**
 * Initialize a device structure
 * (do not call this directly, simply retrieve structure with find_free_dev_struct)
 */
void init_x10_cm19a_device (struct x10_cm19a_device * toInit) {
  memset(toInit, 0, sizeof(*toInit));

  kref_init(&toInit->kernRef);

  init_MUTEX(&toInit->mutex);

  spin_lock_init(&toInit->readBufLock);
  toInit->pktTimeoutJiffies = PktTimeout * HZ / 1000;
  toInit->readLastArrival = jiffies;
  
  // Setup read structures:
  toInit->readBuf = kmalloc(ReadBufSz, GFP_KERNEL);
  if (!toInit->readBuf) {
    debug(DBG_ERROR, "Couldn't allocate read_buffer");
    goto error_read_buf;
  }
  toInit->readBufLen = 0;
  toInit->readPktLen = 0;

  // Setup in structures:
  toInit->intrInBuf = NULL;
  toInit->intrInEpAddr = 0;
  toInit->intrInConnected = 0;
  toInit->intrInDone = 0;
  init_waitqueue_head(&toInit->intrInWait);
  
  toInit->intrInUrb = usb_alloc_urb(0, GFP_KERNEL);
  if (!toInit->intrInUrb) {
    debug(DBG_ERROR, "Couldn't allocate intrInUrb");
    goto error_intr_in_urb;
  }

  // Setup out structures:
  toInit->intrOutBuf = kmalloc (WriteBufSz, GFP_KERNEL);
  if (!toInit->intrOutBuf) {
    debug(DBG_ERROR, "Couldn't allocate interrupt_out_buffer");
    goto error_intr_out_buf;
  }
  toInit->intrOutEpAddr = 0;
  toInit->intrOutBusy = 0;
  init_waitqueue_head(&toInit->intrOutWait);
  
  toInit->intrOutUrb = usb_alloc_urb(0, GFP_KERNEL);
  if (!toInit->intrOutUrb) {
    debug(DBG_ERROR, "Couldn't allocate interrupt_out_urb");
    goto error_intr_out_urb;
  }

  toInit->present = 1;

  return;

 error_intr_out_urb:
  kfree(toInit->intrOutBuf);
 error_intr_out_buf:
  usb_free_urb(toInit->intrInUrb);
 error_intr_in_urb:
  kfree(toInit->readBuf);
 error_read_buf:
  /* nothing to clean up */
  return;
}

/**
 * Locate first available structure in Tscvrs
 */
static struct x10_cm19a_device * find_free_dev_struct (void) {
  int i = 0;
  while ((i < MaxTscvrCnt) && Tscvrs[i].present) i++;

  if (i < MaxTscvrCnt) {
    init_x10_cm19a_device(&Tscvrs[i]);

    return &Tscvrs[i];
  } else {
    return NULL;
  }
}

/**
 * Abort any currently running transfers associated with structure
 */
void x10_cm19a_device_abort_xfers (struct x10_cm19a_device * toStop) {
  if (toStop->intrInConnected) {
    toStop->intrInConnected = 0;
    mb();
    if (toStop->intrInUrb && toStop->usbDev) {
      usb_kill_urb(toStop->intrInUrb);
    }
  }
  if (toStop->intrOutBusy) {
    if (toStop->intrOutUrb && toStop->usbDev) {
      usb_kill_urb(toStop->intrOutUrb);
    }
  }
}

/**
 * Uninitialize a device structure
 */
void uninit_x10_cm19a_device (struct x10_cm19a_device * toFree) {
  x10_cm19a_device_abort_xfers(toFree);

  kfree(toFree->readBuf);
  if (toFree->intrInBuf)
    kfree(toFree->intrInBuf);
  kfree(toFree->intrOutBuf);

  usb_free_urb(toFree->intrInUrb);
  usb_free_urb(toFree->intrOutUrb);

  usb_put_dev(toFree->usbDev);

  toFree->present = 0;
}

/**
 * Release the device structure associated with a particular kernel reference
 */
void release_x10_cm19a_device (struct kref * kernRef) {
  struct x10_cm19a_device * toFree = find_x10_cm19a_device(kernRef);

  uninit_x10_cm19a_device(toFree);
}

/* base minor number */
#define MINOR_BASE 77

/* Initialize CM19A so it recognizes signals from the CR12A or CR14A remote */
static int init_remotes (struct file * devFile, struct x10_cm19a_device * x10Dev);

/* Device file support */
static int devf_install_dev (struct x10_cm19a_device *);
static int devf_remove_dev (struct x10_cm19a_device *);
static ssize_t devf_read (struct file *, char __user *, size_t, loff_t *);
static ssize_t devf_write (struct file *, const char __user *, size_t, loff_t *);
static int devf_readdir (struct file *, void *, filldir_t);
static unsigned int devf_poll (struct file *, struct poll_table_struct *);
static int devf_open (struct inode *, struct file *);
static int devf_release (struct inode *, struct file *);

static struct file_operations FileOps = {
  .read    = devf_read,
  .write   = devf_write,
  .readdir = devf_readdir,
  .poll    = devf_poll,
  .open    = devf_open,
  .release = devf_release
};

/** Recognized devices */
static __devinitdata struct usb_device_id DeviceTab[] = {
  { USB_DEVICE(0x0bc7, 0x0002) }, /* X10 CM19A */
  {}
};
MODULE_DEVICE_TABLE(usb, DeviceTab);

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with devfs and the driver core
 */
static struct usb_class_driver X10Cm19ACls = {
        .name =         "cm19a%d",
        .fops =         &FileOps,
        .minor_base =   MINOR_BASE
};

static int x10_cm19a_probe (struct usb_interface * intf, const struct usb_device_id * id) {
  static const int DESIRED_INTF = 0; /* desired interface */
  
  int i;
  int initRes, devfInstRet;

  struct usb_host_interface * hostIntf;
  struct usb_endpoint_descriptor * endpoint;

  struct usb_interface_descriptor * intfDesc = &intf->cur_altsetting->desc;
  struct x10_cm19a_device * x10Dev;

  if (intfDesc->bInterfaceNumber != DESIRED_INTF) {
    // Only interested in interface #1
    debug(DBG_MINOR, "Skipped unused interface on " DEV_LABEL ".");

    return -EIO;
  } else {
    debug(DBG_INFO, "Probing " SHORT_DEV_LABEL "...");
  }

  x10Dev = find_free_dev_struct();
  if (x10Dev == NULL) {
    debug(DBG_ERROR, "More than %d transceivers attached, increase MaxTscvrCnt.", MaxTscvrCnt);
    goto handleErr;
  }

  x10Dev->usbDev = usb_get_dev(interface_to_usbdev(intf));
  x10Dev->usbIntf = intf;

  /* set up the endpoint information */
  /* X10 devices have only one interrupt input and output endpoint */
  hostIntf = intf->cur_altsetting;
  for (i = 0; i < hostIntf->desc.bNumEndpoints; i++) {
    endpoint = &hostIntf->endpoint[i].desc;
    
    if (!x10Dev->intrInEpAddr &&
	((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN) &&
	((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)) {
      /* we found the interrupt in endpoint */

      x10Dev->intrInInterval = endpoint->bInterval;
      /* setup interrupt input URBs */

      x10Dev->intrInSz = le16_to_cpu(endpoint->wMaxPacketSize);
      x10Dev->intrInEpAddr = endpoint->bEndpointAddress;
      x10Dev->intrInBuf = kmalloc(x10Dev->intrInSz, GFP_KERNEL);
      if (!x10Dev->intrInBuf) {
	debug(DBG_ERROR, "Could not allocate intrInBuf");
	goto handleErr;
      }
    }
    
    if (!x10Dev->intrOutEpAddr &&
	((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT) &&
	((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_INT)) {
      /* we found an interrupt out endpoint */
      x10Dev->intrOutEpAddr = endpoint->bEndpointAddress;
      x10Dev->intrOutInterval = endpoint->bInterval;
    }
  }
  if (!x10Dev->intrInEpAddr || !x10Dev->intrOutEpAddr) {
    debug(DBG_ERROR, "Could not find both interrupt-in and interrupt-out endpoints");
    goto handleErr;
  } else {
    debug(DBG_ERROR, "Found input endpoint: %X, and output endpoint: %X", x10Dev->intrInEpAddr, x10Dev->intrOutEpAddr);
  }

  initRes = init_remotes(NULL, x10Dev);
  if (initRes < 0) goto handleErr;
    
  devfInstRet = devf_install_dev(x10Dev);
  if (0 <= devfInstRet) {
    x10Dev->present = 1;

    usb_set_intfdata(intf, x10Dev);

    debug(DBG_INFO, DEV_LABEL " connected.");

    debug(DBG_INFO, DEV_LABEL " initialized to listen for CR12A and CR14A remote controls.");

    return 0;
  }

 handleErr:
  if (x10Dev) {
    kref_put(&x10Dev->kernRef, release_x10_cm19a_device);
  }

  return -EIO;
}

static void x10_cm19a_disconn (struct usb_interface * intf) {
  struct x10_cm19a_device * x10Dev;
  int minor;
  
  x10Dev = (struct x10_cm19a_device *)usb_get_intfdata(intf);
  if (!x10Dev) {
    debug(DBG_ERROR, "x10_cm19a_disconn: Device structure missing!");
    return;
  }

  down(&disconnMutex);
  
  usb_set_intfdata(intf, NULL);
  
  down(&x10Dev->mutex);

  minor = x10Dev->minor;
  
  /* give back our minor */
  usb_deregister_dev(intf, &X10Cm19ACls);
  
  up (&disconnMutex);
  
  debug(DBG_INFO, DEV_LABEL " #%d now disconnected", (minor - MINOR_BASE));
  
  usb_set_intfdata(x10Dev->usbIntf, NULL);
  
  devf_remove_dev(x10Dev);

  kref_put(&x10Dev->kernRef, release_x10_cm19a_device);

  debug(DBG_INFO, DEV_LABEL " #%d disconnected.\n", x10Dev->usbIntf->minor);
}

/** USB device driver information */
static struct usb_driver X10Cm19ADriver = {
  .name       = SHORT_DEV_LABEL,
  .id_table   = DeviceTab,
  .probe      = x10_cm19a_probe,
  .disconnect = x10_cm19a_disconn,
};

int __init x10_cm19a_init (void) {
  int i;
  int usbRegRet;

  debug(DBG_INFO, "Installing " DEV_LABEL "...");

  // Initialize storage for individual devices:
  if (MAX_MAX_TSCVR_CNT < MaxTscvrCnt) {
    debug(DBG_ERROR, "Maximum number of devices supported: %d", MAX_MAX_TSCVR_CNT);
    return -1;
  }
  Tscvrs = (struct x10_cm19a_device *)kmalloc(sizeof(struct x10_cm19a_device) * MaxTscvrCnt, GFP_KERNEL);
  if (Tscvrs == NULL) {
    debug(DBG_ERROR, "Failed to allocated device structures");
    return -1;
  }
  for (i = 0; i < MaxTscvrCnt; i++) {
    Tscvrs[i].present = 0;
    Tscvrs[i].idx = i;
  }

  // Register USB device:
  usbRegRet = usb_register(&X10Cm19ADriver);
  if (usbRegRet) {
    debug(DBG_ERROR, "USB registration failed: %d", usbRegRet);
    return usbRegRet;
  }

  debug(DBG_INFO, "Installed USB driver with support for up to %d devices.", MaxTscvrCnt);

  return 0;
}

void __exit x10_cm19a_exit (void) {
  usb_deregister(&X10Cm19ADriver);

  debug(DBG_INFO, "Removed USB driver.");
}

module_init(x10_cm19a_init);
module_exit(x10_cm19a_exit);

MODULE_LICENSE("AFL-2.1");
MODULE_AUTHOR("Michael LeMay (http://lemaymd.com)");
MODULE_DESCRIPTION("X10 CM19A Transceiver Driver");

/******************************/
/*** USB SUPPORT OPERATIONS ***/
/******************************/

/**
 * Wait for read buffer to fill or have sat idle for long enough
 */
static void check_read_buffer (struct x10_cm19a_device * x10Dev) {
  spin_lock_irq(&x10Dev->readBufLock);
  if (!PktTimeout
      || time_after(jiffies, x10Dev->readLastArrival + x10Dev->pktTimeoutJiffies)
      || (x10Dev->readBufLen == ReadBufSz)) {
    x10Dev->readPktLen = x10Dev->readBufLen;
  }
  x10Dev->intrInDone = 0;
  spin_unlock_irq(&x10Dev->readBufLock);
}

static void read_intr_cb (struct urb * urb, struct pt_regs * regs) {
  struct x10_cm19a_device * x10Dev = (struct x10_cm19a_device *)urb->context;
  int result;

  if (urb->status) {
    if (urb->status == -ENOENT ||
	urb->status == -ECONNRESET ||
	urb->status == -ESHUTDOWN) {
      goto exit;
    } else {
      debug(DBG_WARN, "%s: nonzero status received: %d\n", __FUNCTION__, urb->status);
      goto resubmit; /* maybe we can recover */
    }
  }
  
  if (urb->actual_length > 0) {
    spin_lock(&x10Dev->readBufLock);
    if (x10Dev->readBufLen + urb->actual_length < ReadBufSz) {
      memcpy (x10Dev->readBuf + x10Dev->readBufLen,
	      x10Dev->intrInBuf,
	      urb->actual_length);
      x10Dev->readBufLen += urb->actual_length;
      x10Dev->readLastArrival = jiffies;
    } else {
      debug(DBG_WARN, "%s: read_buffer overflow, %d bytes dropped", __FUNCTION__, urb->actual_length);
    }
    spin_unlock(&x10Dev->readBufLock);
  }
  
 resubmit:
  /* resubmit if we're still running */
  if (x10Dev->intrInConnected && x10Dev->usbDev) {
    result = usb_submit_urb(x10Dev->intrInUrb, GFP_ATOMIC);
    if (result) {
      debug(DBG_ERROR, "%s: usb_submit_urb failed (%d)", __FUNCTION__, result);
    }
  }
  
 exit:
  x10Dev->intrInDone = 1;
  wake_up_interruptible(&x10Dev->intrInWait);
}

/**
 * Read data from the USB device's interrupt endpoint
 */
static ssize_t read_intr_data (struct file * devFile, struct x10_cm19a_device * x10Dev, size_t readLen) {
  size_t bytesToRead;
  int result = 0;
  unsigned long timeout = 0;
  
  /* lock this object */
  if (down_interruptible(&x10Dev->mutex)) {
    result = -ERESTARTSYS;
    goto exit;
  }
  
  /* verify that the device wasn't unplugged */
  if (x10Dev->usbDev == NULL) {
    result = -ENODEV;
    debug(DBG_ERROR, "No device or device unplugged: %d", result);
    goto unlock_exit;
  }
  
  /* verify that we actually have some data to read */
  if (readLen == 0) {
    goto unlock_exit;
  }
  
  if (ReadTimeout) {
    timeout = jiffies + ReadTimeout * HZ / 1000;
  }
  
  /* wait for data */
  check_read_buffer(x10Dev);
  while (x10Dev->readPktLen == 0) {
    if (devFile->f_flags & O_NONBLOCK) {
      result = -EAGAIN;
      goto unlock_exit;
    }
    result = wait_event_interruptible_timeout(x10Dev->intrInWait, x10Dev->intrInDone, x10Dev->pktTimeoutJiffies);
    if (result < 0) {
      goto unlock_exit;
    }
    
    /* reset read timeout during read or write activity */
    if (ReadTimeout
	&& (x10Dev->readBufLen || x10Dev->intrOutBusy)) {
      timeout = jiffies + ReadTimeout * HZ / 1000;
    }
    /* check for read timeout */
    if (ReadTimeout && time_after(jiffies, timeout)) {
      result = -ETIMEDOUT;
      goto unlock_exit;
    }
    check_read_buffer(x10Dev);
  }
  
  /* copy the data from read_buffer into userspace */
  bytesToRead = min(readLen, x10Dev->readPktLen);
  
  spin_lock_irq (&x10Dev->readBufLock);
  x10Dev->readBufLen -= bytesToRead;
  x10Dev->readPktLen -= bytesToRead;
  //for (i = 0; i < x10Dev->readBufLen; i++) {
  //  x10Dev->readBuf[i] = x10Dev->readBuf[i+bytesToRead];
  //}
  memmove(x10Dev->readBuf, x10Dev->readBuf+bytesToRead, x10Dev->readBufLen);
  spin_unlock_irq(&x10Dev->readBufLock);
  
  result = bytesToRead;
  
 unlock_exit:
  /* unlock the device */
  up(&x10Dev->mutex);
  
 exit:
  return result;
}

static void write_intr_cb (struct urb *urb, struct pt_regs *regs)
{
  struct x10_cm19a_device * x10Dev = (struct x10_cm19a_device *)urb->context;
  
  /* sync/async unlink faults aren't errors */
  if (urb->status && 
      !(urb->status == -ENOENT || 
	urb->status == -ECONNRESET ||
	urb->status == -ESHUTDOWN)) {
    dbg("%s - nonzero write interrupt status received: %d",
	__FUNCTION__, urb->status);
  }

  x10Dev->intrOutBusy = 0;
  wake_up_interruptible(&x10Dev->intrOutWait);
  
  /* free up our allocated buffer */
  usb_buffer_free(urb->dev, urb->transfer_buffer_length, 
		  urb->transfer_buffer, urb->transfer_dma);
}

/** Write raw data to USB device */
static int write_intr_data (struct file * devFile, struct x10_cm19a_device * x10Dev, unsigned char * inpBuf, ssize_t bufLen) {
  size_t bytesToWrite;
  int result = 0;

  /* lock this object */
  if (down_interruptible (&x10Dev->mutex)) {
    result = -ERESTARTSYS;
    goto exit;
  }
  
  /* verify that the device wasn't unplugged */
  if (x10Dev->usbDev == NULL) {
    result = -ENODEV;
    debug(DBG_ERROR, "USB device unplugged, unable to write\n");
    goto unlock_exit;
  }
  
  /* verify that we actually have some data to write */
  if (bufLen == 0) {
    dbg(1, "%s: write request of 0 bytes", __FUNCTION__);
    goto unlock_exit;
  }
  
  /* wait until previous transfer is finished */
  while (x10Dev->intrOutBusy) {
    if (devFile && (devFile->f_flags & O_NONBLOCK)) {
      result = -EAGAIN;
      goto unlock_exit;
    }
    result = wait_event_interruptible(x10Dev->intrOutWait, !x10Dev->intrOutBusy);
    if (result) {
      goto unlock_exit;
    }
  }
  
  /* write the data into interrupt_out_buffer from userspace */
  bytesToWrite = min_t(int, bufLen, WriteBufSz);
  dbg(4, "%s: count = %Zd, bytes_to_write = %Zd", __FUNCTION__, count, bytes_to_write);
  
  if (!memcpy(x10Dev->intrOutBuf, inpBuf, bytesToWrite)) {
    result = -EFAULT;
    goto unlock_exit;
  }
  
  /* send off the urb */
  usb_fill_int_urb(x10Dev->intrOutUrb,
		   x10Dev->usbDev,
		   usb_sndintpipe(x10Dev->usbDev, x10Dev->intrOutEpAddr),
		   x10Dev->intrOutBuf,
		   bytesToWrite,
		   write_intr_cb,
		   x10Dev,
		   x10Dev->intrOutInterval);
  
  x10Dev->intrOutBusy = 1;
  wmb();
  
  result = usb_submit_urb (x10Dev->intrOutUrb, GFP_KERNEL);
  if (result) {
    x10Dev->intrOutBusy = 0;
    err("Couldn't submit interrupt_out_urb %d", result);
    goto unlock_exit;
  }
  result = bytesToWrite;
  
 unlock_exit:
  /* unlock the device */
  up (&x10Dev->mutex);
  
 exit:
  return result;
}

/**********************/
/*** X10 OPERATIONS ***/
/**********************/

/* Bounds of X10 house codes */
#define HOUSE_MIN 'a'
#define HOUSE_MAX 'p'

/* Bounds of X10 unit codes */
#define UNIT_MIN 1
#define UNIT_MAX 16

/**
 * House codes
 */
enum HouseCode {
  HOUSE_A = 0x60, HOUSE_B = 0x70, HOUSE_C = 0x40, HOUSE_D = 0x50,
  HOUSE_E = 0x80, HOUSE_F = 0x90, HOUSE_G = 0xA0, HOUSE_H = 0xB0,
  HOUSE_I = 0xE0, HOUSE_J = 0xF0, HOUSE_K = 0xC0, HOUSE_L = 0xD0,
  HOUSE_M = 0x00, HOUSE_N = 0x10, HOUSE_O = 0x20, HOUSE_P = 0x30
};

static char house_code_to_char (enum HouseCode code) {
  switch (code) {
  case HOUSE_A: return 'a';
  case HOUSE_B: return 'b';
  case HOUSE_C: return 'c';
  case HOUSE_D: return 'd';
  case HOUSE_E: return 'e';
  case HOUSE_F: return 'f';
  case HOUSE_G: return 'g';
  case HOUSE_H: return 'h';
  case HOUSE_I: return 'i';
  case HOUSE_J: return 'j';
  case HOUSE_K: return 'k';
  case HOUSE_L: return 'l';
  case HOUSE_M: return 'm';
  case HOUSE_N: return 'n';
  case HOUSE_O: return 'o';
  case HOUSE_P: return 'p';
  default: return '?';
  }
}

static enum HouseCode HouseCodeLut[(HOUSE_MAX-HOUSE_MIN)+1] = {
  HOUSE_A, HOUSE_B, HOUSE_C, HOUSE_D, HOUSE_E, HOUSE_F, HOUSE_G, HOUSE_H,
  HOUSE_I, HOUSE_J, HOUSE_K, HOUSE_L, HOUSE_M, HOUSE_N, HOUSE_O, HOUSE_P
};

/**
 * Translate house code to octet necessary for 2nd byte in Pan'n'Tilt commands
 */
static u_int8_t house_code_to_cam_code (enum HouseCode code) {
  switch (code) {
  case HOUSE_A: return 0x90;
  case HOUSE_B: return 0xA0;
  case HOUSE_C: return 0x70;
  case HOUSE_D: return 0x80;
  case HOUSE_E: return 0xB0;
  case HOUSE_F: return 0xC0;
  case HOUSE_G: return 0xD0;
  case HOUSE_H: return 0xE0;
  case HOUSE_I: return 0x10;
  case HOUSE_J: return 0x20;
  case HOUSE_K: return 0xF0;
  case HOUSE_L: return 0x00;
  case HOUSE_M: return 0x30;
  case HOUSE_N: return 0x40;
  case HOUSE_O: return 0x50;
  case HOUSE_P: return 0x60;
  default: return 0x90; /* default to A if bad housecode */
  }
}

/**
 * Unit codes
 */
enum UnitCode {
  UNIT_1  = 0x000, UNIT_2  = 0x010, UNIT_3  = 0x008, UNIT_4  = 0x018,
  UNIT_5  = 0x040, UNIT_6  = 0x050, UNIT_7  = 0x048, UNIT_8  = 0x058,
  UNIT_9  = 0x400, UNIT_10 = 0x410, UNIT_11 = 0x408, UNIT_12 = 0x418,
  UNIT_13 = 0x440, UNIT_14 = 0x450, UNIT_15 = 0x448, UNIT_16 = 0x458
};

static enum UnitCode UnitCodeLut[(UNIT_MAX-UNIT_MIN)+1] = {
  UNIT_1, UNIT_2,  UNIT_3,  UNIT_4,  UNIT_5,  UNIT_6,  UNIT_7,  UNIT_8,
  UNIT_9, UNIT_10, UNIT_11, UNIT_12, UNIT_13, UNIT_14, UNIT_15, UNIT_16
};

static int unit_code_to_int (enum UnitCode code) {
  int unit = ((code << 1) & 0x8);
  unit |= ((code >> 4) & 0x4);
  unit |= ((code >> 2) & 0x2);
  unit |= ((code >> 4) & 0x1);

  return ++unit;
}

/**
 * Command codes
 */
enum CmdCode {
  /* Standard 5-byte commands: */
  CMD_ON      = 0x00,  /* Turn on unit */
  CMD_OFF     = 0x20,  /* Turn off unit */
  CMD_DIM     = 0x98,  /* Dim lamp */
  CMD_BRIGHT  = 0x88,  /* Brighten lamp */
  /* Pan'n'Tilt 4-byte commands: */
  CMD_UP      = 0x762,
  CMD_RIGHT   = 0x661,
  CMD_DOWN    = 0x863,
  CMD_LEFT    = 0x560,
  /* Error flag */
  CMD_INVALID = 0xFF
};

static enum CmdCode parse_cmd_code (char c) {
  switch (c) {
  case '+': return CMD_ON;
  case '-': return CMD_OFF;
  case 'u': return CMD_UP;
  case 'd': return CMD_DOWN;
  case 'l': return CMD_LEFT;
  case 'r': return CMD_RIGHT;
  case 'b': return CMD_BRIGHT;
  case 's': return CMD_DIM;
  default:
    debug(DBG_ERROR, "Invalid command code: %c", c);
    return CMD_INVALID;
  }
}

static char cmd_code_to_char (enum CmdCode code) {
  switch (code) {
  case CMD_ON: return '+';
  case CMD_OFF: return '-';
  case CMD_DIM: return 's';
  case CMD_BRIGHT: return 'b';
  case CMD_UP: return 'u';
  case CMD_RIGHT: return 'r';
  case CMD_DOWN: return 'd';
  case CMD_LEFT: return 'l';
  default: return '?';
  }
}

static const char * cmd_code_to_str (enum CmdCode code) {
  switch (code) {
  case CMD_ON: return "on";
  case CMD_OFF: return "off";
  case CMD_DIM: return "dim";
  case CMD_BRIGHT: return "brighten";
  case CMD_UP: return "up";
  case CMD_RIGHT: return "right";
  case CMD_DOWN: return "down";
  case CMD_LEFT: return "left";
  default: return "invalid";
  }
}

#define IS_CAM_CODE(code) ((code) & ~0xFF)

/** Normal command length */
#define NORM_CMD_LEN 5
/** Pan'n'Tilt command length */
#define CAM_CMD_LEN 4
/** Larger of the two lengths, used for allocating buffers */
#define MAX_CMD_LEN NORM_CMD_LEN

/** Prefix for all normal commands */
#define NORM_CMD_PFX 0x20
/** Prefix for all Pan'n'Tilt commands */
#define CAM_CMD_PFX 0x14

#define ACK_LEN 1
#define ACK 0xFF

/**
 * X10 home automation command structure
 */
struct x10_ha_command {
  /** Command code */
  enum CmdCode cmd;
  /** House code */
  enum HouseCode house;
  /** Unit code, 1-16 (not used for dim/bright or camera commands) */
  enum UnitCode unit;
};

#define TO_LOWER(ch) (((ch) < 'a')? (((ch) - 'A') + 'a') : (ch))
#define OUT_OF_BOUNDS(val, low, high) (((val) < (low)) || ((high) < (val)))

/**
 * Construct HA command from parameters
 */
struct x10_ha_command * new_x10_ha_command (enum CmdCode cmd, char house, int unit) {
  struct x10_ha_command * res;
  house = TO_LOWER(house);
  if (OUT_OF_BOUNDS(house, HOUSE_MIN, HOUSE_MAX)) {
    debug(DBG_ERROR, "House code out of bounds: %c", house);
    return NULL;
  } 

  if (OUT_OF_BOUNDS(unit, UNIT_MIN, UNIT_MAX)) {
    debug(DBG_ERROR, "Unit code out of bounds: %d", unit);
    return NULL;
  }

  res = (struct x10_ha_command *)kmalloc(sizeof(struct x10_ha_command), GFP_KERNEL);
  res->cmd = cmd;
  res->house = HouseCodeLut[house - HOUSE_MIN];
  res->unit = UnitCodeLut[unit - UNIT_MIN];

  debug(DBG_INFO, "Created HA command: %s %c %d", cmd_code_to_str(cmd), house, unit);

  return res;
}

/**
 * Destroy command structure
 */
void del_x10_ha_command (struct x10_ha_command * haCmd) {
  kfree(haCmd);
}

static int x10_transmit_cmd (struct file * devFile, struct x10_cm19a_device * x10Dev, struct x10_ha_command * cmd) {
  int writeRes, readRes;
  char cmdBuf[MAX_CMD_LEN];
  int cmdLen;  
  int i;

  memset(cmdBuf, 0, MAX_CMD_LEN);

  if (IS_CAM_CODE(cmd->cmd)) {
    cmdLen = CAM_CMD_LEN;

    cmdBuf[0] = CAM_CMD_PFX;
    cmdBuf[1] = cmd->cmd >> 8;
    cmdBuf[1] |= house_code_to_cam_code(cmd->house);
    cmdBuf[2] = cmd->cmd & 0xFF;
    cmdBuf[3] = cmd->house;
  } else {
    cmdLen = NORM_CMD_LEN;

    cmdBuf[0] = NORM_CMD_PFX;
    cmdBuf[1] = cmd->unit >> 8;
    cmdBuf[1] |= cmd->house;
    cmdBuf[2] = ~cmdBuf[1];
    cmdBuf[3] = cmd->unit & 0xFF;
    cmdBuf[3] |= cmd->cmd;
    cmdBuf[4] = ~cmdBuf[3];
  }

  debug(DBG_INFO, "Transmitting %d byte command:", cmdLen);
  for (i = 0; i < cmdLen; i++) {
    debug(DBG_INFO, "buf[%d] = %02X", i, cmdBuf[i]);
  }

  writeRes = write_intr_data(devFile, x10Dev, cmdBuf, cmdLen);
  if (writeRes != cmdLen) {
    debug(DBG_ERROR, "Error occurred writing command");
    return -1;
  }

  /* Receive checksum from USB transceiver: */
  readRes = read_intr_data(devFile, x10Dev, ACK_LEN);
  if (readRes < 0) {
    debug(DBG_ERROR, "Error occurred reading checksum");
    return -1;
  } else if ((x10Dev->intrInBuf)[0] != ACK) {
    debug(DBG_WARN, "Wrong ack received: %02X", (x10Dev->intrInBuf)[0]);
  }

  return 0;
}

static int x10_recv (struct file * devFile, struct x10_cm19a_device * x10Dev, struct x10_ha_command * cmd) {
  int i, red, cmdLen = 0;
  unsigned char * buf;

  static const int CMD_REP = 5; /* commands are repeated up to six times by remote */
  int reading = 0;

  /* read command prefix: */
  red = read_intr_data(devFile, x10Dev, 1);
  if (red < 0) {
    return red;
  } else if (red == 1) {
    switch (x10Dev->intrInBuf[0]) {
    case CAM_CMD_PFX:
      cmdLen = CAM_CMD_LEN;
      debug(DBG_INFO, "Read camera command prefix");
      break;
    case NORM_CMD_PFX:
      cmdLen = NORM_CMD_LEN;
      debug(DBG_INFO, "Read on/off/brighten/dim command prefix");
      break;
    default:
      debug(DBG_ERROR, "Read invalid command prefix: %02X", x10Dev->intrInBuf[0]);
      return -1;
    }
  }
  /* read remainder of command, plus ack */
  red = read_intr_data(devFile, x10Dev, cmdLen-1);

  for (i = 0; (i < CMD_REP) && (reading < 2); i++) {
    red = read_intr_data(devFile, x10Dev, cmdLen);
    if (red < 0) {
      /* Error occurred reading data */
      return red;
    }
    if (red == 0) {
      --i;
      if (reading == 1) {
	reading = 2;
      }
    } else {
      reading = 1;
    }
  }

  buf = x10Dev->intrInBuf;

  debug(DBG_INFO, "Read data:");
  for (i = 0; i < cmdLen; i++) {
    debug(DBG_INFO, "buf[%d] = %02X", i, (x10Dev->intrInBuf)[i]);
  }

  if (cmdLen == CAM_CMD_LEN) {
    cmd->cmd = (enum CmdCode)(((buf[1] & 0xF) << 8) | buf[2]);
    cmd->house = (enum HouseCode)buf[3];
    cmd->unit = UNIT_1;
  } else {
    cmd->unit = (enum UnitCode)(((buf[1] & 0xF) << 8) | (buf[3] & ~CMD_OFF));
    cmd->house = (enum HouseCode)(buf[1] & 0xF0);
    cmd->cmd = (enum CmdCode)(buf[3] & CMD_OFF);
  }

  return 0;
}

/* Initialize CM19A so it recognizes signals from the CR12A or CR14A remote */
static int init_remotes (struct file * devFile, struct x10_cm19a_device * x10Dev) {
  unsigned char cr12init1[] =
    { 0x20, 0x34, 0xcb, 0x58, 0xa7 };
  unsigned char cr12init2[]  =
    { 0x80, 0x01, 0x00, 0x20, 0x14 };
  unsigned char cr12init3[]  =
    { 0x80, 0x01, 0x00, 0x00, 0x14, 0x24, 0x20, 0x20 };

  int writeRes = write_intr_data(devFile, x10Dev, cr12init1, sizeof(cr12init1));
  if (writeRes == sizeof(cr12init1)) {
    writeRes = write_intr_data(devFile, x10Dev, cr12init2, sizeof(cr12init2));
    if (writeRes == sizeof(cr12init2)) {
      writeRes = write_intr_data(devFile, x10Dev, cr12init3, sizeof(cr12init3));
      if (writeRes == sizeof(cr12init3)) {
	return 0;
      }
    }
  }

  debug(DBG_ERROR, "Failed to initialize transceiver to listen to CR12A and CR14A remote controls.");
  return -EIO;
}

/******************************/
/*** DEVICE FILE OPERATIONS ***/
/******************************/

/**
 * Initialize an individual transceiver
 */
static int devf_install_dev (struct x10_cm19a_device * toInst) {
  int addRet = usb_register_dev(toInst->usbIntf, &X10Cm19ACls);
  if (addRet) {
    debug(DBG_ERROR, "Failed to register minor number for USB device.");
  }
  
  return addRet;
}

/**
 * Remove an individual transceiver
 */
static int devf_remove_dev (struct x10_cm19a_device * toRemove) {
  usb_deregister_dev(toRemove->usbIntf, &X10Cm19ACls);

  return 0;
}

/**
 * Process a read request on the device file
 */
static ssize_t devf_read (struct file * devFile, char __user * buf, size_t bufLen, loff_t * bufOff) {
  int result;

  struct x10_cm19a_device * x10Dev;
  struct x10_ha_command cmd;

#define MAX_CMD_STR_LEN 10

  char cmdStrBuf[MAX_CMD_STR_LEN+1];

  debug(DBG_INFO, "Read from device file");

  x10Dev = (struct x10_cm19a_device *)devFile->private_data;
  
  result = x10_recv(devFile, x10Dev, &cmd);

  /* if the read was successful, copy the data to userspace */
  if (!result) {
    int bytesWritten = 0;
    switch (cmd.cmd) {
    case CMD_ON:
    case CMD_OFF:
      bytesWritten = snprintf(cmdStrBuf, MAX_CMD_STR_LEN, "%c%c%02d\n", cmd_code_to_char(cmd.cmd), house_code_to_char(cmd.house), unit_code_to_int(cmd.unit));
      break;
    default:
      bytesWritten = snprintf(cmdStrBuf, MAX_CMD_STR_LEN, "%c%c\n", cmd_code_to_char(cmd.cmd), house_code_to_char(cmd.house));
    }

    if (copy_to_user(buf, cmdStrBuf, bytesWritten))
      return -EFAULT;
    else
      return bytesWritten;
  }
  
  return 0;
}

/**
 * Parse command directed to device file
 *
 * <+|-><house><unit>
 *  + = on
 *  - = off
 * 
 * <u|d|l|r|b|s><house>
 *  u = up
 *  d = down
 *  l = left
 *  r = right
 *  b = brighten
 *  s = soften
 */
static ssize_t devf_write (struct file * devFile, const char __user * userBuf, size_t bufLen, loff_t * bufOff) {
  unsigned char * buf;
  char digit[2];
  u_int8_t unit = 1;
  char house;
  enum CmdCode cmdCode;
  struct x10_ha_command * cmd;

  int i = 0;

  struct x10_cm19a_device * x10Dev = (struct x10_cm19a_device *)devFile->private_data;

  /* check for empty buffer */
  if (bufLen == 0) {
    return 0;
  }

  buf = kmalloc(bufLen, GFP_KERNEL);

  bufLen--; /* discard terminator */

  debug(DBG_INFO, "Wrote %d bytes to device file", (int)bufLen);

  if (copy_from_user(buf, userBuf, bufLen)) {
    goto handleErr;
  }

  if (bufLen < 2) {
    debug(DBG_INFO, "Command is too short, ignoring: %d.", (int)bufLen);
    goto handleErr;
  }

  cmdCode = parse_cmd_code(buf[i++]);
  if (cmdCode == CMD_INVALID) goto handleErr;

  house = buf[i++];

  if (i < bufLen) {
    digit[0] = buf[i++];
    if ((digit[0] < '0') || ('9' < digit[0])) {
      debug(DBG_INFO, "Malformed command, illegal digit #1: %c, ignoring: %s", digit[0], buf);
      goto handleErr;
    } else {
      unit = (digit[0] - '0');
    }
    if (i < bufLen) {
      digit[1] = buf[i++];
      if ((digit[1] < '0') || ('9' < digit[1])) {
	debug(DBG_INFO, "Illegal digit #2: %c, ignoring: %s", digit[1], buf);
      } else {
	unit *= 10;
	unit += (digit[1] - '0');
      }
    }
  } else {
    if ((cmdCode == CMD_ON) || (cmdCode == CMD_OFF)) {
      debug(DBG_ERROR, "On and off commands require a unit number");
      goto handleErr;
    }
  }

  cmd = new_x10_ha_command(cmdCode, house, unit);
  if (cmd == NULL) goto handleErr;

  /* Process command in buffer: */
  x10_transmit_cmd(devFile, x10Dev, cmd);

  del_x10_ha_command(cmd);

  kfree(buf);

  return i;

 handleErr:
  kfree(buf);
  return 1;
}

/**
 * Process readdir operation (not yet supported)
 */
static int devf_readdir (struct file * devFile, void * a0, filldir_t a1) {
  return -EINVAL;
}

static unsigned int devf_poll (struct file * devFile, struct poll_table_struct * pollTbl) {
  struct x10_cm19a_device * x10Dev;
  unsigned int mask = 0;

  debug(DBG_ERROR, "Attempted to poll file");
  
  x10Dev = (struct x10_cm19a_device *)devFile->private_data;
  
  poll_wait(devFile, &x10Dev->intrInWait, pollTbl);
  poll_wait(devFile, &x10Dev->intrOutWait, pollTbl);
  
  check_read_buffer(x10Dev);
  if (x10Dev->readPktLen > 0) {
    mask |= POLLIN | POLLRDNORM;
  }
  if (!x10Dev->intrOutBusy) {
    mask |= POLLOUT | POLLWRNORM;
  }
  
  return mask;
}

/**
 * Handle device file open, control simultaneous access
 */
static int devf_open (struct inode * iNode, struct file * toOpen) {
  int subMinor;
  int result = 0;
  struct x10_cm19a_device * x10Dev;
  struct usb_interface * usbIntf;
  
  nonseekable_open(iNode, toOpen);

  subMinor = iminor(iNode);

  down(&disconnMutex);
  
  usbIntf = usb_find_interface(&X10Cm19ADriver, subMinor);
  if (!usbIntf) {
    debug(DBG_ERROR, "Can't find device for minor %d", subMinor);
    result = -ENODEV;
    goto handleErr;
  }
  
  x10Dev = usb_get_intfdata(usbIntf);
  if (!x10Dev) {
    debug(DBG_ERROR, "X10 device structure not stored in interface");
    result = -ENODEV;
    goto handleErr;
  }
 
  if (down_interruptible(&x10Dev->mutex)) {
    debug(DBG_ERROR, "Unable to lock X10 device structure mutex");
    result = -ERESTARTSYS;
    goto handleErr;
  }
 
  /* initialize in direction */
  //x10Dev->read_buffer_length = 0;
  //x10Dev->read_packet_length = 0;
  usb_fill_int_urb(x10Dev->intrInUrb, x10Dev->usbDev,
		   usb_rcvintpipe(x10Dev->usbDev, x10Dev->intrInEpAddr),
		   x10Dev->intrInBuf, x10Dev->intrInSz,
		   read_intr_cb,
		   x10Dev, x10Dev->intrInInterval);
  
  x10Dev->intrInConnected = 1;
  x10Dev->intrInDone = 0;
  mb();
  
  result = usb_submit_urb(x10Dev->intrInUrb, GFP_KERNEL);
  if (result) {
    debug(DBG_ERROR, "Couldn't submit interrupt input URB: %d", result);
    x10Dev->intrInConnected = 0;
    goto handleErrUnlock;
  }
  
  if (x10Dev->devfOpened) {
    debug(DBG_ERROR, "Device file already opened");
    result = -EBUSY;
    goto handleErr;
  }
  x10Dev->devfOpened = 1;

  /* increment our usage count for the device */
  kref_get(&x10Dev->kernRef);
  
  /* save our object in the file's private structure */
  toOpen->private_data = x10Dev;

 handleErrUnlock:
  up(&x10Dev->mutex);

 handleErr:
  up(&disconnMutex);

  return result;
}

/**
 * Handle release of device file
 */
static int devf_release (struct inode * iNode, struct file * toOpen) {
  int result = 0;
  
  struct x10_cm19a_device * x10Dev = (struct x10_cm19a_device *)toOpen->private_data;
  if (x10Dev == NULL) {
    debug(DBG_ERROR, "devf_release: invalid device");
    return -ENODEV;
  }

  if (down_interruptible(&x10Dev->mutex)) {
    result = -ERESTARTSYS;
    goto handleErr;
  }

  /* Wait for I/O to complete */
  if (x10Dev->intrOutBusy) {
    wait_event_interruptible_timeout(x10Dev->intrOutWait, !x10Dev->intrOutBusy, 2 * HZ);
  }
  x10_cm19a_device_abort_xfers(x10Dev);

  x10Dev->devfOpened = 0;

  up(&x10Dev->mutex);

  /* Release object reference */
  kref_put(&x10Dev->kernRef, release_x10_cm19a_device);

 handleErr:
  return result;
}

#endif
