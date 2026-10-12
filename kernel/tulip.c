//
// Tulip network driver.
//
// The card qemu emulates for this lab is an Intel 82540EM (the
// "e1000").  The rest of the kernel (pci.c, net.c, trap.c) refers to
// the NIC through the e1000_* names, so this driver exports that
// interface: e1000_init(), e1000_transmit(), and e1000_intr().
//
// Step One: initialize the NIC -- define the transmit/receive
// descriptor rings, point the device's ring-base registers at them,
// program the MAC filter and the TX/RX control registers, and turn on
// transmit, receive, and receive interrupts.
//
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
// this code loosely follows the initialization directions
// in Chapter 14 of Intel's Software Developer's Manual.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  // Build the transmit descriptor ring.  Each descriptor is marked
  // DD ("done") so that the first TX_RING_SIZE transmits see it free.
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_ring[i].addr = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;

  // [E1000 14.4] Receive initialization
  // Build the receive descriptor ring; hand each descriptor a
  // freshly allocated DMA buffer that the device can fill in.
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_ring[i].addr = (uint64) kalloc();
    if (!rx_ring[i].addr)
      panic("e1000");
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC

  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

// buf contains an ethernet frame; program it into the TX descriptor
// ring so that the e1000 sends it.  The caller hands us ownership of
// buf, so we stash the pointer in the descriptor and kfree() it once
// the device is done with the slot.
//
// return 0 on success.
// return -1 on failure (e.g., there is no descriptor available)
// so that the caller knows to free buf.
int
e1000_transmit(char *buf, int len)
{
  acquire(&e1000_lock);

  uint32 index = regs[E1000_TDT];

  // the device has moved on only if it has set DD on this descriptor.
  if((tx_ring[index].status & E1000_TXD_STAT_DD) == 0){
    // no free descriptor; tell the caller to keep (and free) buf.
    release(&e1000_lock);
    return -1;
  }

  // the slot is free; release the buffer of the packet sent last time
  // it was used.
  if(tx_ring[index].addr)
    kfree((void *) tx_ring[index].addr);

  tx_ring[index].addr = (uint64) buf;
  tx_ring[index].length = len;
  tx_ring[index].status = 0; // device sets DD when it finishes
  tx_ring[index].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

  // tell the device there is a new packet at this slot.
  regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;

  release(&e1000_lock);
  return 0;
}

// check for packets that have arrived, and deliver each one to
// net_rx().  net_rx() (and the handlers it calls) takes ownership of
// the buffer, so we give the descriptor a fresh buffer in its place.
static void
e1000_recv(void)
{
  while(1){
    uint32 index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

    // the device has filled in this descriptor if it set DD.
    if((rx_ring[index].status & E1000_RXD_STAT_DD) == 0)
      return;

    // hand the packet to xv6's network stack; it will free the buffer.
    net_rx((char *) rx_ring[index].addr, rx_ring[index].length);

    // allocate a replacement buffer and re-arm the descriptor.
    char *buf = kalloc();
    if(buf == 0)
      panic("e1000_recv");
    rx_ring[index].addr = (uint64) buf;
    rx_ring[index].status = 0;

    // advance the tail so the device knows this slot is free again.
    regs[E1000_RDT] = index;
  }
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
