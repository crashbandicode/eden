// Original firmware-free AArch64 probe. The NRO wrapper is added by the generator.
.text
b start
.org 0x80
start:
    adr x20, buffer
    mov x21, 0
loop:
    add x21, x21, 1
    str x21, [x20]
    mov x0, 0x1234
    mov x1, x20
    mov x2, 64
hit:
    nop
    movz x0, 0x4240
    movk x0, 0xf, lsl 16
    svc 0xb                       // SleepThread(1,000,000 ns)
    b loop
.org 0x1000
.org 0x2000
buffer:
    .quad 0
    .fill 56, 1, 0xa5
.org 0x3000
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
