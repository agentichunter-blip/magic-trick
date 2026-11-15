; Magic Trick Lynx boot stub (original code). Used in place of the Atari boot
; ROM when none is on the SD card. The MCU decrypts the cartridge's loader
; (RSA, public key, as the boot ROM would) and patches it into LOADER below,
; with the number of encrypted bytes to skip in SKIPLO/SKIPHI. At reset this
; stub clears RAM, sets up the display timers, selects cartridge block 0,
; advances the cartridge counter past the encrypted loader, copies the loader
; to $0200 and jumps to it. $FE00 (select cartridge block A) works as on the
; Lynx; the boot ROM's own loader entries ($FE19, $FE4A) cannot decrypt here
; and restart the stub instead. The core starts the CPU at $FF80 (the boot
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
.segment "J19"
        jmp reset               ; $FE19
.segment "SEL"
; A = block: shifted MSB first into the cartridge address register through
; IODAT bit 1, one strobe rising edge per bit (SYSCTL1 bit 0; bit 1 = power).
selblk: phx
        phy
        tay
        lda #$1a
        sta IODIR
        ldx #8
@bit:   tya
        asl a
        tay
        lda #0
        rol a
        asl a
        sta IODAT
        lda #$03
        sta SYSCTL1
        lda #$02
        sta SYSCTL1
        dex
        bne @bit
        ply
        plx
        rts

.segment "J4A"
        jmp reset               ; $FE4A
.segment "DATA"
LOADER: .res 256, 0
SKIPLO: .byte 0
SKIPHI: .byte 0
irq:    rti
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
        lda #0
        tax
        tay
        jmp $0200

.segment "VECTORS"
        .word irq, reset, irq
