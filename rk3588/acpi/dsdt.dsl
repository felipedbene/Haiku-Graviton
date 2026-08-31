/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20260408 (64-bit version)
 * Copyright (c) 2000 - 2026 Intel Corporation
 * 
 * Disassembling to symbolic ASL+ operators
 *
 * Disassembly of dsdt.dat
 *
 * Original Table Header:
 *     Signature        "DSDT"
 *     Length           0x000036C8 (14024)
 *     Revision         0x02
 *     Checksum         0xDE
 *     OEM ID           "RKCP  "
 *     OEM Table ID     "RK3588  "
 *     OEM Revision     0x00000002 (2)
 *     Compiler ID      "INTL"
 *     Compiler Version 0x20230628 (539166248)
 */
DefinitionBlock ("", "DSDT", 2, "RKCP  ", "RK3588  ", 0x00000002)
{
    Scope (_SB)
    {
        Scope (\_SB)
        {
            Device (SCMI)
            {
                Name (_HID, "PNP0C02" /* PNP Motherboard Resources */)  // _HID: Hardware ID
                Name (_UID, Zero)  // _UID: Unique ID
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x0E)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0xF0, 0x10, 0x00,  // ........
                        /* 0008 */  0x00, 0x01, 0x00, 0x00, 0x79, 0x00               // ....y.
                    })
                    Return (RBUF) /* \_SB_.SCMI._CRS.RBUF */
                }

                Method (CLRG, 1, Serialized)
                {
                    Name (PBUF, Buffer (0x04){})
                    CreateDWordField (PBUF, Zero, PCID)
                    PCID = Arg0
                    Local0 = SMT (0x14, 0x06, PBUF)
                    CreateDWordField (Local0, Zero, RSTA)
                    CreateDWordField (Local0, 0x04, RRTL)
                    CreateDWordField (Local0, 0x08, RRTH)
                    Local1 = Package (0x02)
                        {
                            Zero, 
                            Zero
                        }
                    Local1 [Zero] = RSTA /* \_SB_.SCMI.CLRG.RSTA */
                    If ((RSTA == Zero))
                    {
                        Local1 [One] = ((RRTH << 0x20) | RRTL) /* \_SB_.SCMI.CLRG.RRTL */
                    }

                    Return (Local1)
                }

                Method (CLRS, 2, Serialized)
                {
                    Name (PBUF, Buffer (0x10){})
                    CreateDWordField (PBUF, Zero, PFLG)
                    CreateDWordField (PBUF, 0x04, PCID)
                    CreateDWordField (PBUF, 0x08, PRTL)
                    CreateDWordField (PBUF, 0x0C, PRTH)
                    PFLG = Zero
                    PCID = Arg0
                    PRTL = Arg1
                    PRTH = (Arg1 >> 0x20)
                    Local0 = SMT (0x14, 0x05, PBUF)
                    CreateDWordField (Local0, Zero, RSTA)
                    Local1 = Package (0x01)
                        {
                            Zero
                        }
                    Local1 [Zero] = RSTA /* \_SB_.SCMI.CLRS.RSTA */
                    Return (Local1)
                }

                Method (CLCS, 2, Serialized)
                {
                    Name (PBUF, Buffer (0x08){})
                    CreateDWordField (PBUF, Zero, PCID)
                    CreateDWordField (PBUF, 0x04, PATR)
                    PCID = Arg0
                    PATR = Arg1
                    Local0 = SMT (0x14, 0x07, PBUF)
                    CreateDWordField (Local0, Zero, RSTA)
                    Local1 = Package (0x01)
                        {
                            Zero
                        }
                    Local1 [Zero] = RSTA /* \_SB_.SCMI.CLCS.RSTA */
                    Return (Local1)
                }

                Method (VDLG, 1, Serialized)
                {
                    Name (PBUF, Buffer (0x04){})
                    CreateDWordField (PBUF, Zero, PDID)
                    PDID = Arg0
                    Local0 = SMT (0x17, 0x08, PBUF)
                    CreateDWordField (Local0, Zero, RSTA)
                    CreateDWordField (Local0, 0x04, RVOL)
                    Local1 = Package (0x02)
                        {
                            Zero, 
                            Zero
                        }
                    Local1 [Zero] = RSTA /* \_SB_.SCMI.VDLG.RSTA */
                    If ((RSTA == Zero))
                    {
                        Local1 [One] = RVOL /* \_SB_.SCMI.VDLG.RVOL */
                    }

                    Return (Local1)
                }

                Method (VDLS, 2, Serialized)
                {
                    Name (PBUF, Buffer (0x0C){})
                    CreateDWordField (PBUF, Zero, PDID)
                    CreateDWordField (PBUF, 0x04, PFLG)
                    CreateDWordField (PBUF, 0x08, PVOL)
                    PDID = Arg0
                    PFLG = Zero
                    PVOL = Arg1
                    Local0 = SMT (0x17, 0x07, PBUF)
                    CreateDWordField (Local0, Zero, RSTA)
                    Local1 = Package (0x01)
                        {
                            Zero
                        }
                    Local1 [Zero] = RSTA /* \_SB_.SCMI.VDLS.RSTA */
                    Return (Local1)
                }

                OperationRegion (SHM, SystemMemory, 0x0010F000, 0x0100)
                Field (SHM, ByteAcc, NoLock, Preserve)
                {
                    Offset (0x04), 
                    SCHS,   32, 
                    Offset (0x10), 
                    SCHF,   32, 
                    SLEN,   32, 
                    SMSH,   32, 
                    SPLD,   736
                }

                OperationRegion (DBEL, SystemMemory, 0xFEC60030, 0x08)
                Field (DBEL, DWordAcc, NoLock, Preserve)
                {
                    DCMD,   32, 
                    DDAT,   32
                }

                Method (FREE, 0, Serialized)
                {
                    Local0 = 0x4E20
                    While ((Local0 > Zero))
                    {
                        If (((SCHS & One) != Zero))
                        {
                            Return (One)
                        }

                        Sleep (One)
                        Local0--
                    }

                    Return (Zero)
                }

                Method (SMT, 3, Serialized)
                {
                    Local0 = Buffer (0x04)
                        {
                             0x01                                             // .
                        }
                    If ((FREE () == Zero))
                    {
                        Return (Local0)
                    }

                    Name (MSGH, Buffer (0x04){})
                    CreateField (MSGH, 0x0A, 0x08, PRTI)
                    CreateField (MSGH, 0x08, 0x02, MSGT)
                    CreateField (MSGH, Zero, 0x08, MSGI)
                    PRTI = Arg0
                    MSGT = Zero
                    MSGI = Arg1
                    SMSH = MSGH /* \_SB_.SCMI.SMT_.MSGH */
                    SCHS = Zero
                    SCHF = Zero
                    SLEN = (0x04 + SizeOf (Arg2))
                    SPLD = Arg2
                    DCMD = Zero
                    DDAT = Zero
                    If (((FREE () == Zero) || ((SCHS & 0x02) != Zero)))
                    {
                        Return (Local0)
                    }

                    Local0 = SPLD /* \_SB_.SCMI.SPLD */
                    Return (Local0)
                }
            }
        }

        Device (PKG0)
        {
            Name (_HID, "ACPI0010" /* Processor Container Device */)  // _HID: Hardware ID
            Name (_UID, 0x08)  // _UID: Unique ID
            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }

            Device (CLU0)
            {
                Name (_HID, "ACPI0010" /* Processor Container Device */)  // _HID: Hardware ID
                Name (_UID, 0x09)  // _UID: Unique ID
                Method (_STA, 0, NotSerialized)  // _STA: Status
                {
                    Return (0x0F)
                }

                Device (CPU0)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, Zero)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }

                Device (CPU1)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, One)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }

                Device (CPU2)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, 0x02)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }

                Device (CPU3)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, 0x03)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }
            }

            Device (CLU1)
            {
                Name (_HID, "ACPI0010" /* Processor Container Device */)  // _HID: Hardware ID
                Name (_UID, 0x0A)  // _UID: Unique ID
                Method (_STA, 0, NotSerialized)  // _STA: Status
                {
                    Return (0x0F)
                }

                Device (CPU4)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, 0x04)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }

                Device (CPU5)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, 0x05)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }
            }

            Device (CLU2)
            {
                Name (_HID, "ACPI0010" /* Processor Container Device */)  // _HID: Hardware ID
                Name (_UID, 0x0B)  // _UID: Unique ID
                Method (_STA, 0, NotSerialized)  // _STA: Status
                {
                    Return (0x0F)
                }

                Device (CPU6)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, 0x06)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }

                Device (CPU7)
                {
                    Name (_HID, "ACPI0007" /* Processor Device */)  // _HID: Hardware ID
                    Name (_UID, 0x07)  // _UID: Unique ID
                    Method (_STA, 0, NotSerialized)  // _STA: Status
                    {
                        Return (0x0F)
                    }
                }
            }
        }

        Scope (\_SB)
        {
            Name (PBMI, 0x0001)
            Name (PBMA, 0x0001)
            Device (PCI0)
            {
                Name (_HID, "PNP0A08" /* PCI Express Bus */)  // _HID: Hardware ID
                Name (_CID, "PNP0A03" /* PCI Bus */)  // _CID: Compatible ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_UID, Zero)  // _UID: Unique ID
                Name (_SEG, Zero)  // _SEG: PCI Segment
                Method (_BBN, 0, NotSerialized)  // _BBN: BIOS Bus Number
                {
                    Return (PBMI) /* \_SB_.PBMI */
                }

                Name (_STA, 0x0F)  // _STA: Status
                Name (_PRT, Package (0x04)  // _PRT: PCI Routing Table
                {
                    Package (0x04)
                    {
                        0xFFFF, 
                        Zero, 
                        Zero, 
                        0x0124
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        One, 
                        Zero, 
                        0x0124
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x02, 
                        Zero, 
                        0x0124
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x03, 
                        Zero, 
                        0x0124
                    }
                })
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x88)
                    {
                        /* 0000 */  0x88, 0x0D, 0x00, 0x02, 0x0C, 0x00, 0x00, 0x00,  // ........
                        /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0010 */  0x87, 0x17, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // ........
                        /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0028 */  0x00, 0x00, 0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01,  // ...+....
                        /* 0030 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0038 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0040 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0048 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0050 */  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0058 */  0x8A, 0x2B, 0x00, 0x01, 0x0C, 0x03, 0x00, 0x00,  // .+......
                        /* 0060 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0068 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0070 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0078 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0080 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                    })
                    CreateWordField (RBUF, 0x08, MI00)
                    CreateWordField (RBUF, 0x0A, MA00)
                    CreateWordField (RBUF, 0x0C, TR00)
                    CreateWordField (RBUF, 0x0E, LE00)
                    LE00 = ((PBMA - PBMI) + One)
                    MI00 = PBMI /* \_SB_.PBMI */
                    TR00 = Zero
                    MA00 = ((MI00 + LE00) - One)
                    CreateDWordField (RBUF, 0x1A, MI01)
                    CreateDWordField (RBUF, 0x1E, MA01)
                    CreateDWordField (RBUF, 0x22, TR01)
                    CreateDWordField (RBUF, 0x26, LE01)
                    LE01 = 0x01000000
                    MI01 = 0xF0000000
                    TR01 = Zero
                    MA01 = ((MI01 + LE01) - One)
                    CreateQWordField (RBUF, 0x38, MI02)
                    CreateQWordField (RBUF, 0x40, MA02)
                    CreateQWordField (RBUF, 0x48, TR02)
                    CreateQWordField (RBUF, 0x50, LE02)
                    LE02 = 0x2FFF0000
                    MI02 = 0x0000000910000000
                    TR02 = Zero
                    MA02 = ((MI02 + LE02) - One)
                    CreateQWordField (RBUF, 0x66, MI03)
                    CreateQWordField (RBUF, 0x6E, MA03)
                    CreateQWordField (RBUF, 0x76, TR03)
                    CreateQWordField (RBUF, 0x7E, LE03)
                    LE03 = 0x00010000
                    MI03 = Zero
                    TR03 = 0x000000093FFF0000
                    MA03 = ((MI03 + LE03) - One)
                    Return (RBUF) /* \_SB_.PCI0._CRS.RBUF */
                }

                Device (RES0)
                {
                    Name (_HID, "AMZN0001")  // _HID: Hardware ID
                    Name (_CID, "PNP0C02" /* PNP Motherboard Resources */)  // _CID: Compatible ID
                    Name (_UID, Zero)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x00400000
                        MI00 = 0x0000000A40000000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI0.RES0._CRS.RBUF */
                    }
                }

                Device (RES1)
                {
                    Name (_HID, "PNP0C02" /* PNP Motherboard Resources */)  // _HID: Hardware ID
                    Name (_UID, One)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x10000000
                        MI00 = 0x0000000900000000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI0.RES1._CRS.RBUF */
                    }
                }

                Name (SUPP, Zero)
                Name (CTRL, Zero)
                Method (_OSC, 4, NotSerialized)  // _OSC: Operating System Capabilities
                {
                    If ((Arg0 == ToUUID ("33db4d5b-1ff7-401c-9657-7441c03dd766") /* PCI Host Bridge Device */))
                    {
                        CreateDWordField (Arg3, Zero, CDW1)
                        CreateDWordField (Arg3, 0x04, CDW2)
                        CreateDWordField (Arg3, 0x08, CDW3)
                        SUPP = CDW2 /* \_SB_.PCI0._OSC.CDW2 */
                        CTRL = CDW3 /* \_SB_.PCI0._OSC.CDW3 */
                        CTRL &= 0x1E
                        CTRL &= 0x1D
                        If ((Arg1 != One))
                        {
                            CDW1 |= 0x08
                        }

                        If ((CDW3 != CTRL))
                        {
                            CDW1 |= 0x10
                        }

                        CDW3 = CTRL /* \_SB_.PCI0.CTRL */
                        Return (Arg3)
                    }
                    Else
                    {
                        CDW1 |= 0x04
                        Return (Arg3)
                    }
                }
            }

            Device (PCI1)
            {
                Name (_HID, "PNP0A08" /* PCI Express Bus */)  // _HID: Hardware ID
                Name (_CID, "PNP0A03" /* PCI Bus */)  // _CID: Compatible ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_UID, One)  // _UID: Unique ID
                Name (_SEG, One)  // _SEG: PCI Segment
                Method (_BBN, 0, NotSerialized)  // _BBN: BIOS Bus Number
                {
                    Return (PBMI) /* \_SB_.PBMI */
                }

                Name (_STA, 0x0F)  // _STA: Status
                Name (_PRT, Package (0x04)  // _PRT: PCI Routing Table
                {
                    Package (0x04)
                    {
                        0xFFFF, 
                        Zero, 
                        Zero, 
                        0x011F
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        One, 
                        Zero, 
                        0x011F
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x02, 
                        Zero, 
                        0x011F
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x03, 
                        Zero, 
                        0x011F
                    }
                })
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x88)
                    {
                        /* 0000 */  0x88, 0x0D, 0x00, 0x02, 0x0C, 0x00, 0x00, 0x00,  // ........
                        /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0010 */  0x87, 0x17, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // ........
                        /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0028 */  0x00, 0x00, 0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01,  // ...+....
                        /* 0030 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0038 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0040 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0048 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0050 */  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0058 */  0x8A, 0x2B, 0x00, 0x01, 0x0C, 0x03, 0x00, 0x00,  // .+......
                        /* 0060 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0068 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0070 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0078 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0080 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                    })
                    CreateWordField (RBUF, 0x08, MI00)
                    CreateWordField (RBUF, 0x0A, MA00)
                    CreateWordField (RBUF, 0x0C, TR00)
                    CreateWordField (RBUF, 0x0E, LE00)
                    LE00 = ((PBMA - PBMI) + One)
                    MI00 = PBMI /* \_SB_.PBMI */
                    TR00 = Zero
                    MA00 = ((MI00 + LE00) - One)
                    CreateDWordField (RBUF, 0x1A, MI01)
                    CreateDWordField (RBUF, 0x1E, MA01)
                    CreateDWordField (RBUF, 0x22, TR01)
                    CreateDWordField (RBUF, 0x26, LE01)
                    LE01 = 0x01000000
                    MI01 = 0xF1000000
                    TR01 = Zero
                    MA01 = ((MI01 + LE01) - One)
                    CreateQWordField (RBUF, 0x38, MI02)
                    CreateQWordField (RBUF, 0x40, MA02)
                    CreateQWordField (RBUF, 0x48, TR02)
                    CreateQWordField (RBUF, 0x50, LE02)
                    LE02 = 0x2FFF0000
                    MI02 = 0x0000000950000000
                    TR02 = Zero
                    MA02 = ((MI02 + LE02) - One)
                    CreateQWordField (RBUF, 0x66, MI03)
                    CreateQWordField (RBUF, 0x6E, MA03)
                    CreateQWordField (RBUF, 0x76, TR03)
                    CreateQWordField (RBUF, 0x7E, LE03)
                    LE03 = 0x00010000
                    MI03 = Zero
                    TR03 = 0x000000097FFF0000
                    MA03 = ((MI03 + LE03) - One)
                    Return (RBUF) /* \_SB_.PCI1._CRS.RBUF */
                }

                Device (RES0)
                {
                    Name (_HID, "AMZN0001")  // _HID: Hardware ID
                    Name (_CID, "PNP0C02" /* PNP Motherboard Resources */)  // _CID: Compatible ID
                    Name (_UID, One)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x00400000
                        MI00 = 0x0000000A40400000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI1.RES0._CRS.RBUF */
                    }
                }

                Device (RES1)
                {
                    Name (_HID, "PNP0C02" /* PNP Motherboard Resources */)  // _HID: Hardware ID
                    Name (_UID, 0x02)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x10000000
                        MI00 = 0x0000000940000000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI1.RES1._CRS.RBUF */
                    }
                }

                Name (SUPP, Zero)
                Name (CTRL, Zero)
                Method (_OSC, 4, NotSerialized)  // _OSC: Operating System Capabilities
                {
                    If ((Arg0 == ToUUID ("33db4d5b-1ff7-401c-9657-7441c03dd766") /* PCI Host Bridge Device */))
                    {
                        CreateDWordField (Arg3, Zero, CDW1)
                        CreateDWordField (Arg3, 0x04, CDW2)
                        CreateDWordField (Arg3, 0x08, CDW3)
                        SUPP = CDW2 /* \_SB_.PCI1._OSC.CDW2 */
                        CTRL = CDW3 /* \_SB_.PCI1._OSC.CDW3 */
                        CTRL &= 0x1E
                        CTRL &= 0x1D
                        If ((Arg1 != One))
                        {
                            CDW1 |= 0x08
                        }

                        If ((CDW3 != CTRL))
                        {
                            CDW1 |= 0x10
                        }

                        CDW3 = CTRL /* \_SB_.PCI1.CTRL */
                        Return (Arg3)
                    }
                    Else
                    {
                        CDW1 |= 0x04
                        Return (Arg3)
                    }
                }
            }

            Device (PCI2)
            {
                Name (_HID, "PNP0A08" /* PCI Express Bus */)  // _HID: Hardware ID
                Name (_CID, "PNP0A03" /* PCI Bus */)  // _CID: Compatible ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_UID, 0x02)  // _UID: Unique ID
                Name (_SEG, 0x02)  // _SEG: PCI Segment
                Method (_BBN, 0, NotSerialized)  // _BBN: BIOS Bus Number
                {
                    Return (PBMI) /* \_SB_.PBMI */
                }

                Name (_STA, 0x0F)  // _STA: Status
                Name (_PRT, Package (0x04)  // _PRT: PCI Routing Table
                {
                    Package (0x04)
                    {
                        0xFFFF, 
                        Zero, 
                        Zero, 
                        0x0110
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        One, 
                        Zero, 
                        0x0110
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x02, 
                        Zero, 
                        0x0110
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x03, 
                        Zero, 
                        0x0110
                    }
                })
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x88)
                    {
                        /* 0000 */  0x88, 0x0D, 0x00, 0x02, 0x0C, 0x00, 0x00, 0x00,  // ........
                        /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0010 */  0x87, 0x17, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // ........
                        /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0028 */  0x00, 0x00, 0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01,  // ...+....
                        /* 0030 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0038 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0040 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0048 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0050 */  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0058 */  0x8A, 0x2B, 0x00, 0x01, 0x0C, 0x03, 0x00, 0x00,  // .+......
                        /* 0060 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0068 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0070 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0078 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0080 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                    })
                    CreateWordField (RBUF, 0x08, MI00)
                    CreateWordField (RBUF, 0x0A, MA00)
                    CreateWordField (RBUF, 0x0C, TR00)
                    CreateWordField (RBUF, 0x0E, LE00)
                    LE00 = ((PBMA - PBMI) + One)
                    MI00 = PBMI /* \_SB_.PBMI */
                    TR00 = Zero
                    MA00 = ((MI00 + LE00) - One)
                    CreateDWordField (RBUF, 0x1A, MI01)
                    CreateDWordField (RBUF, 0x1E, MA01)
                    CreateDWordField (RBUF, 0x22, TR01)
                    CreateDWordField (RBUF, 0x26, LE01)
                    LE01 = 0x01000000
                    MI01 = 0xF2000000
                    TR01 = Zero
                    MA01 = ((MI01 + LE01) - One)
                    CreateQWordField (RBUF, 0x38, MI02)
                    CreateQWordField (RBUF, 0x40, MA02)
                    CreateQWordField (RBUF, 0x48, TR02)
                    CreateQWordField (RBUF, 0x50, LE02)
                    LE02 = 0x2FFF0000
                    MI02 = 0x0000000990000000
                    TR02 = Zero
                    MA02 = ((MI02 + LE02) - One)
                    CreateQWordField (RBUF, 0x66, MI03)
                    CreateQWordField (RBUF, 0x6E, MA03)
                    CreateQWordField (RBUF, 0x76, TR03)
                    CreateQWordField (RBUF, 0x7E, LE03)
                    LE03 = 0x00010000
                    MI03 = Zero
                    TR03 = 0x00000009BFFF0000
                    MA03 = ((MI03 + LE03) - One)
                    Return (RBUF) /* \_SB_.PCI2._CRS.RBUF */
                }

                Device (RES0)
                {
                    Name (_HID, "AMZN0001")  // _HID: Hardware ID
                    Name (_CID, "PNP0C02" /* PNP Motherboard Resources */)  // _CID: Compatible ID
                    Name (_UID, 0x02)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x00400000
                        MI00 = 0x0000000A40800000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI2.RES0._CRS.RBUF */
                    }
                }

                Device (RES1)
                {
                    Name (_HID, "PNP0C02" /* PNP Motherboard Resources */)  // _HID: Hardware ID
                    Name (_UID, 0x03)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x10000000
                        MI00 = 0x0000000980000000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI2.RES1._CRS.RBUF */
                    }
                }

                Name (SUPP, Zero)
                Name (CTRL, Zero)
                Method (_OSC, 4, NotSerialized)  // _OSC: Operating System Capabilities
                {
                    If ((Arg0 == ToUUID ("33db4d5b-1ff7-401c-9657-7441c03dd766") /* PCI Host Bridge Device */))
                    {
                        CreateDWordField (Arg3, Zero, CDW1)
                        CreateDWordField (Arg3, 0x04, CDW2)
                        CreateDWordField (Arg3, 0x08, CDW3)
                        SUPP = CDW2 /* \_SB_.PCI2._OSC.CDW2 */
                        CTRL = CDW3 /* \_SB_.PCI2._OSC.CDW3 */
                        CTRL &= 0x1E
                        CTRL &= 0x1D
                        If ((Arg1 != One))
                        {
                            CDW1 |= 0x08
                        }

                        If ((CDW3 != CTRL))
                        {
                            CDW1 |= 0x10
                        }

                        CDW3 = CTRL /* \_SB_.PCI2.CTRL */
                        Return (Arg3)
                    }
                    Else
                    {
                        CDW1 |= 0x04
                        Return (Arg3)
                    }
                }
            }

            Device (PCI3)
            {
                Name (_HID, "PNP0A08" /* PCI Express Bus */)  // _HID: Hardware ID
                Name (_CID, "PNP0A03" /* PCI Bus */)  // _CID: Compatible ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_UID, 0x03)  // _UID: Unique ID
                Name (_SEG, 0x03)  // _SEG: PCI Segment
                Method (_BBN, 0, NotSerialized)  // _BBN: BIOS Bus Number
                {
                    Return (PBMI) /* \_SB_.PBMI */
                }

                Name (_STA, 0x0F)  // _STA: Status
                Name (_PRT, Package (0x04)  // _PRT: PCI Routing Table
                {
                    Package (0x04)
                    {
                        0xFFFF, 
                        Zero, 
                        Zero, 
                        0x0115
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        One, 
                        Zero, 
                        0x0115
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x02, 
                        Zero, 
                        0x0115
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x03, 
                        Zero, 
                        0x0115
                    }
                })
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x88)
                    {
                        /* 0000 */  0x88, 0x0D, 0x00, 0x02, 0x0C, 0x00, 0x00, 0x00,  // ........
                        /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0010 */  0x87, 0x17, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // ........
                        /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0028 */  0x00, 0x00, 0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01,  // ...+....
                        /* 0030 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0038 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0040 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0048 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0050 */  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0058 */  0x8A, 0x2B, 0x00, 0x01, 0x0C, 0x03, 0x00, 0x00,  // .+......
                        /* 0060 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0068 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0070 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0078 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0080 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                    })
                    CreateWordField (RBUF, 0x08, MI00)
                    CreateWordField (RBUF, 0x0A, MA00)
                    CreateWordField (RBUF, 0x0C, TR00)
                    CreateWordField (RBUF, 0x0E, LE00)
                    LE00 = ((PBMA - PBMI) + One)
                    MI00 = PBMI /* \_SB_.PBMI */
                    TR00 = Zero
                    MA00 = ((MI00 + LE00) - One)
                    CreateDWordField (RBUF, 0x1A, MI01)
                    CreateDWordField (RBUF, 0x1E, MA01)
                    CreateDWordField (RBUF, 0x22, TR01)
                    CreateDWordField (RBUF, 0x26, LE01)
                    LE01 = 0x01000000
                    MI01 = 0xF3000000
                    TR01 = Zero
                    MA01 = ((MI01 + LE01) - One)
                    CreateQWordField (RBUF, 0x38, MI02)
                    CreateQWordField (RBUF, 0x40, MA02)
                    CreateQWordField (RBUF, 0x48, TR02)
                    CreateQWordField (RBUF, 0x50, LE02)
                    LE02 = 0x2FFF0000
                    MI02 = 0x00000009D0000000
                    TR02 = Zero
                    MA02 = ((MI02 + LE02) - One)
                    CreateQWordField (RBUF, 0x66, MI03)
                    CreateQWordField (RBUF, 0x6E, MA03)
                    CreateQWordField (RBUF, 0x76, TR03)
                    CreateQWordField (RBUF, 0x7E, LE03)
                    LE03 = 0x00010000
                    MI03 = Zero
                    TR03 = 0x00000009FFFF0000
                    MA03 = ((MI03 + LE03) - One)
                    Return (RBUF) /* \_SB_.PCI3._CRS.RBUF */
                }

                Device (RES0)
                {
                    Name (_HID, "AMZN0001")  // _HID: Hardware ID
                    Name (_CID, "PNP0C02" /* PNP Motherboard Resources */)  // _CID: Compatible ID
                    Name (_UID, 0x03)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x00400000
                        MI00 = 0x0000000A40C00000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI3.RES0._CRS.RBUF */
                    }
                }

                Device (RES1)
                {
                    Name (_HID, "PNP0C02" /* PNP Motherboard Resources */)  // _HID: Hardware ID
                    Name (_UID, 0x04)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x10000000
                        MI00 = 0x00000009C0000000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI3.RES1._CRS.RBUF */
                    }
                }

                Name (SUPP, Zero)
                Name (CTRL, Zero)
                Method (_OSC, 4, NotSerialized)  // _OSC: Operating System Capabilities
                {
                    If ((Arg0 == ToUUID ("33db4d5b-1ff7-401c-9657-7441c03dd766") /* PCI Host Bridge Device */))
                    {
                        CreateDWordField (Arg3, Zero, CDW1)
                        CreateDWordField (Arg3, 0x04, CDW2)
                        CreateDWordField (Arg3, 0x08, CDW3)
                        SUPP = CDW2 /* \_SB_.PCI3._OSC.CDW2 */
                        CTRL = CDW3 /* \_SB_.PCI3._OSC.CDW3 */
                        CTRL &= 0x1E
                        CTRL &= 0x1D
                        If ((Arg1 != One))
                        {
                            CDW1 |= 0x08
                        }

                        If ((CDW3 != CTRL))
                        {
                            CDW1 |= 0x10
                        }

                        CDW3 = CTRL /* \_SB_.PCI3.CTRL */
                        Return (Arg3)
                    }
                    Else
                    {
                        CDW1 |= 0x04
                        Return (Arg3)
                    }
                }
            }

            Device (PCI4)
            {
                Name (_HID, "PNP0A08" /* PCI Express Bus */)  // _HID: Hardware ID
                Name (_CID, "PNP0A03" /* PCI Bus */)  // _CID: Compatible ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_UID, 0x04)  // _UID: Unique ID
                Name (_SEG, 0x04)  // _SEG: PCI Segment
                Method (_BBN, 0, NotSerialized)  // _BBN: BIOS Bus Number
                {
                    Return (PBMI) /* \_SB_.PBMI */
                }

                Name (_STA, 0x0F)  // _STA: Status
                Name (_PRT, Package (0x04)  // _PRT: PCI Routing Table
                {
                    Package (0x04)
                    {
                        0xFFFF, 
                        Zero, 
                        Zero, 
                        0x011A
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        One, 
                        Zero, 
                        0x011A
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x02, 
                        Zero, 
                        0x011A
                    }, 

                    Package (0x04)
                    {
                        0xFFFF, 
                        0x03, 
                        Zero, 
                        0x011A
                    }
                })
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x88)
                    {
                        /* 0000 */  0x88, 0x0D, 0x00, 0x02, 0x0C, 0x00, 0x00, 0x00,  // ........
                        /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0010 */  0x87, 0x17, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // ........
                        /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0028 */  0x00, 0x00, 0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01,  // ...+....
                        /* 0030 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0038 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0040 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0048 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0050 */  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0058 */  0x8A, 0x2B, 0x00, 0x01, 0x0C, 0x03, 0x00, 0x00,  // .+......
                        /* 0060 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0068 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0070 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                        /* 0078 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                        /* 0080 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                    })
                    CreateWordField (RBUF, 0x08, MI00)
                    CreateWordField (RBUF, 0x0A, MA00)
                    CreateWordField (RBUF, 0x0C, TR00)
                    CreateWordField (RBUF, 0x0E, LE00)
                    LE00 = ((PBMA - PBMI) + One)
                    MI00 = PBMI /* \_SB_.PBMI */
                    TR00 = Zero
                    MA00 = ((MI00 + LE00) - One)
                    CreateDWordField (RBUF, 0x1A, MI01)
                    CreateDWordField (RBUF, 0x1E, MA01)
                    CreateDWordField (RBUF, 0x22, TR01)
                    CreateDWordField (RBUF, 0x26, LE01)
                    LE01 = 0x01000000
                    MI01 = 0xF4000000
                    TR01 = Zero
                    MA01 = ((MI01 + LE01) - One)
                    CreateQWordField (RBUF, 0x38, MI02)
                    CreateQWordField (RBUF, 0x40, MA02)
                    CreateQWordField (RBUF, 0x48, TR02)
                    CreateQWordField (RBUF, 0x50, LE02)
                    LE02 = 0x2FFF0000
                    MI02 = 0x0000000A10000000
                    TR02 = Zero
                    MA02 = ((MI02 + LE02) - One)
                    CreateQWordField (RBUF, 0x66, MI03)
                    CreateQWordField (RBUF, 0x6E, MA03)
                    CreateQWordField (RBUF, 0x76, TR03)
                    CreateQWordField (RBUF, 0x7E, LE03)
                    LE03 = 0x00010000
                    MI03 = Zero
                    TR03 = 0x0000000A3FFF0000
                    MA03 = ((MI03 + LE03) - One)
                    Return (RBUF) /* \_SB_.PCI4._CRS.RBUF */
                }

                Device (RES0)
                {
                    Name (_HID, "AMZN0001")  // _HID: Hardware ID
                    Name (_CID, "PNP0C02" /* PNP Motherboard Resources */)  // _CID: Compatible ID
                    Name (_UID, 0x04)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x00400000
                        MI00 = 0x0000000A41000000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI4.RES0._CRS.RBUF */
                    }
                }

                Device (RES1)
                {
                    Name (_HID, "PNP0C02" /* PNP Motherboard Resources */)  // _HID: Hardware ID
                    Name (_UID, 0x05)  // _UID: Unique ID
                    Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                    {
                        Name (RBUF, Buffer (0x30)
                        {
                            /* 0000 */  0x8A, 0x2B, 0x00, 0x00, 0x0C, 0x01, 0x00, 0x00,  // .+......
                            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0010 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0018 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // ........
                            /* 0020 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,  // ........
                            /* 0028 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00   // ......y.
                        })
                        CreateQWordField (RBUF, 0x0E, MI00)
                        CreateQWordField (RBUF, 0x16, MA00)
                        CreateQWordField (RBUF, 0x1E, TR00)
                        CreateQWordField (RBUF, 0x26, LE00)
                        LE00 = 0x10000000
                        MI00 = 0x0000000A00000000
                        TR00 = Zero
                        MA00 = ((MI00 + LE00) - One)
                        Return (RBUF) /* \_SB_.PCI4.RES1._CRS.RBUF */
                    }
                }

                Name (SUPP, Zero)
                Name (CTRL, Zero)
                Method (_OSC, 4, NotSerialized)  // _OSC: Operating System Capabilities
                {
                    If ((Arg0 == ToUUID ("33db4d5b-1ff7-401c-9657-7441c03dd766") /* PCI Host Bridge Device */))
                    {
                        CreateDWordField (Arg3, Zero, CDW1)
                        CreateDWordField (Arg3, 0x04, CDW2)
                        CreateDWordField (Arg3, 0x08, CDW3)
                        SUPP = CDW2 /* \_SB_.PCI4._OSC.CDW2 */
                        CTRL = CDW3 /* \_SB_.PCI4._OSC.CDW3 */
                        CTRL &= 0x1E
                        CTRL &= 0x1D
                        If ((Arg1 != One))
                        {
                            CDW1 |= 0x08
                        }

                        If ((CDW3 != CTRL))
                        {
                            CDW1 |= 0x10
                        }

                        CDW3 = CTRL /* \_SB_.PCI4.CTRL */
                        Return (Arg3)
                    }
                    Else
                    {
                        CDW1 |= 0x04
                        Return (Arg3)
                    }
                }
            }
        }

        Scope (\_SB)
        {
            Device (ATA0)
            {
                Name (_HID, "RKCP0161")  // _HID: Hardware ID
                Name (_UID, Zero)  // _UID: Unique ID
                Name (_CLS, Package (0x03)  // _CLS: Class Code
                {
                    One, 
                    0x06, 
                    One
                })
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_STA, 0x00)  // _STA: Status
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x17)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x21, 0xFE,  // ......!.
                        /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                        /* 0010 */  0x01, 0x31, 0x01, 0x00, 0x00, 0x79, 0x00         // .1...y.
                    })
                    Return (RBUF) /* \_SB_.ATA0._CRS.RBUF */
                }

                Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
                {
                    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                    Package (0x01)
                    {
                        Package (0x02)
                        {
                            "compatible", 
                            "rockchip,rk-ahci"
                        }
                    }
                })
            }

            Device (ATA1)
            {
                Name (_HID, "RKCP0161")  // _HID: Hardware ID
                Name (_UID, One)  // _UID: Unique ID
                Name (_CLS, Package (0x03)  // _CLS: Class Code
                {
                    One, 
                    0x06, 
                    One
                })
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_STA, 0x00)  // _STA: Status
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x17)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x22, 0xFE,  // ......".
                        /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                        /* 0010 */  0x01, 0x32, 0x01, 0x00, 0x00, 0x79, 0x00         // .2...y.
                    })
                    Return (RBUF) /* \_SB_.ATA1._CRS.RBUF */
                }

                Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
                {
                    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                    Package (0x01)
                    {
                        Package (0x02)
                        {
                            "compatible", 
                            "rockchip,rk-ahci"
                        }
                    }
                })
            }

            Device (ATA2)
            {
                Name (_HID, "RKCP0161")  // _HID: Hardware ID
                Name (_UID, 0x02)  // _UID: Unique ID
                Name (_CLS, Package (0x03)  // _CLS: Class Code
                {
                    One, 
                    0x06, 
                    One
                })
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_STA, 0x00)  // _STA: Status
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x17)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x23, 0xFE,  // ......#.
                        /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                        /* 0010 */  0x01, 0x33, 0x01, 0x00, 0x00, 0x79, 0x00         // .3...y.
                    })
                    Return (RBUF) /* \_SB_.ATA2._CRS.RBUF */
                }

                Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
                {
                    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                    Package (0x01)
                    {
                        Package (0x02)
                        {
                            "compatible", 
                            "rockchip,rk-ahci"
                        }
                    }
                })
            }
        }

        Device (SDC3)
        {
            Name (_HID, "RKCP0D40")  // _HID: Hardware ID
            Name (_UID, 0x03)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x2E, 0xFE,  // ........
                    /* 0008 */  0x00, 0x00, 0x01, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0xED, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                })
                Return (RBUF) /* \_SB_.SDC3._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x08)
                {
                    Package (0x02)
                    {
                        "compatible", 
                        "rockchip,rk3588-dwcmshc"
                    }, 

                    Package (0x02)
                    {
                        "max-frequency", 
                        0x0BEBC200
                    }, 

                    Package (0x02)
                    {
                        "bus-width", 
                        0x08
                    }, 

                    Package (0x02)
                    {
                        "no-sd", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "no-sdio", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "mmc-hs400-1_8v", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "mmc-hs400-enhanced-strobe", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "non-removable", 
                        One
                    }
                }
            })
            OperationRegion (EMMC, SystemMemory, 0xFD7C0434, 0x04)
            Field (EMMC, DWordAcc, NoLock, WriteAsZeros)
            {
                PLLE,   32
            }

            Method (_DSM, 4, Serialized)  // _DSM: Device-Specific Method
            {
                If ((Arg0 == ToUUID ("434addb0-8ff3-49d5-a724-95844b79ad1f") /* Unknown UUID */))
                {
                    Switch (ToInteger (Arg2))
                    {
                        Case (Zero)
                        {
                            Return (0x03)
                        }
                        Case (One)
                        {
                            Local0 = DerefOf (Arg3 [Zero])
                            If ((Local0 >= 0x0BEBC200))
                            {
                                PLLE = 0xFF000500
                                Return (0x0BEBC200)
                            }

                            If ((Local0 >= 0x08F0D180))
                            {
                                PLLE = 0xFF000700
                                Return (0x08F0D180)
                            }

                            If ((Local0 >= 0x05F5E100))
                            {
                                PLLE = 0xFF000B00
                                Return (0x05F5E100)
                            }

                            If ((Local0 >= 0x02FAF080))
                            {
                                PLLE = 0xFF001700
                                Return (0x02FAF080)
                            }

                            If ((Local0 >= 0x016E3600))
                            {
                                PLLE = 0xFF008000
                                Return (0x016E3600)
                            }

                            If ((Local0 >= 0x0005B8D8))
                            {
                                PLLE = 0xFF00BF00
                                Return (0x0005B8D8)
                            }

                            Return (Zero)
                        }

                    }
                }

                Return (Zero)
            }

            Method (SCLK, 1, Serialized)
            {
                If ((Arg0 <= 0x00061A80))
                {
                    PLLE = 0xFF00BF00
                }
                ElseIf ((Arg0 <= 0x02FAF080))
                {
                    PLLE = 0xFF008000
                }
                Else
                {
                    PLLE = 0xFF000600
                }
            }

            Device (SDMM)
            {
                Method (_ADR, 0, NotSerialized)  // _ADR: Address
                {
                    Return (Zero)
                }

                Method (_RMV, 0, NotSerialized)  // _RMV: Removal Status
                {
                    Return (Zero)
                }
            }
        }

        Scope (\_SB)
        {
            Name (SDRM, One)
            Device (SDHC)
            {
                Name (_HID, "RKCPFE2C")  // _HID: Hardware ID
                Name (_UID, Zero)  // _UID: Unique ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_S1D, One)  // _S1D: S1 Device State
                Name (_S2D, One)  // _S2D: S2 Device State
                Name (_S3D, One)  // _S3D: S3 Device State
                Name (_S4D, One)  // _S4D: S4 Device State
                Name (_STA, 0x0F)  // _STA: Status
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x5D)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x2C, 0xFE,  // ......,.
                        /* 0008 */  0x00, 0x40, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // .@......
                        /* 0010 */  0x01, 0xEB, 0x00, 0x00, 0x00, 0x8C, 0x20, 0x00,  // ...... .
                        /* 0018 */  0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00,  // ........
                        /* 0020 */  0x00, 0x00, 0x00, 0x17, 0x00, 0x00, 0x19, 0x00,  // ........
                        /* 0028 */  0x23, 0x00, 0x00, 0x00, 0x04, 0x00, 0x5C, 0x5F,  // #.....\_
                        /* 0030 */  0x53, 0x42, 0x2E, 0x47, 0x50, 0x49, 0x30, 0x00,  // SB.GPI0.
                        /* 0038 */  0x8C, 0x20, 0x00, 0x01, 0x00, 0x01, 0x00, 0x0D,  // . ......
                        /* 0040 */  0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x17, 0x00,  // ........
                        /* 0048 */  0x00, 0x19, 0x00, 0x23, 0x00, 0x00, 0x00, 0x04,  // ...#....
                        /* 0050 */  0x00, 0x5C, 0x5F, 0x53, 0x42, 0x2E, 0x47, 0x50,  // .\_SB.GP
                        /* 0058 */  0x49, 0x30, 0x00, 0x79, 0x00                     // I0.y.
                    })
                    Return (RBUF) /* \_SB_.SDHC._CRS.RBUF */
                }

                Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
                {
                    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                    Package (0x09)
                    {
                        Package (0x02)
                        {
                            "compatible", 
                            Package (0x02)
                            {
                                "rockchip,rk3588-dw-mshc", 
                                "rockchip,rk3288-dw-mshc"
                            }
                        }, 

                        Package (0x02)
                        {
                            "fifo-depth", 
                            0x0100
                        }, 

                        Package (0x02)
                        {
                            "max-frequency", 
                            0x0BEBC200
                        }, 

                        Package (0x02)
                        {
                            "bus-width", 
                            0x04
                        }, 

                        Package (0x02)
                        {
                            "cap-sd-highspeed", 
                            One
                        }, 

                        Package (0x02)
                        {
                            "sd-uhs-ddr50", 
                            One
                        }, 

                        Package (0x02)
                        {
                            "sd-uhs-sdr50", 
                            One
                        }, 

                        Package (0x02)
                        {
                            "sd-uhs-sdr104", 
                            One
                        }, 

                        Package (0x02)
                        {
                            "broken-cd", 
                            Zero
                        }
                    }
                })
                Device (SDMM)
                {
                    Name (_ADR, Zero)  // _ADR: Address
                    Method (_RMV, 0, NotSerialized)  // _RMV: Removal Status
                    {
                        Return (SDRM) /* \_SB_.SDRM */
                    }
                }
            }
        }

        Device (DMA0)
        {
            Name (_HID, "ARMH0330")  // _HID: Hardware ID
            Name (_UID, Zero)  // _UID: Unique ID
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x20)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xA1, 0xFE,  // ........
                    /* 0008 */  0x00, 0x40, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // .@......
                    /* 0010 */  0x01, 0x76, 0x00, 0x00, 0x00, 0x89, 0x06, 0x00,  // .v......
                    /* 0018 */  0x01, 0x01, 0x77, 0x00, 0x00, 0x00, 0x79, 0x00   // ..w...y.
                })
                Return (RBUF) /* \_SB_.DMA0._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "ctlrName", 
                        "DMA0"
                    }
                }
            })
        }

        Device (DMA1)
        {
            Name (_HID, "ARMH0330")  // _HID: Hardware ID
            Name (_UID, One)  // _UID: Unique ID
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x20)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xA3, 0xFE,  // ........
                    /* 0008 */  0x00, 0x40, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // .@......
                    /* 0010 */  0x01, 0x78, 0x00, 0x00, 0x00, 0x89, 0x06, 0x00,  // .x......
                    /* 0018 */  0x01, 0x01, 0x79, 0x00, 0x00, 0x00, 0x79, 0x00   // ..y...y.
                })
                Return (RBUF) /* \_SB_.DMA1._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "ctlrName", 
                        "DMA1"
                    }
                }
            })
        }

        Device (DMA2)
        {
            Name (_HID, "ARMH0330")  // _HID: Hardware ID
            Name (_UID, 0x02)  // _UID: Unique ID
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x20)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xD1, 0xFE,  // ........
                    /* 0008 */  0x00, 0x40, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // .@......
                    /* 0010 */  0x01, 0x7A, 0x00, 0x00, 0x00, 0x89, 0x06, 0x00,  // .z......
                    /* 0018 */  0x01, 0x01, 0x7B, 0x00, 0x00, 0x00, 0x79, 0x00   // ..{...y.
                })
                Return (RBUF) /* \_SB_.DMA2._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "ctlrName", 
                        "DMA2"
                    }
                }
            })
        }

        Device (GPI0)
        {
            Name (_HID, "RKCP3002")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, Zero)  // _UID: Unique ID
            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "compatible", 
                        "rockchip,gpio-bank"
                    }
                }
            })
            Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x8A, 0xFD,  // ........
                    /* 0008 */  0x00, 0x01, 0x00, 0x00, 0x89, 0x06, 0x00, 0x09,  // ........
                    /* 0010 */  0x01, 0x35, 0x01, 0x00, 0x00, 0x79, 0x00         // .5...y.
                })
                Return (RBUF) /* \_SB_.GPI0._CRS.RBUF */
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }
        }

        Device (GPI1)
        {
            Name (_HID, "RKCP3002")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, One)  // _UID: Unique ID
            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "compatible", 
                        "rockchip,gpio-bank"
                    }
                }
            })
            Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xC2, 0xFE,  // ........
                    /* 0008 */  0x00, 0x01, 0x00, 0x00, 0x89, 0x06, 0x00, 0x09,  // ........
                    /* 0010 */  0x01, 0x36, 0x01, 0x00, 0x00, 0x79, 0x00         // .6...y.
                })
                Return (RBUF) /* \_SB_.GPI1._CRS.RBUF */
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }
        }

        Device (GPI2)
        {
            Name (_HID, "RKCP3002")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x02)  // _UID: Unique ID
            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "compatible", 
                        "rockchip,gpio-bank"
                    }
                }
            })
            Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xC3, 0xFE,  // ........
                    /* 0008 */  0x00, 0x01, 0x00, 0x00, 0x89, 0x06, 0x00, 0x09,  // ........
                    /* 0010 */  0x01, 0x37, 0x01, 0x00, 0x00, 0x79, 0x00         // .7...y.
                })
                Return (RBUF) /* \_SB_.GPI2._CRS.RBUF */
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }
        }

        Device (GPI3)
        {
            Name (_HID, "RKCP3002")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x03)  // _UID: Unique ID
            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "compatible", 
                        "rockchip,gpio-bank"
                    }
                }
            })
            Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xC4, 0xFE,  // ........
                    /* 0008 */  0x00, 0x01, 0x00, 0x00, 0x89, 0x06, 0x00, 0x09,  // ........
                    /* 0010 */  0x01, 0x38, 0x01, 0x00, 0x00, 0x79, 0x00         // .8...y.
                })
                Return (RBUF) /* \_SB_.GPI3._CRS.RBUF */
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }
        }

        Device (GPI4)
        {
            Name (_HID, "RKCP3002")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x04)  // _UID: Unique ID
            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "compatible", 
                        "rockchip,gpio-bank"
                    }
                }
            })
            Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xC5, 0xFE,  // ........
                    /* 0008 */  0x00, 0x01, 0x00, 0x00, 0x89, 0x06, 0x00, 0x09,  // ........
                    /* 0010 */  0x01, 0x39, 0x01, 0x00, 0x00, 0x79, 0x00         // .9...y.
                })
                Return (RBUF) /* \_SB_.GPI4._CRS.RBUF */
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }
        }

        Device (PINC)
        {
            Name (_HID, "PRP0001")  // _HID: Hardware ID
            Name (_UID, 0x04)  // _UID: Unique ID
            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x01)
                {
                    Package (0x02)
                    {
                        "compatible", 
                        "rockchip,rk3588-pinctrl"
                    }
                }
            })
            Method (_CRS, 0, NotSerialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x0E)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x5F, 0xFD,  // ......_.
                    /* 0008 */  0x00, 0x00, 0x01, 0x00, 0x79, 0x00               // ....y.
                })
                Return (RBUF) /* \_SB_.PINC._CRS.RBUF */
            }

            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }
        }

        Device (I2C1)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, One)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xA9, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x5E, 0x01, 0x00, 0x00, 0x79, 0x00         // .^...y.
                })
                Return (RBUF) /* \_SB_.I2C1._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (I2C2)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x02)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xAA, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x5F, 0x01, 0x00, 0x00, 0x79, 0x00         // ._...y.
                })
                Return (RBUF) /* \_SB_.I2C2._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (I2C3)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x03)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xAB, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x60, 0x01, 0x00, 0x00, 0x79, 0x00         // .`...y.
                })
                Return (RBUF) /* \_SB_.I2C3._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (I2C4)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x04)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xAC, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x61, 0x01, 0x00, 0x00, 0x79, 0x00         // .a...y.
                })
                Return (RBUF) /* \_SB_.I2C4._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (I2C5)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x05)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xAD, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x62, 0x01, 0x00, 0x00, 0x79, 0x00         // .b...y.
                })
                Return (RBUF) /* \_SB_.I2C5._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (I2C6)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x06)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xC8, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x63, 0x01, 0x00, 0x00, 0x79, 0x00         // .c...y.
                })
                Return (RBUF) /* \_SB_.I2C6._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (I2C7)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x07)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xC9, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x64, 0x01, 0x00, 0x00, 0x79, 0x00         // .d...y.
                })
                Return (RBUF) /* \_SB_.I2C7._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (I2C8)
        {
            Name (_HID, "RKCP3001")  // _HID: Hardware ID
            Name (_CID, "PRP0001")  // _CID: Compatible ID
            Name (_UID, 0x08)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xCA, 0xFE,  // ........
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0x65, 0x01, 0x00, 0x00, 0x79, 0x00         // .e...y.
                })
                Return (RBUF) /* \_SB_.I2C8._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "i2c,clk-rate", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "rockchip,bclk", 
                        0x0BCD3D80
                    }, 

                    Package (0x02)
                    {
                        "#address-cells", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "#size-cells", 
                        Zero
                    }
                }
            })
        }

        Device (UAR2)
        {
            Name (_HID, "HISI0031")  // _HID: Hardware ID
            Name (_UID, 0x02)  // _UID: Unique ID
            Name (_CRS, Buffer (0x17)  // _CRS: Current Resource Settings
            {
                /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xB5, 0xFE,  // ........
                /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                /* 0010 */  0x01, 0x6D, 0x01, 0x00, 0x00, 0x79, 0x00         // .m...y.
            })
            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x03)
                {
                    Package (0x02)
                    {
                        "reg-shift", 
                        0x02
                    }, 

                    Package (0x02)
                    {
                        "reg-io-width", 
                        0x04
                    }, 

                    Package (0x02)
                    {
                        "clock-frequency", 
                        0x016E3600
                    }
                }
            })
            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                Return (0x0F)
            }
        }

        Device (I2S0)
        {
            Name (_HID, "RKCP3003")  // _HID: Hardware ID
            Name (_UID, Zero)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x47, 0xFE,  // ......G.
                    /* 0008 */  0x00, 0x10, 0x00, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                    /* 0010 */  0x01, 0xD4, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                })
                Return (RBUF) /* \_SB_.I2S0._CRS.RBUF */
            }

            Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
            {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                Package (0x04)
                {
                    Package (0x02)
                    {
                        "rockchip,dma", 
                        "DMA0"
                    }, 

                    Package (0x02)
                    {
                        "rockchip,tx", 
                        Zero
                    }, 

                    Package (0x02)
                    {
                        "rockchip,rx", 
                        One
                    }, 

                    Package (0x02)
                    {
                        "rockchip,tplg", 
                        "i2s-jack"
                    }
                }
            })
            Name (_DEP, Package (0x01)  // _DEP: Dependencies
            {
                DMA0, 
            })
            Method (_INI, 0, Serialized)  // _INI: Initialize
            {
                _DSM (ToUUID ("7056bfa1-af0b-48e5-b67f-139f2004a26a") /* Unknown UUID */, Zero, One, Package (0x01)
                    {
                        0x00BB8000
                    })
            }

            Method (_DSM, 4, Serialized)  // _DSM: Device-Specific Method
            {
                OperationRegion (CRU, SystemMemory, 0xFD7C0360, 0x08)
                Field (CRU, DWordAcc, NoLock, Preserve)
                {
                    C24,    32, 
                    C25,    32
                }

                If ((Arg0 == ToUUID ("7056bfa1-af0b-48e5-b67f-139f2004a26a") /* Unknown UUID */))
                {
                    If ((Arg1 >= Zero))
                    {
                        Switch (ToInteger (Arg2))
                        {
                            Case (Zero)
                            {
                                Return (Buffer (One)
                                {
                                     0x03                                             // .
                                })
                            }
                            Case (One)
                            {
                                Local0 = (0x2EE00000 / (((C24 >> 0x04) & 0x0F) + One
                                    ))
                                Local1 = FRBA (DerefOf (Arg3 [Zero]), Local0, 0xFFFF, 0xFFFF)
                                C25 = ((DerefOf (Local1 [Zero]) << 0x10) | DerefOf (
                                    Local1 [One]))
                                Return (((Local0 * DerefOf (Local1 [Zero])) / DerefOf (
                                    Local1 [One])))
                            }

                        }
                    }
                }

                Return (Buffer (One)
                {
                     0x00                                             // .
                })
            }
        }

        Method (FRBA, 4, NotSerialized)
        {
            Local0 = Arg0
            Local1 = Arg1
            Local2 = Zero
            Local3 = One
            Local4 = One
            Local5 = Zero
            While (One)
            {
                If (((Local3 > Arg2) || (Local5 > Arg3)))
                {
                    Local3 = Local2
                    Local5 = Local4
                    Break
                }

                If ((Local1 == Zero))
                {
                    Break
                }

                Local7 = (Local0 / Local1)
                Local6 = Local1
                Local1 = (Local0 % Local1)
                Local0 = Local6
                Local6 = (Local2 + (Local7 * Local3))
                Local2 = Local3
                Local3 = Local6
                Local6 = (Local4 + (Local7 * Local5))
                Local4 = Local5
                Local5 = Local6
            }

            Local0 = Package (0x02)
                {
                    Zero, 
                    Zero
                }
            Local0 [Zero] = Local3
            Local0 [One] = Local5
            Return (Local0)
        }

        Scope (\_SB)
        {
            Name (EHID, One)
            Device (EHC0)
            {
                Name (_HID, "RKCP0D20")  // _HID: Hardware ID
                If ((EHID == One))
                {
                    Name (_CID, "PNP0D20" /* EHCI USB Controller without debug */)  // _CID: Compatible ID
                }

                Name (_UID, Zero)  // _UID: Unique ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x17)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x80, 0xFC,  // ........
                        /* 0008 */  0x00, 0x00, 0x04, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                        /* 0010 */  0x01, 0xF7, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                    })
                    Return (RBUF) /* \_SB_.EHC0._CRS.RBUF */
                }

                Device (RHUB)
                {
                    Name (_ADR, Zero)  // _ADR: Address
                    Device (PRT1)
                    {
                        Name (_ADR, One)  // _ADR: Address
                        Name (_UPC, Package (0x04)  // _UPC: USB Port Capabilities
                        {
                            0xFF, 
                            Zero, 
                            Zero, 
                            Zero
                        })
                        Name (_PLD, Package (0x01)  // _PLD: Physical Location of Device
                        {
                            ToPLD (
                                PLD_Revision           = 0x2,
                                PLD_IgnoreColor        = 0x1,
                                PLD_Red                = 0x0,
                                PLD_Green              = 0x0,
                                PLD_Blue               = 0x0,
                                PLD_Width              = 0x0,
                                PLD_Height             = 0x0,
                                PLD_UserVisible        = 0x1,
                                PLD_Dock               = 0x0,
                                PLD_Lid                = 0x0,
                                PLD_Panel              = "UNKNOWN",
                                PLD_VerticalPosition   = "UPPER",
                                PLD_HorizontalPosition = "LEFT",
                                PLD_Shape              = "HORIZONTALRECTANGLE",
                                PLD_GroupOrientation   = 0x0,
                                PLD_GroupToken         = 0x0,
                                PLD_GroupPosition      = 0x0,
                                PLD_Bay                = 0x0,
                                PLD_Ejectable          = 0x1,
                                PLD_EjectRequired      = 0x1,
                                PLD_CabinetNumber      = 0x0,
                                PLD_CardCageNumber     = 0x0,
                                PLD_Reference          = 0x0,
                                PLD_Rotation           = 0x0,
                                PLD_Order              = 0x0,
                                PLD_VerticalOffset     = 0x0,
                                PLD_HorizontalOffset   = 0x0)

                        })
                    }
                }
            }

            Device (EHC1)
            {
                Name (_HID, "RKCP0D20")  // _HID: Hardware ID
                If ((EHID == One))
                {
                    Name (_CID, "PNP0D20" /* EHCI USB Controller without debug */)  // _CID: Compatible ID
                }

                Name (_UID, One)  // _UID: Unique ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x17)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x88, 0xFC,  // ........
                        /* 0008 */  0x00, 0x00, 0x04, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                        /* 0010 */  0x01, 0xFA, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                    })
                    Return (RBUF) /* \_SB_.EHC1._CRS.RBUF */
                }

                Device (RHUB)
                {
                    Name (_ADR, Zero)  // _ADR: Address
                    Device (PRT1)
                    {
                        Name (_ADR, One)  // _ADR: Address
                        Name (_UPC, Package (0x04)  // _UPC: USB Port Capabilities
                        {
                            0xFF, 
                            Zero, 
                            Zero, 
                            Zero
                        })
                        Name (_PLD, Package (0x01)  // _PLD: Physical Location of Device
                        {
                            ToPLD (
                                PLD_Revision           = 0x2,
                                PLD_IgnoreColor        = 0x1,
                                PLD_Red                = 0x0,
                                PLD_Green              = 0x0,
                                PLD_Blue               = 0x0,
                                PLD_Width              = 0x0,
                                PLD_Height             = 0x0,
                                PLD_UserVisible        = 0x1,
                                PLD_Dock               = 0x0,
                                PLD_Lid                = 0x0,
                                PLD_Panel              = "UNKNOWN",
                                PLD_VerticalPosition   = "LOWER",
                                PLD_HorizontalPosition = "LEFT",
                                PLD_Shape              = "HORIZONTALRECTANGLE",
                                PLD_GroupOrientation   = 0x0,
                                PLD_GroupToken         = 0x0,
                                PLD_GroupPosition      = 0x0,
                                PLD_Bay                = 0x0,
                                PLD_Ejectable          = 0x1,
                                PLD_EjectRequired      = 0x1,
                                PLD_CabinetNumber      = 0x0,
                                PLD_CardCageNumber     = 0x0,
                                PLD_Reference          = 0x0,
                                PLD_Rotation           = 0x0,
                                PLD_Order              = 0x0,
                                PLD_VerticalOffset     = 0x0,
                                PLD_HorizontalOffset   = 0x0)

                        })
                    }
                }
            }

            Device (OHC0)
            {
                Name (_HID, "PRP0001")  // _HID: Hardware ID
                Name (_CLS, Package (0x03)  // _CLS: Class Code
                {
                    0x0C, 
                    0x03, 
                    0x10
                })
                Name (_UID, Zero)  // _UID: Unique ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
                {
                    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                    Package (0x01)
                    {
                        Package (0x02)
                        {
                            "compatible", 
                            "generic-ohci"
                        }
                    }
                })
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x17)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x84, 0xFC,  // ........
                        /* 0008 */  0x00, 0x00, 0x04, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                        /* 0010 */  0x01, 0xF8, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                    })
                    Return (RBUF) /* \_SB_.OHC0._CRS.RBUF */
                }

                Device (RHUB)
                {
                    Name (_ADR, Zero)  // _ADR: Address
                    Device (PRT1)
                    {
                        Name (_ADR, One)  // _ADR: Address
                        Name (_UPC, Package (0x04)  // _UPC: USB Port Capabilities
                        {
                            0xFF, 
                            Zero, 
                            Zero, 
                            Zero
                        })
                        Name (_PLD, Package (0x01)  // _PLD: Physical Location of Device
                        {
                            ToPLD (
                                PLD_Revision           = 0x2,
                                PLD_IgnoreColor        = 0x1,
                                PLD_Red                = 0x0,
                                PLD_Green              = 0x0,
                                PLD_Blue               = 0x0,
                                PLD_Width              = 0x0,
                                PLD_Height             = 0x0,
                                PLD_UserVisible        = 0x1,
                                PLD_Dock               = 0x0,
                                PLD_Lid                = 0x0,
                                PLD_Panel              = "UNKNOWN",
                                PLD_VerticalPosition   = "UPPER",
                                PLD_HorizontalPosition = "LEFT",
                                PLD_Shape              = "HORIZONTALRECTANGLE",
                                PLD_GroupOrientation   = 0x0,
                                PLD_GroupToken         = 0x0,
                                PLD_GroupPosition      = 0x0,
                                PLD_Bay                = 0x0,
                                PLD_Ejectable          = 0x1,
                                PLD_EjectRequired      = 0x1,
                                PLD_CabinetNumber      = 0x0,
                                PLD_CardCageNumber     = 0x0,
                                PLD_Reference          = 0x0,
                                PLD_Rotation           = 0x0,
                                PLD_Order              = 0x0,
                                PLD_VerticalOffset     = 0x0,
                                PLD_HorizontalOffset   = 0x0)

                        })
                    }
                }
            }

            Device (OHC1)
            {
                Name (_HID, "PRP0001")  // _HID: Hardware ID
                Name (_CLS, Package (0x03)  // _CLS: Class Code
                {
                    0x0C, 
                    0x03, 
                    0x10
                })
                Name (_UID, One)  // _UID: Unique ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Name (_DSD, Package (0x02)  // _DSD: Device-Specific Data
                {
                    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301") /* Device Properties for _DSD */, 
                    Package (0x01)
                    {
                        Package (0x02)
                        {
                            "compatible", 
                            "generic-ohci"
                        }
                    }
                })
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x17)
                    {
                        /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x8C, 0xFC,  // ........
                        /* 0008 */  0x00, 0x00, 0x04, 0x00, 0x89, 0x06, 0x00, 0x01,  // ........
                        /* 0010 */  0x01, 0xFB, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                    })
                    Return (RBUF) /* \_SB_.OHC1._CRS.RBUF */
                }

                Device (RHUB)
                {
                    Name (_ADR, Zero)  // _ADR: Address
                    Device (PRT1)
                    {
                        Name (_ADR, One)  // _ADR: Address
                        Name (_UPC, Package (0x04)  // _UPC: USB Port Capabilities
                        {
                            0xFF, 
                            Zero, 
                            Zero, 
                            Zero
                        })
                        Name (_PLD, Package (0x01)  // _PLD: Physical Location of Device
                        {
                            ToPLD (
                                PLD_Revision           = 0x2,
                                PLD_IgnoreColor        = 0x1,
                                PLD_Red                = 0x0,
                                PLD_Green              = 0x0,
                                PLD_Blue               = 0x0,
                                PLD_Width              = 0x0,
                                PLD_Height             = 0x0,
                                PLD_UserVisible        = 0x1,
                                PLD_Dock               = 0x0,
                                PLD_Lid                = 0x0,
                                PLD_Panel              = "UNKNOWN",
                                PLD_VerticalPosition   = "LOWER",
                                PLD_HorizontalPosition = "LEFT",
                                PLD_Shape              = "HORIZONTALRECTANGLE",
                                PLD_GroupOrientation   = 0x0,
                                PLD_GroupToken         = 0x0,
                                PLD_GroupPosition      = 0x0,
                                PLD_Bay                = 0x0,
                                PLD_Ejectable          = 0x1,
                                PLD_EjectRequired      = 0x1,
                                PLD_CabinetNumber      = 0x0,
                                PLD_CardCageNumber     = 0x0,
                                PLD_Reference          = 0x0,
                                PLD_Rotation           = 0x0,
                                PLD_Order              = 0x0,
                                PLD_VerticalOffset     = 0x0,
                                PLD_HorizontalOffset   = 0x0)

                        })
                    }
                }
            }
        }

        Device (XHC0)
        {
            Name (_HID, "PNP0D10" /* XHCI USB Controller with debug */)  // _HID: Hardware ID
            Name (_UID, Zero)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFC,  // ........
                    /* 0008 */  0x00, 0x00, 0x40, 0x00, 0x89, 0x06, 0x00, 0x01,  // ..@.....
                    /* 0010 */  0x01, 0xFC, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                })
                Return (RBUF) /* \_SB_.XHC0._CRS.RBUF */
            }
        }

        Device (XHC1)
        {
            Name (_HID, "PNP0D10" /* XHCI USB Controller with debug */)  // _HID: Hardware ID
            Name (_UID, One)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0x40, 0xFC,  // ......@.
                    /* 0008 */  0x00, 0x00, 0x40, 0x00, 0x89, 0x06, 0x00, 0x01,  // ..@.....
                    /* 0010 */  0x01, 0xFD, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                })
                Return (RBUF) /* \_SB_.XHC1._CRS.RBUF */
            }
        }

        Device (XHC2)
        {
            Name (_HID, "PNP0D10" /* XHCI USB Controller with debug */)  // _HID: Hardware ID
            Name (_UID, 0x02)  // _UID: Unique ID
            Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
            Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
            {
                Name (RBUF, Buffer (0x17)
                {
                    /* 0000 */  0x86, 0x09, 0x00, 0x01, 0x00, 0x00, 0xD0, 0xFC,  // ........
                    /* 0008 */  0x00, 0x00, 0x40, 0x00, 0x89, 0x06, 0x00, 0x01,  // ..@.....
                    /* 0010 */  0x01, 0xFE, 0x00, 0x00, 0x00, 0x79, 0x00         // .....y.
                })
                Return (RBUF) /* \_SB_.XHC2._CRS.RBUF */
            }
        }

        Scope (I2C7)
        {
            Device (JACK)
            {
                Name (_HID, "ESSX8316")  // _HID: Hardware ID
                Name (_UID, Zero)  // _UID: Unique ID
                Name (_CCA, Zero)  // _CCA: Cache Coherency Attribute
                Method (_CRS, 0, Serialized)  // _CRS: Current Resource Settings
                {
                    Name (RBUF, Buffer (0x41)
                    {
                        /* 0000 */  0x8E, 0x19, 0x00, 0x02, 0x00, 0x01, 0x02, 0x00,  // ........
                        /* 0008 */  0x00, 0x01, 0x06, 0x00, 0xA0, 0x86, 0x01, 0x00,  // ........
                        /* 0010 */  0x11, 0x00, 0x5C, 0x5F, 0x53, 0x42, 0x2E, 0x49,  // ..\_SB.I
                        /* 0018 */  0x32, 0x43, 0x37, 0x00, 0x8C, 0x20, 0x00, 0x01,  // 2C7.. ..
                        /* 0020 */  0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x00, 0x00,  // ........
                        /* 0028 */  0x00, 0x00, 0x17, 0x00, 0x00, 0x19, 0x00, 0x23,  // .......#
                        /* 0030 */  0x00, 0x00, 0x00, 0x1D, 0x00, 0x5C, 0x5F, 0x53,  // .....\_S
                        /* 0038 */  0x42, 0x2E, 0x47, 0x50, 0x49, 0x31, 0x00, 0x79,  // B.GPI1.y
                        /* 0040 */  0x00                                             // .
                    })
                    Return (RBUF) /* \_SB_.I2C7.JACK._CRS.RBUF */
                }
            }
        }
    }
}

