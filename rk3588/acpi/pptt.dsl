/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20260408 (64-bit version)
 * Copyright (c) 2000 - 2026 Intel Corporation
 * 
 * Disassembly of pptt.dat
 *
 * ACPI Data Table [PPTT]
 *
 * Format: [HexOffset DecimalOffset ByteLength]  FieldName : FieldValue (in hex)
 */

[000h 0000 004h]                   Signature : "PPTT"    [Processor Properties Topology Table]
[004h 0004 004h]                Table Length : 00000220
[008h 0008 001h]                    Revision : 01
[009h 0009 001h]                    Checksum : 05
[00Ah 0010 006h]                      Oem ID : "RKCP  "
[010h 0016 008h]                Oem Table ID : "RK3588  "
[018h 0024 004h]                Oem Revision : 00000000
[01Ch 0028 004h]             Asl Compiler ID : "EDK2"
[020h 0032 004h]       Asl Compiler Revision : 00000000


[024h 0036 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[025h 0037 001h]                      Length : 18
[026h 0038 002h]                    Reserved : 0000
[028h 0040 004h]       Flags (decoded below) : 00000003
                            Physical package : 1
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[02Ch 0044 004h]                      Parent : 00000000
[030h 0048 004h]           ACPI Processor ID : 00000008
[034h 0052 004h]     Private Resource Number : 00000001
[038h 0056 004h]            Private Resource : 00000178

[03Ch 0060 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[03Dh 0061 001h]                      Length : 14
[03Eh 0062 002h]                    Reserved : 0000
[040h 0064 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[044h 0068 004h]                      Parent : 00000024
[048h 0072 004h]           ACPI Processor ID : 00000009
[04Ch 0076 004h]     Private Resource Number : 00000000

[050h 0080 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[051h 0081 001h]                      Length : 14
[052h 0082 002h]                    Reserved : 0000
[054h 0084 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[058h 0088 004h]                      Parent : 00000024
[05Ch 0092 004h]           ACPI Processor ID : 0000000A
[060h 0096 004h]     Private Resource Number : 00000000

[064h 0100 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[065h 0101 001h]                      Length : 14
[066h 0102 002h]                    Reserved : 0000
[068h 0104 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[06Ch 0108 004h]                      Parent : 00000024
[070h 0112 004h]           ACPI Processor ID : 0000000B
[074h 0116 004h]     Private Resource Number : 00000000

[078h 0120 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[079h 0121 001h]                      Length : 20
[07Ah 0122 002h]                    Reserved : 0000
[07Ch 0124 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[080h 0128 004h]                      Parent : 0000003C
[084h 0132 004h]           ACPI Processor ID : 00000000
[088h 0136 004h]     Private Resource Number : 00000003
[08Ch 0140 004h]            Private Resource : 00000190
[090h 0144 004h]            Private Resource : 000001A8
[094h 0148 004h]            Private Resource : 000001C0

[098h 0152 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[099h 0153 001h]                      Length : 20
[09Ah 0154 002h]                    Reserved : 0000
[09Ch 0156 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[0A0h 0160 004h]                      Parent : 0000003C
[0A4h 0164 004h]           ACPI Processor ID : 00000001
[0A8h 0168 004h]     Private Resource Number : 00000003
[0ACh 0172 004h]            Private Resource : 00000190
[0B0h 0176 004h]            Private Resource : 000001A8
[0B4h 0180 004h]            Private Resource : 000001C0

[0B8h 0184 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[0B9h 0185 001h]                      Length : 20
[0BAh 0186 002h]                    Reserved : 0000
[0BCh 0188 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[0C0h 0192 004h]                      Parent : 0000003C
[0C4h 0196 004h]           ACPI Processor ID : 00000002
[0C8h 0200 004h]     Private Resource Number : 00000003
[0CCh 0204 004h]            Private Resource : 00000190
[0D0h 0208 004h]            Private Resource : 000001A8
[0D4h 0212 004h]            Private Resource : 000001C0

[0D8h 0216 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[0D9h 0217 001h]                      Length : 20
[0DAh 0218 002h]                    Reserved : 0000
[0DCh 0220 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[0E0h 0224 004h]                      Parent : 0000003C
[0E4h 0228 004h]           ACPI Processor ID : 00000003
[0E8h 0232 004h]     Private Resource Number : 00000003
[0ECh 0236 004h]            Private Resource : 00000190
[0F0h 0240 004h]            Private Resource : 000001A8
[0F4h 0244 004h]            Private Resource : 000001C0

[0F8h 0248 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[0F9h 0249 001h]                      Length : 20
[0FAh 0250 002h]                    Reserved : 0000
[0FCh 0252 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[100h 0256 004h]                      Parent : 00000050
[104h 0260 004h]           ACPI Processor ID : 00000004
[108h 0264 004h]     Private Resource Number : 00000003
[10Ch 0268 004h]            Private Resource : 000001D8
[110h 0272 004h]            Private Resource : 000001F0
[114h 0276 004h]            Private Resource : 00000208

[118h 0280 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[119h 0281 001h]                      Length : 20
[11Ah 0282 002h]                    Reserved : 0000
[11Ch 0284 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[120h 0288 004h]                      Parent : 00000050
[124h 0292 004h]           ACPI Processor ID : 00000005
[128h 0296 004h]     Private Resource Number : 00000003
[12Ch 0300 004h]            Private Resource : 000001D8
[130h 0304 004h]            Private Resource : 000001F0
[134h 0308 004h]            Private Resource : 00000208

[138h 0312 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[139h 0313 001h]                      Length : 20
[13Ah 0314 002h]                    Reserved : 0000
[13Ch 0316 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[140h 0320 004h]                      Parent : 00000064
[144h 0324 004h]           ACPI Processor ID : 00000006
[148h 0328 004h]     Private Resource Number : 00000003
[14Ch 0332 004h]            Private Resource : 000001D8
[150h 0336 004h]            Private Resource : 000001F0
[154h 0340 004h]            Private Resource : 00000208

[158h 0344 001h]               Subtable Type : 00 [Processor Hierarchy Node]
[159h 0345 001h]                      Length : 20
[15Ah 0346 002h]                    Reserved : 0000
[15Ch 0348 004h]       Flags (decoded below) : 00000002
                            Physical package : 0
                     ACPI Processor ID valid : 1
                       Processor is a thread : 0
                              Node is a leaf : 0
                    Identical Implementation : 0
[160h 0352 004h]                      Parent : 00000064
[164h 0356 004h]           ACPI Processor ID : 00000007
[168h 0360 004h]     Private Resource Number : 00000003
[16Ch 0364 004h]            Private Resource : 000001D8
[170h 0368 004h]            Private Resource : 000001F0
[174h 0372 004h]            Private Resource : 00000208

[178h 0376 001h]               Subtable Type : 01 [Cache Type]
[179h 0377 001h]                      Length : 18
[17Ah 0378 002h]                    Reserved : 0000
[17Ch 0380 004h]       Flags (decoded below) : 0000007F
                                  Size valid : 1
                        Number of Sets valid : 1
                         Associativity valid : 1
                       Allocation Type valid : 1
                            Cache Type valid : 1
                          Write Policy valid : 1
                             Line Size valid : 1
                              Cache ID valid : 0
[180h 0384 004h]         Next Level of Cache : 00000000
[184h 0388 004h]                        Size : 00300000
[188h 0392 004h]              Number of Sets : 00001000
[18Ch 0396 001h]               Associativity : 0C
[18Dh 0397 001h]                  Attributes : 0A
                             Allocation Type : 2
                                  Cache Type : 2
                                Write Policy : 0
[18Eh 0398 002h]                   Line Size : 0040

[190h 0400 001h]               Subtable Type : 01 [Cache Type]
[191h 0401 001h]                      Length : 18
[192h 0402 002h]                    Reserved : 0000
[194h 0404 004h]       Flags (decoded below) : 0000007F
                                  Size valid : 1
                        Number of Sets valid : 1
                         Associativity valid : 1
                       Allocation Type valid : 1
                            Cache Type valid : 1
                          Write Policy valid : 1
                             Line Size valid : 1
                              Cache ID valid : 0
[198h 0408 004h]         Next Level of Cache : 000001C0
[19Ch 0412 004h]                        Size : 00008000
[1A0h 0416 004h]              Number of Sets : 00000080
[1A4h 0420 001h]               Associativity : 04
[1A5h 0421 001h]                  Attributes : 02
                             Allocation Type : 2
                                  Cache Type : 0
                                Write Policy : 0
[1A6h 0422 002h]                   Line Size : 0040

[1A8h 0424 001h]               Subtable Type : 01 [Cache Type]
[1A9h 0425 001h]                      Length : 18
[1AAh 0426 002h]                    Reserved : 0000
[1ACh 0428 004h]       Flags (decoded below) : 0000007F
                                  Size valid : 1
                        Number of Sets valid : 1
                         Associativity valid : 1
                       Allocation Type valid : 1
                            Cache Type valid : 1
                          Write Policy valid : 1
                             Line Size valid : 1
                              Cache ID valid : 0
[1B0h 0432 004h]         Next Level of Cache : 000001C0
[1B4h 0436 004h]                        Size : 00008000
[1B8h 0440 004h]              Number of Sets : 00000080
[1BCh 0444 001h]               Associativity : 04
[1BDh 0445 001h]                  Attributes : 04
                             Allocation Type : 0
                                  Cache Type : 1
                                Write Policy : 0
[1BEh 0446 002h]                   Line Size : 0040

[1C0h 0448 001h]               Subtable Type : 01 [Cache Type]
[1C1h 0449 001h]                      Length : 18
[1C2h 0450 002h]                    Reserved : 0000
[1C4h 0452 004h]       Flags (decoded below) : 0000007F
                                  Size valid : 1
                        Number of Sets valid : 1
                         Associativity valid : 1
                       Allocation Type valid : 1
                            Cache Type valid : 1
                          Write Policy valid : 1
                             Line Size valid : 1
                              Cache ID valid : 0
[1C8h 0456 004h]         Next Level of Cache : 00000000
[1CCh 0460 004h]                        Size : 00020000
[1D0h 0464 004h]              Number of Sets : 00000200
[1D4h 0468 001h]               Associativity : 04
[1D5h 0469 001h]                  Attributes : 0A
                             Allocation Type : 2
                                  Cache Type : 2
                                Write Policy : 0
[1D6h 0470 002h]                   Line Size : 0040

[1D8h 0472 001h]               Subtable Type : 01 [Cache Type]
[1D9h 0473 001h]                      Length : 18
[1DAh 0474 002h]                    Reserved : 0000
[1DCh 0476 004h]       Flags (decoded below) : 0000007F
                                  Size valid : 1
                        Number of Sets valid : 1
                         Associativity valid : 1
                       Allocation Type valid : 1
                            Cache Type valid : 1
                          Write Policy valid : 1
                             Line Size valid : 1
                              Cache ID valid : 0
[1E0h 0480 004h]         Next Level of Cache : 00000208
[1E4h 0484 004h]                        Size : 00010000
[1E8h 0488 004h]              Number of Sets : 00000100
[1ECh 0492 001h]               Associativity : 04
[1EDh 0493 001h]                  Attributes : 02
                             Allocation Type : 2
                                  Cache Type : 0
                                Write Policy : 0
[1EEh 0494 002h]                   Line Size : 0040

[1F0h 0496 001h]               Subtable Type : 01 [Cache Type]
[1F1h 0497 001h]                      Length : 18
[1F2h 0498 002h]                    Reserved : 0000
[1F4h 0500 004h]       Flags (decoded below) : 0000007F
                                  Size valid : 1
                        Number of Sets valid : 1
                         Associativity valid : 1
                       Allocation Type valid : 1
                            Cache Type valid : 1
                          Write Policy valid : 1
                             Line Size valid : 1
                              Cache ID valid : 0
[1F8h 0504 004h]         Next Level of Cache : 00000208
[1FCh 0508 004h]                        Size : 00010000
[200h 0512 004h]              Number of Sets : 00000100
[204h 0516 001h]               Associativity : 04
[205h 0517 001h]                  Attributes : 04
                             Allocation Type : 0
                                  Cache Type : 1
                                Write Policy : 0
[206h 0518 002h]                   Line Size : 0040

[208h 0520 001h]               Subtable Type : 01 [Cache Type]
[209h 0521 001h]                      Length : 18
[20Ah 0522 002h]                    Reserved : 0000
[20Ch 0524 004h]       Flags (decoded below) : 0000007F
                                  Size valid : 1
                        Number of Sets valid : 1
                         Associativity valid : 1
                       Allocation Type valid : 1
                            Cache Type valid : 1
                          Write Policy valid : 1
                             Line Size valid : 1
                              Cache ID valid : 0
[210h 0528 004h]         Next Level of Cache : 00000000
[214h 0532 004h]                        Size : 00080000
[218h 0536 004h]              Number of Sets : 00000400
[21Ch 0540 001h]               Associativity : 08
[21Dh 0541 001h]                  Attributes : 0A
                             Allocation Type : 2
                                  Cache Type : 2
                                Write Policy : 0
[21Eh 0542 002h]                   Line Size : 0040

Raw Table Data: Length 544 (0x220)

    0000: 50 50 54 54 20 02 00 00 01 05 52 4B 43 50 20 20  // PPTT .....RKCP  
    0010: 52 4B 33 35 38 38 20 20 00 00 00 00 45 44 4B 32  // RK3588  ....EDK2
    0020: 00 00 00 00 00 18 00 00 03 00 00 00 00 00 00 00  // ................
    0030: 08 00 00 00 01 00 00 00 78 01 00 00 00 14 00 00  // ........x.......
    0040: 02 00 00 00 24 00 00 00 09 00 00 00 00 00 00 00  // ....$...........
    0050: 00 14 00 00 02 00 00 00 24 00 00 00 0A 00 00 00  // ........$.......
    0060: 00 00 00 00 00 14 00 00 02 00 00 00 24 00 00 00  // ............$...
    0070: 0B 00 00 00 00 00 00 00 00 20 00 00 02 00 00 00  // ......... ......
    0080: 3C 00 00 00 00 00 00 00 03 00 00 00 90 01 00 00  // <...............
    0090: A8 01 00 00 C0 01 00 00 00 20 00 00 02 00 00 00  // ......... ......
    00A0: 3C 00 00 00 01 00 00 00 03 00 00 00 90 01 00 00  // <...............
    00B0: A8 01 00 00 C0 01 00 00 00 20 00 00 02 00 00 00  // ......... ......
    00C0: 3C 00 00 00 02 00 00 00 03 00 00 00 90 01 00 00  // <...............
    00D0: A8 01 00 00 C0 01 00 00 00 20 00 00 02 00 00 00  // ......... ......
    00E0: 3C 00 00 00 03 00 00 00 03 00 00 00 90 01 00 00  // <...............
    00F0: A8 01 00 00 C0 01 00 00 00 20 00 00 02 00 00 00  // ......... ......
    0100: 50 00 00 00 04 00 00 00 03 00 00 00 D8 01 00 00  // P...............
    0110: F0 01 00 00 08 02 00 00 00 20 00 00 02 00 00 00  // ......... ......
    0120: 50 00 00 00 05 00 00 00 03 00 00 00 D8 01 00 00  // P...............
    0130: F0 01 00 00 08 02 00 00 00 20 00 00 02 00 00 00  // ......... ......
    0140: 64 00 00 00 06 00 00 00 03 00 00 00 D8 01 00 00  // d...............
    0150: F0 01 00 00 08 02 00 00 00 20 00 00 02 00 00 00  // ......... ......
    0160: 64 00 00 00 07 00 00 00 03 00 00 00 D8 01 00 00  // d...............
    0170: F0 01 00 00 08 02 00 00 01 18 00 00 7F 00 00 00  // ................
    0180: 00 00 00 00 00 00 30 00 00 10 00 00 0C 0A 40 00  // ......0.......@.
    0190: 01 18 00 00 7F 00 00 00 C0 01 00 00 00 80 00 00  // ................
    01A0: 80 00 00 00 04 02 40 00 01 18 00 00 7F 00 00 00  // ......@.........
    01B0: C0 01 00 00 00 80 00 00 80 00 00 00 04 04 40 00  // ..............@.
    01C0: 01 18 00 00 7F 00 00 00 00 00 00 00 00 00 02 00  // ................
    01D0: 00 02 00 00 04 0A 40 00 01 18 00 00 7F 00 00 00  // ......@.........
    01E0: 08 02 00 00 00 00 01 00 00 01 00 00 04 02 40 00  // ..............@.
    01F0: 01 18 00 00 7F 00 00 00 08 02 00 00 00 00 01 00  // ................
    0200: 00 01 00 00 04 04 40 00 01 18 00 00 7F 00 00 00  // ......@.........
    0210: 00 00 00 00 00 00 08 00 00 04 00 00 08 0A 40 00  // ..............@.
