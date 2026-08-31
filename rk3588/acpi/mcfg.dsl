/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20260408 (64-bit version)
 * Copyright (c) 2000 - 2026 Intel Corporation
 * 
 * Disassembly of mcfg.dat
 *
 * ACPI Data Table [MCFG]
 *
 * Format: [HexOffset DecimalOffset ByteLength]  FieldName : FieldValue (in hex)
 */

[000h 0000 004h]                   Signature : "MCFG"    [Memory Mapped Configuration Table]
[004h 0004 004h]                Table Length : 0000007C
[008h 0008 001h]                    Revision : 01
[009h 0009 001h]                    Checksum : 79
[00Ah 0010 006h]                      Oem ID : "RKCP  "
[010h 0016 008h]                Oem Table ID : "RK3588  "
[018h 0024 004h]                Oem Revision : 00000000
[01Ch 0028 004h]             Asl Compiler ID : "EDK2"
[020h 0032 004h]       Asl Compiler Revision : 00000000

[024h 0036 008h]                    Reserved : 0000000000000000

[02Ch 0044 008h]                Base Address : 0000000900008000
[034h 0052 002h]        Segment Group Number : 0000
[036h 0054 001h]            Start Bus Number : 01
[037h 0055 001h]              End Bus Number : 01
[038h 0056 004h]                    Reserved : 00000000

[03Ch 0060 008h]                Base Address : 0000000940000000
[044h 0068 002h]        Segment Group Number : 0001
[046h 0070 001h]            Start Bus Number : 01
[047h 0071 001h]              End Bus Number : 01
[048h 0072 004h]                    Reserved : 00000000

[04Ch 0076 008h]                Base Address : 0000000980008000
[054h 0084 002h]        Segment Group Number : 0002
[056h 0086 001h]            Start Bus Number : 01
[057h 0087 001h]              End Bus Number : 01
[058h 0088 004h]                    Reserved : 00000000

[05Ch 0092 008h]                Base Address : 00000009C0008000
[064h 0100 002h]        Segment Group Number : 0003
[066h 0102 001h]            Start Bus Number : 01
[067h 0103 001h]              End Bus Number : 01
[068h 0104 004h]                    Reserved : 00000000

[06Ch 0108 008h]                Base Address : 0000000A00008000
[074h 0116 002h]        Segment Group Number : 0004
[076h 0118 001h]            Start Bus Number : 01
[077h 0119 001h]              End Bus Number : 01
[078h 0120 004h]                    Reserved : 00000000

Raw Table Data: Length 124 (0x7C)

    0000: 4D 43 46 47 7C 00 00 00 01 79 52 4B 43 50 20 20  // MCFG|....yRKCP  
    0010: 52 4B 33 35 38 38 20 20 00 00 00 00 45 44 4B 32  // RK3588  ....EDK2
    0020: 00 00 00 00 00 00 00 00 00 00 00 00 00 80 00 00  // ................
    0030: 09 00 00 00 00 00 01 01 00 00 00 00 00 00 00 40  // ...............@
    0040: 09 00 00 00 01 00 01 01 00 00 00 00 00 80 00 80  // ................
    0050: 09 00 00 00 02 00 01 01 00 00 00 00 00 80 00 C0  // ................
    0060: 09 00 00 00 03 00 01 01 00 00 00 00 00 80 00 00  // ................
    0070: 0A 00 00 00 04 00 01 01 00 00 00 00              // ............
