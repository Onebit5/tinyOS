#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* the sata controller, which is how a real disk is reached.
 *
 * 0.1.8 found the devices on the pci bus and said what they were. this
 * is the first time we do anything with one. an ahci controller is
 * found by its class -- mass storage, sata, ahci -- and its last base
 * address register points at a block of memory-mapped registers rather
 * than at ports.
 *
 * the shape of it is worth knowing because it is nothing like the old
 * ide interface it replaced. you do not write a command to a register
 * and wait. you build the command in ram: a command header saying how
 * long it is and where its table is, a table holding the frame the
 * drive will actually receive, and a scatter list saying which physical
 * addresses the data should be moved to or from. then you set one bit
 * to say slot N is ready, and the controller does the rest by itself,
 * clearing the bit when it is done. the cpu is not involved in moving
 * any of the bytes.
 *
 * we use one slot and poll for it, because a disk read that blocks the
 * kernel for a millisecond is not worth an interrupt handler yet. every
 * wait is bounded: a controller that never answers must not be able to
 * hang the boot. */

#define AHCI_SECTOR 512

/* find a controller on the pci bus and bring up the first disk on it.
 * returns false, quietly and safely, when there is no controller, no
 * drive, or anything at all goes wrong -- a machine with no disk boots
 * exactly as it did before */
bool ahci_init(void);

bool ahci_present(void);

/* the drive, as it describes itself. empty strings if unknown */
const char *ahci_model(void);
const char *ahci_serial(void);

/* how many 512-byte sectors it has */
uint64_t ahci_sectors(void);

/* move sectors to and from the disk. `count` is limited to what fits in
 * the bounce buffer, currently 8 */
bool ahci_read(void *ctx, uint64_t lba, uint32_t count, void *buf);
bool ahci_write(void *ctx, uint64_t lba, uint32_t count, const void *buf);

#define AHCI_MAX_SECTORS 8

#endif
