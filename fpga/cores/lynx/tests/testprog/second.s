; Second-stage loader for the HLE test cartridge (original code). cc65's signed
; first stage copies these 151 bytes to $FB68 and jumps here; we copy the test
; program (LEN bytes, next on the cartridge) to $0400 and start it.
.setcpu "65C02"
RCART0 = $FCB2
.import LEN
.segment "CODE"
        lda #$00
        sta $10
        lda #$04
        sta $11
        ldx #<LEN
        ldy #>LEN+1
@loop:  lda RCART0
        sta ($10)
        inc $10
        bne @n
        inc $11
@n:     dex
        bne @loop
        dey
        bne @loop
        jmp $0400
