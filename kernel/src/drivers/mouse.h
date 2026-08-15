#ifndef DRIVERS_MOUSE_H
#define DRIVERS_MOUSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* the ps/2 mouse: the first input here that is not a stream of
 * characters.
 *
 * everything until now arrived as bytes in a queue, and a queue is the
 * right shape for typing because typing *is* a sequence -- the order is
 * the meaning. a mouse is not that. it reports a change since the last
 * time it was asked, and the interesting thing is never one report, it
 * is where the pointer has ended up. so the driver keeps a position and
 * the events are edges: it moved, a button went down, a button came up.
 *
 * the packets are three bytes, or four on anything made after about
 * 1996 -- and the fourth is asked for by a handshake so odd it can only
 * be historical: set the sample rate to 200, then 100, then 80, and a
 * mouse that understands starts answering with a wheel byte. see
 * mouse_init.
 *
 * the decoder takes bytes and gives events, and touches no hardware at
 * all. that is what lets the fiddly parts -- the sign bits living in
 * the first byte, the resynchronisation when a byte goes missing -- be
 * tested against sequences written by hand. */

#define MOUSE_LEFT   0x1
#define MOUSE_RIGHT  0x2
#define MOUSE_MIDDLE 0x4

struct mouse_event {
    int  dx, dy;        /* since the last event. y is already flipped:
                         * the mouse counts up, screens count down */
    int  wheel;         /* -1, 0 or 1 on a mouse that has one */
    uint8_t buttons;    /* what is held *now* */
    uint8_t pressed;    /* what went down in this event */
    uint8_t released;   /* and what came up */
};

/* ---- the part with no hardware in it ---------------------------------
 *
 * feed it bytes as they arrive. returns true when a whole packet has
 * been assembled, and fills in what changed. */

struct mouse_decoder {
    uint8_t  packet[4];
    unsigned at;
    unsigned size;      /* 3, or 4 once the wheel has been negotiated */
    uint8_t  buttons;   /* what was held last time, to work out edges */
};

void mouse_decoder_init(struct mouse_decoder *d, unsigned packet_size);
bool mouse_decode(struct mouse_decoder *d, uint8_t byte,
                  struct mouse_event *out);

/* ---- the driver ------------------------------------------------------ */

/* find one, wake it up, and start listening. safe to call on a machine
 * with no mouse: it says so and everything carries on */
void mouse_init(void);

bool mouse_present(void);
bool mouse_has_wheel(void);

/* where the pointer is, in characters rather than pixels -- this is a
 * text console and a pointer that could sit between two cells would be
 * pointing at nothing */
void mouse_position(size_t *col, size_t *row);
uint8_t mouse_buttons(void);

/* how many packets have arrived, and how many were thrown away for
 * arriving out of step. the second number being anything but zero is
 * the only symptom a desynchronised mouse has */
uint64_t mouse_packets(void);
uint64_t mouse_resyncs(void);

#endif
