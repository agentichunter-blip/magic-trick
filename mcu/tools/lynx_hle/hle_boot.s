; Magic Trick Lynx boot stub (original code). Used in place of the Atari boot
; ROM when none is on the SD card. The MCU decrypts the cartridge's loader
; (RSA, public key, as the boot ROM would) and patches it into LOADER below,
; with the number of encrypted bytes to skip in SKIPLO/SKIPHI. At reset this
; stub clears RAM, sets up the display timers, selects cartridge block 0,
; advances the cartridge counter past the encrypted loader, copies the loader
; to $0200 and jumps to it. $FE00 (select cartridge block A) works as on the
; Lynx. $FE4A (decrypt the next encrypted chunk to ($05) and jump there; used
; by two-stage loaders such as Alien vs Predator) reads that chunk already
; decrypted: the MCU replaces each 51-byte block in its copy of the cartridge
; with the 50 plaintext bytes and one padding byte (LynxHle_BuildStage2).
; $FE19 (the boot ROM's own loader entry) restarts the stub. The core starts the CPU at $FF80 (the boot
; ROM's reset entry), not through the reset vector, so reset is there.
.setcpu "65C02"
RCART0   = $FCB2
TIM0BKUP = $FD00
TIM0CTLA = $FD01
TIM2BKUP = $FD08
TIM2CTLA = $FD09
SYSCTL1  = $FD87
IODIR    = $FD8A
IODAT    = $FD8B
DISPCTL  = $FD92
PBKUP    = $FD93
MAPCTL   = $FFF9

.segment "J00"
        jmp selblk              ; $FE00 select cartridge block A
.segment "G03"
; Hand over to the loader in the state the boot ROM leaves: copy pointer
; ($05/$06) at the loader's end (two-stage loaders increment $06 to place their
; second stage) and the stack pointer at $03.
handoff: lda #$02
        sta $06
        lda LOADLEN
        sta $05
        ldx #$03
        txs
        lda #0
        tay
        jmp $0200
.segment "J19"
        jmp reset               ; $FE19
.segment "SEL"
; A = block: shifted MSB first into the cartridge address register through
; IODAT bit 1, one strobe rising edge per bit (SYSCTL1 bit 0; bit 1 = power).
; Uses no stack or zero page (games call it with their own stack layout) and
; leaves A=0, X=2, carry set and Y unchanged, as the boot ROM's routine does.
selblk: sec                     ; end marker
        rol a
@bit:   ldx #0
        bcc @z
        ldx #2
@z:     stx IODAT
        ldx #$03
        stx SYSCTL1
        ldx #$02
        stx SYSCTL1
        asl a
        bne @bit
        stz IODAT
        rts

.segment "J4A"
        jmp stage2              ; $FE4A
.segment "DATA"
LOADER: .res 256, 0
SKIPLO: .byte 0
SKIPHI: .byte 0
irq:    rti
; $FE4A: count byte, then per block 50 decrypted bytes to ($05),y and one
; padding byte. Like the boot ROM: leave ($05/$06) at the end of the data and
; re-enter the loader at $0200 (it patches its own first branch beforehand).
stage2: lda RCART0
        eor #$ff
        inc a                   ; blocks = 256 - count
        tax
        ldy #0
@blk:   lda #50
        sta $07
@byte:  lda RCART0
        sta ($05),y
        iny
        bne @nc
        inc $06
@nc:    dec $07
        bne @byte
        lda RCART0              ; padding byte
        dex
        bne @blk
        tya
        clc
        adc $05
        sta $05
        bcc @done
        inc $06
@done:  jmp $0200
        .byte 0
LOADLEN: .byte 0                ; loader length, patched by the MCU ($FF7F)
.segment "RESET"
reset:  sei
        cld
        ldx #$ff
        txs
        stz MAPCTL
        ; clear zero page, stack and $0200-$FBFF
        lda #0
        tax
@zp:    sta $00,x
        sta $0100,x
        inx
        bne @zp
        lda #$02
        sta $01
        ldy #0
@pg:    lda #0
        sta ($00),y
        iny
        bne @pg
        inc $01
        lda $01
        cmp #$fc
        bne @pg
        stz $01
        ; LCD timing as after the boot ROM (59.9 Hz)
        lda #$9e
        sta TIM0BKUP
        lda #$18
        sta TIM0CTLA
        lda #$68
        sta TIM2BKUP
        lda #$1f
        sta TIM2CTLA
        lda #$29
        sta PBKUP
        lda #$0d
        sta DISPCTL
        ; block 0, then skip the encrypted loader (1 + 51 * blocks bytes)
        lda #0
        jsr selblk
        ldx SKIPLO
        ldy SKIPHI
        iny
@skip:  lda RCART0
        dex
        bne @skip
        dey
        bne @skip
        ; copy the decrypted loader to $0200 (256 bytes, zero padded;
        ; at most 5 blocks of 50 bytes)
        ldx #0
@c1:    lda LOADER,x
        sta $0200,x
        inx
        bne @c1
        jmp handoff

.segment "VECTORS"
        .word irq, reset, irq
