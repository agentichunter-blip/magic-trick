; Magic Trick Lynx test program (original code). BS93 format: the core loads it
; straight into RAM and starts it, no boot ROM needed.
; - Display at 75 Hz (timer 0 backup 126 = 127 us lines), so the core's 60 Hz
;   lock has something to do; frame counter at $F0 (frames since start).
; - Top half: a 2-pixel white bar moving 2 pixels per frame (smoothness).
; - Bottom half: 16 colour bars.
; - A tone on audio channel 0 (B held: silent). A held: blue background.
; - Pause (Start) toggles the bar's motion.
.setcpu "65C02"

TIM0BKUP = $FD00
TIM0CTLA = $FD01
TIM2BKUP = $FD08
TIM2CTLA = $FD09
AUD0VOL  = $FD20
AUD0FEED = $FD21
AUD0OUT  = $FD22
AUD0SHFT = $FD23
AUD0BKUP = $FD24
AUD0CTLA = $FD25
MSTEREO  = $FD50
INTRST   = $FD80
INTSET   = $FD81
DISPCTL  = $FD92
PBKUP    = $FD93
DISPADRL = $FD94
DISPADRH = $FD95
GREEN    = $FDA0
BLUERED  = $FDB0
JOYSTICK = $FCB0
SWITCHES = $FCB1
MAPCTL   = $FFF9

FB       = $C000          ; 160x102 at 4 bpp = 8160 bytes
FRAMES   = $F0            ; 16-bit frame counter
BARX     = $F2            ; bar byte column 0..79
PTR      = $F4
MOVING   = $F6
LASTSW   = $F7

.segment "BLLHDR"
        .byte $80, $08
        .byte >start, <start
        .byte >(__CODE_SIZE__ + 10), <(__CODE_SIZE__ + 10)
        .byte "BS93"
.import __CODE_SIZE__

.segment "CODE"
start:  sei
        cld
        ldx #$ff
        txs
        lda #$0c                ; RAM at $FE00-$FFFF (no boot ROM, no ROM vectors)
        sta MAPCTL
        ; LCD timing: 127 us lines x 105 = 13.3 ms (75 Hz)
        lda #126
        sta TIM0BKUP
        lda #$18                ; reload, count, 1 us clock
        sta TIM0CTLA
        lda #104
        sta TIM2BKUP
        lda #$9f                ; IRQ flag, reload, count, linked to timer 0
        sta TIM2CTLA
        lda #41
        sta PBKUP
        ; palette: pen 0 dark grey, 1 white, 2..15 a rainbow
        ldx #15
@pal:   lda green_tab,x
        sta GREEN,x
        lda bluered_tab,x
        sta BLUERED,x
        dex
        bpl @pal
        lda #<FB
        sta DISPADRL
        lda #>FB
        sta DISPADRH
        lda #$0d                ; colour, 4 bpp, DMA on
        sta DISPCTL
        ; clear top half (rows 0-50) to pen 0, bars in rows 51-101
        lda #<FB
        sta PTR
        lda #>FB
        sta PTR+1
        ldx #0                  ; row
@row:   ldy #0
@col:   cpx #51
        bcc @bg
        tya                     ; pen = column/5 (16 bars of 10 pixels)
        lsr a
        lsr a
        sta $FE
        ; pen*$11 via table
        phx
        ldx $FE
        cpx #16
        bcc @ok
        ldx #15
@ok:    lda dup_tab,x
        plx
        bra @put
@bg:    lda #0
@put:   sta (PTR),y
        iny
        cpy #80
        bne @col
        clc
        lda PTR
        adc #80
        sta PTR
        bcc @nc
        inc PTR+1
@nc:    inx
        cpx #102
        bne @row
        ; tone: channel 0, square wave
        lda #$00
        sta MSTEREO
        lda #$30
        sta AUD0VOL
        lda #$01
        sta AUD0FEED
        lda #$01
        sta AUD0SHFT
        lda #70                 ; period
        sta AUD0BKUP
        lda #$18                ; reload, count, 1 us
        sta AUD0CTLA
        stz FRAMES
        stz FRAMES+1
        stz BARX
        lda #1
        sta MOVING
        stz LASTSW
        lda #$ff
        sta INTRST

main:   ; wait for the end of a frame (timer 2 flag)
@w:     lda INTSET
        and #$04
        beq @w
        lda #$04
        sta INTRST
        inc FRAMES
        bne @n1
        inc FRAMES+1
@n1:    ; buttons: A = blue background, B = mute
        lda JOYSTICK
        and #$01                ; A (outer button) is bit 0
        beq @noa
        lda #$00
        sta GREEN
        lda #$a0
        sta BLUERED
        bra @adone
@noa:   lda #$03
        sta GREEN
        lda #$33
        sta BLUERED
@adone: lda JOYSTICK
        and #$02                ; B is bit 1
        beq @nob
        stz AUD0VOL
        bra @bdone
@nob:   lda #$30
        sta AUD0VOL
@bdone: lda SWITCHES
        and #$01                ; pause
        tax
        eor LASTSW
        and #$01
        beq @nopause
        txa
        beq @nopause            ; act on press only
        lda MOVING
        eor #1
        sta MOVING
@nopause:
        stx LASTSW
        lda MOVING
        beq main
        ; erase the bar's column in rows 0-50, move, draw
        lda #0
        jsr bar
        inc BARX
        lda BARX
        cmp #80
        bne @nw
        stz BARX
@nw:    lda #$11
        jsr bar
        jmp main

; write A to byte column BARX of rows 0-50
bar:    pha
        lda #<FB
        sta PTR
        lda #>FB
        sta PTR+1
        ldx #51
        ldy BARX
@b:     pla
        pha
        sta (PTR),y
        clc
        lda PTR
        adc #80
        sta PTR
        bcc @c
        inc PTR+1
@c:     dex
        bne @b
        pla
        rts

dup_tab:     .byte $00,$11,$22,$33,$44,$55,$66,$77,$88,$99,$aa,$bb,$cc,$dd,$ee,$ff
; GBR444: GREEN register = G, BLUERED = B<<4 | R
green_tab:   .byte $3,$f,$0,$4,$8,$c,$f,$f,$f,$c,$8,$4,$0,$0,$0,$8
bluered_tab: .byte $33,$ff,$0f,$0f,$0f,$0f,$0f,$0c,$08,$00,$00,$00,$40,$80,$f0,$f8
