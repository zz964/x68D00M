#ifndef __IRQ_H__
#define __IRQ_H__



#define IRQ_BASE 	(0x000100)

/* MFP */
#define IRQ_40	(IRQ_BASE)	/* RTC alarm/1Hz */
#define IRQ_41	(IRQ_BASE+0x04) /* external power OFF */
#define IRQ_42	(IRQ_BASE+0x08) /* front switch OFF */
#define IRQ_43	(IRQ_BASE+0x0C) /* FM sound source */
#define IRQ_44	(IRQ_BASE+0X10) /* TIMER-D (used in BG processing) */
#define IRQ_45	(IRQ_BASE+0x14) /* TIMER-C (mouse/cursor/FDD/etc) */
#define IRQ_46	(IRQ_BASE+0x18) /* V-DISP */
#define IRQ_47	(IRQ_BASE+0x1C) /* RTC clock */
#define IRQ_48	(IRQ_BASE+0X20) /* TIMER-B */
#define IRQ_49	(IRQ_BASE+0x24) /* key serial output error */
#define IRQ_4A	(IRQ_BASE+0x28) /* key serial output air */
#define IRQ_4B	(IRQ_BASE+0x2C) /* key serial input error */
#define IRQ_4C	(IRQ_BASE+0X30) /* key yes serial output  */
#define IRQ_4D	(IRQ_BASE+0x34) /* TIMER-A */
#define IRQ_4E	(IRQ_BASE+0x38) /* CRTC */
#define IRQ_4F	(IRQ_BASE+0x3C) /* H-SYNC */

/* SCC */
#define IRQ_50	(IRQ_BASE+0X40)	/* SCC B xmit buffer empty */
#define IRQ_51	(IRQ_BASE+0x44) /* " */
#define IRQ_52	(IRQ_BASE+0x48) /* SCC B external status chng */
#define IRQ_53	(IRQ_BASE+0x4C) /* " */
#define IRQ_54	(IRQ_BASE+0X50) /* SCC B recv char valid(mouse 1 byte input) */
#define IRQ_55	(IRQ_BASE+0x54) /* "  */
#define IRQ_56	(IRQ_BASE+0x58) /* SCC B special rx condition */
#define IRQ_57	(IRQ_BASE+0x5C) /* " */
#define IRQ_58	(IRQ_BASE+0X60) /* SCC A transmit buffer empty */
#define IRQ_59	(IRQ_BASE+0x64) /* " */
#define IRQ_5A	(IRQ_BASE+0x68) /* SCC A external status change */
#define IRQ_5B	(IRQ_BASE+0x6C) /* " */
#define IRQ_5C	(IRQ_BASE+0X70) /* SCC A receive character enable(RS-232C 1 byte input) */
#define IRQ_5D	(IRQ_BASE+0x74) /* "  */
#define IRQ_5E	(IRQ_BASE+0x78) /* SCC A special rx condition */
#define IRQ_5F	(IRQ_BASE+0x7C) /* " */

/* I/O */
#define IRQ_60	(IRQ_BASE+0X80) /* FDC status */
#define IRQ_61	(IRQ_BASE+0x84) /* FDC insert/eject */
#define IRQ_62	(IRQ_BASE+0x88) /* HDC status */
#define IRQ_63	(IRQ_BASE+0x8C) /* printer lady(?) */

/* DMAC */
#define IRQ_64	(IRQ_BASE+0X90) /* #0 exit (FDD) */
#define IRQ_65	(IRQ_BASE+0x94) /* #0 error (") */
#define IRQ_66	(IRQ_BASE+0x98) /* #1 exit (SASI) */
#define IRQ_67	(IRQ_BASE+0x9C) /* #1 error (") */
#define IRQ_68	(IRQ_BASE+0XA0) /* #2 exit (IOCS _DMAMOVE, _DMAMOV_A, _DMAMOV_L) */
#define IRQ_69	(IRQ_BASE+0xA4) /* #2 error (") */
#define IRQ_6A	(IRQ_BASE+0xA8) /* #3 exit (ADPCM) */
#define IRQ_6B	(IRQ_BASE+0xAC) /* #3 error (") */

/* SCSI */
#define IRQ_6C	(IRQ_BASE+0X100) /* SCSI (built-in SCSI) */
#define IRQ_F6	(IRQ_BASE+0x2D8) /* SCSI (SCSI board) */

#include <stdint.h>

extern volatile uint32_t *vec_vdisp;


extern void IRQ_vdisp() __attribute__ ((interrupt_handler));

extern void IRQ_insVDisp(void (*func)()); /* function pointer to handler, address of irq vector */
extern void IRQ_uninsVDisp();

/*
set bit 4(V-DISP) of MFP GPIP to 0 for VBLANK interrupt (1 = VDISP)
set bit 6(V-DISP) of MFP IERB to 1 to enable V-DISP interrupt
*/
#endif