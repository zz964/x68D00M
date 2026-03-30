/*
 * dmac.h  --  X68000 HD63450 DMAC channel-2 register map.
 *
 * Register offsets and bit fields verified against the NFG Games X68000 wiki
 * (https://gamesx.com/wiki/doku.php?id=x68000:dmac), which documents the
 * actual HD63450 as fitted to the X68000.  Several offsets and bit
 * assignments differ from the MC68450 data sheet.
 *
 * Key differences vs MC68450 data sheet:
 *   - NIV/EIV/MFC/CPR/DFC/BFC are at DIFFERENT offsets (see table below).
 *   - SCR bits[7:4] are RESERVED (always 0); MAC is bits[3:2], DAC is [1:0].
 *   - OCR CHAIN field: 10=array chain (NOT 01 as in some MC68450 errata).
 *   - OCR DIR: 0=Memory->Device (MAR->DAR), 1=Device->Memory (DAR->MAR).
 *   - CSR status bits are WRITE-1-TO-CLEAR (not write-0-to-clear).
 *   - CCR INT bit is bit 3, and 1=interrupt DISABLED.
 *
 * Doom async blit design (screens_expanded -> GVRAM, array-chain):
 *   Entry 2y  : mar = screens_expanded row y,  mtc = 320 (words)
 *   Entry 2y+1: mar = zero_pad,               mtc = 704 (words)
 *   After both entries DAR has advanced 640+1408 = 2048 bytes = GVRAM stride.
 *
 * Cache coherency: I_CachePushAll() (cpusha dc) is called before each kick
 * to flush the 68060 copy-back cache for screens_expanded.
 *
 * All DMAC registers are reprogrammed every frame in I_FinishUpdate to
 * survive IOCS resets (the OS reprograms channel 2 for disk/audio DMA).
 */

#ifndef DMAC_H
#define DMAC_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Base addresses                                                       */
/* ------------------------------------------------------------------ */
#define DMAC_BASE       0x00E84000UL
#define DMAC_CH_STRIDE  0x40U
#define DMAC_CH2_BASE   (DMAC_BASE + 2U * DMAC_CH_STRIDE)   /* $E84080 */

/* ------------------------------------------------------------------ */
/* Per-channel register BYTE offsets (HD63450 on X68000)               */
/* ------------------------------------------------------------------ */
#define DMAC_OFF_CSR   0x00   /* Channel Status Register          (byte, r/w W1C) */
#define DMAC_OFF_CER   0x01   /* Channel Error Register           (byte, r)       */
#define DMAC_OFF_DCR   0x04   /* Device Control Register          (byte, r/w)     */
#define DMAC_OFF_OCR   0x05   /* Operation Control Register       (byte, r/w)     */
#define DMAC_OFF_SCR   0x06   /* Sequence Control Register        (byte, r/w)     */
#define DMAC_OFF_CCR   0x07   /* Channel Control Register         (byte, r/w)     */
#define DMAC_OFF_MTC   0x0A   /* Memory Transfer Count            (word, r/w)     */
#define DMAC_OFF_MAR   0x0C   /* Memory Address Register          (long, r/w)     */
#define DMAC_OFF_DAR   0x14   /* Device Address Register          (long, r/w)     */
#define DMAC_OFF_BTC   0x1A   /* Base Transfer Count (chain entries)(word, r/w)   */
#define DMAC_OFF_BAR   0x1C   /* Base Address Reg (chain table ptr)(long, r/w)    */
#define DMAC_OFF_NIV   0x25   /* Normal Interrupt Vector          (byte, r/w)     */
#define DMAC_OFF_EIV   0x27   /* Error Interrupt Vector           (byte, r/w)     */
#define DMAC_OFF_MFC   0x29   /* Memory Function Code             (byte, r/w)     */
#define DMAC_OFF_CPR   0x2D   /* Channel Priority Register        (byte, r/w)     */
#define DMAC_OFF_DFC   0x31   /* Device Function Code             (byte, r/w)     */
#define DMAC_OFF_BFC   0x39   /* Base Function Code (for BAR)     (byte, r/w)     */

/* ------------------------------------------------------------------ */
/* Typed access macros for channel 2                                    */
/* ------------------------------------------------------------------ */
#define DMAC_CH2_B(off) (*(volatile  uint8_t *)(DMAC_CH2_BASE + (off)))
#define DMAC_CH2_W(off) (*(volatile uint16_t *)(DMAC_CH2_BASE + (off)))
#define DMAC_CH2_L(off) (*(volatile uint32_t *)(DMAC_CH2_BASE + (off)))

/* ------------------------------------------------------------------ */
/* CSR bits  (WRITE-1-TO-CLEAR for COC/BTC/NDT/ERR/ACT)               */
/* ------------------------------------------------------------------ */
#define DMAC_CSR_COC   0x80U  /* Channel Operation Complete                     */
#define DMAC_CSR_BTC   0x40U  /* Block Transfer Complete (all chain entries done)*/
#define DMAC_CSR_NDT   0x20U  /* Normal Device Termination                      */
#define DMAC_CSR_ERR   0x10U  /* Error occurred                                 */
#define DMAC_CSR_ACT   0x08U  /* Channel Active (transfer in progress)          */
/* Write this value to CSR to clear all sticky status bits (W1C): */
#define DMAC_CSR_CLEAR 0xF8U  /* COC|BTC|NDT|ERR|ACT */

/* ------------------------------------------------------------------ */
/* CCR bits                                                             */
/* NOTE: INT bit 3 = 1 means DISABLE interrupt (1=off, 0=on).         */
/* ------------------------------------------------------------------ */
#define DMAC_CCR_STR   0x80U  /* Start transfer                                 */
#define DMAC_CCR_CNT   0x40U  /* Continue (restart from chain)                  */
#define DMAC_CCR_HLT   0x20U  /* Halt channel                                   */
#define DMAC_CCR_SAB   0x10U  /* Software abort                                 */
#define DMAC_CCR_INT   0x08U  /* Interrupt DISABLE (1=disabled, 0=enabled)      */

/* Start a transfer with interrupts disabled (we poll CSR.BTC instead): */
#define DMAC_CCR_START_NOINT  (DMAC_CCR_STR | DMAC_CCR_INT)  /* $88 */

/* ------------------------------------------------------------------ */
/* DCR values                                                           */
/* ------------------------------------------------------------------ */

/* Burst mode (XRM=00): DMAC holds bus for entire chain.
 * On the X68000 060turbo the bus-bridge renegotiates arbitration per
 * word, adding ~4 us overhead per word (~700 ms for a full blit).
 * DO NOT USE burst mode with the 060turbo. */
#define DMAC_DCR_BURST_16     0x08U  /* XRM=00, DTYP=00, DPS=1 */

/* Cycle-steal mode (XRM=10): DMAC takes one bus cycle per word, then
 * releases.  No continuous burst, bus-bridge overhead is minimal.
 * Use this on the 060turbo for acceptable DMA throughput.
 *   bits[7:6] XRM  = 10 (cycle-steal, no hold)
 *   bits[5:4] DTYP = 00 (dual-address, normal 68000 bus)
 *   bit  3    DPS  =  1 (16-bit device port)
 *   bits[2:0] PCL  = 00 (unused) */
#define DMAC_DCR_STEAL_16     0x88U  /* XRM=10, DTYP=00, DPS=1 */

/* ------------------------------------------------------------------ */
/* OCR value: memory->device, 16-bit word, array chain, speed-limited  */
/*   bit  7    DIR  =  0 (Memory->Device: read MAR, write DAR)        */
/*   bit  6    BTD  =  0 (normal operation)                           */
/*   bits[5:4] SIZE = 01 (16-bit word transfers)                      */
/*   bits[3:2] CHAIN= 10 (array chain mode)                           */
/*   bits[1:0] REQG = 00 (speed-limited auto request -- no REQ line)  */
/* = 0_0_01_10_00 = 0x18                                              */
/* Note: REQG=01 (max speed) with burst mode caused ~700 ms blits on  */
/* the 060turbo due to bus-bridge arbitration overhead.               */
/* ------------------------------------------------------------------ */
#define DMAC_OCR_M2D_W16_ACHAIN  0x18U

/* ------------------------------------------------------------------ */
/* SCR value: increment both MAR (source) and DAR (destination)        */
/*   bits[7:4] = 0 (reserved -- hardware ignores writes to these)     */
/*   bits[3:2] MAC = 01 (increment memory address after each transfer)*/
/*   bits[1:0] DAC = 01 (increment device address after each transfer)*/
/* = 0x05                                                             */
/* ------------------------------------------------------------------ */
#define DMAC_SCR_INC_INC      0x05U

/* ------------------------------------------------------------------ */
/* Interrupt vectors (channel 2)                                        */
/* ------------------------------------------------------------------ */
#define DMAC_NIV_CH2  0x68U   /* normal completion -- vector at $0001A0 */
#define DMAC_EIV_CH2  0x69U   /* error            -- vector at $0001A4  */

/* ------------------------------------------------------------------ */
/* Function codes (supervisor data access for MFC/DFC/BFC)             */
/* ------------------------------------------------------------------ */
#define DMAC_FC_SUPER_DATA  0x05U

/* ------------------------------------------------------------------ */
/* Array chain entry: 4-byte source address + 2-byte word count.       */
/* The HD63450 reads these 6-byte entries sequentially from BAR.       */
/* packed ensures sizeof == 6 with no struct padding.                  */
/* ------------------------------------------------------------------ */
typedef struct __attribute__((packed)) {
    uint32_t mar;   /* source byte address */
    uint16_t mtc;   /* transfer count in WORDS */
} dma_chain_t;

#endif /* DMAC_H */
