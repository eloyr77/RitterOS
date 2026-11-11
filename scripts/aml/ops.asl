/*
 * (37b-3) The AML interpreter's operators, case by case: every method Tnnn
 * returns a value, evaluated by RitterOS's interpreter (scripts/amlhost.c)
 * and by Intel's acpiexec (the oracle) and compared (scripts/amleval.py,
 * power_report_test). Our own cases, written from the ACPI specification;
 * iasl compiles them on the Mac at test time.
 */
DefinitionBlock ("", "SSDT", 2, "RITTER", "AMLOPS", 1)
{
    Name (INT1, 0x1234)
    Name (STR1, "Hello")
    Name (BUF1, Buffer (4) { 1, 2, 3, 4 })
    Name (PKG1, Package (3) { 1, "two", Buffer (2) { 3, 3 } })
    Name (CNT1, 0)

    OperationRegion (RAM1, SystemMemory, 0x100000, 0x100)
    Field (RAM1, ByteAcc, NoLock, Preserve)
    {
        FB00, 8,
        FB01, 4,
        FB02, 4,
        Offset (4),
        FW00, 16,
        FD00, 32,
        Offset (0x10),
        FQ00, 64,
        FBIG, 128
    }
    Field (RAM1, DWordAcc, NoLock, WriteAsOnes)
    {
        Offset (0x20),
        FO00, 3,
        FO01, 5
    }
    Field (RAM1, DWordAcc, NoLock, WriteAsZeros)
    {
        Offset (0x24),
        FZ00, 3,
        FZ01, 5
    }
    Field (RAM1, WordAcc, NoLock, Preserve)
    {
        Offset (0x28),
            , 3,
        FP00, 10
    }
    OperationRegion (RAM2, SystemMemory, 0x100100, 0x10)
    Field (RAM2, ByteAcc, NoLock, Preserve)
    {
        INDX, 8,
        DATA, 8
    }
    IndexField (INDX, DATA, ByteAcc, NoLock, Preserve)
    {
        Offset (0x10),
        IX10, 8,
        IX11, 16
    }
    Field (RAM2, ByteAcc, NoLock, Preserve)
    {
        Offset (4),
        BANK, 8
    }
    BankField (RAM2, BANK, 2, ByteAcc, NoLock, Preserve)
    {
        Offset (8),
        BK08, 8
    }
    Mutex (MTX1, 0)
    Event (EVT1)

    Method (T001) { Return (Add (3, 4)) }
    Method (T002) { Return (Subtract (3, 4)) }
    Method (T003) { Return (Multiply (0x10000, 0x10000)) }
    Method (T004) { Divide (100, 7, Local0, Local1) Local7 = Package (2) {} Local7 [0] = Local0 Local7 [1] = Local1 Return (Local7) }
    Method (T005) { Return (Mod (100, 7)) }
    Method (T006) { Return (ShiftLeft (1, 63)) }
    Method (T007) { Return (ShiftLeft (1, 64)) }
    Method (T008) { Return (ShiftRight (0x8000000000000000, 63)) }
    Method (T009) { Local7 = Package (6) {} Local7 [0] = And (0xF0, 0x3C) Local7 [1] = Or (0xF0, 0x0F) Local7 [2] = Xor (0xFF, 0x0F) Local7 [3] = NAnd (0xF0, 0x3C) Local7 [4] = NOr (0xF0, 0x0F) Local7 [5] = Not (0) Return (Local7) }
    Method (T010) { Local7 = Package (4) {} Local7 [0] = FindSetLeftBit (0) Local7 [1] = FindSetLeftBit (0x80) Local7 [2] = FindSetRightBit (0x80) Local7 [3] = FindSetRightBit (0) Return (Local7) }
    Method (T011) { Local0 = 5 Local0++ Local0++ Local0-- Return (Local0) }
    Method (T012) { Local7 = Package (10) {} Local7 [0] = LAnd (1, 0) Local7 [1] = LOr (1, 0) Local7 [2] = LNot (0) Local7 [3] = LNot (5) Local7 [4] = LEqual (3, 3) Local7 [5] = LGreater (4, 3) Local7 [6] = LLess (4, 3) Local7 [7] = LNotEqual (3, 4) Local7 [8] = LLessEqual (3, 3) Local7 [9] = LGreaterEqual (2, 3) Return (Local7) }
    Method (T013) { Local7 = Package (4) {} Local7 [0] = LEqual ("abc", "abc") Local7 [1] = LLess ("abc", "abd") Local7 [2] = LGreater ("abcd", "abc") Local7 [3] = LEqual (Buffer () { 1, 2 }, Buffer () { 1, 2 }) Return (Local7) }
    Method (T014) { Return (Concatenate ("abc", "def")) }
    Method (T015) { Return (Concatenate ("n=", 0x1F)) }
    Method (T016) { Return (Concatenate (0x1122, 0x3344)) }
    Method (T017) { Return (Concatenate (Buffer () { 1, 2 }, Buffer () { 3 })) }
    Method (T018) { Return (Concatenate ("b=", Buffer () { 0xA, 0xB })) }
    Method (T019) { Return (ToHexString (0x1F)) }
    Method (T020) { Return (ToHexString (Buffer () { 1, 0xAB })) }
    Method (T021) { Return (ToDecimalString (1234)) }
    Method (T022) { Return (ToDecimalString (Buffer () { 1, 200 })) }
    Method (T023) { Return (ToInteger ("0x1F")) }
    Method (T024) { Return (ToInteger ("123")) }
    Method (T025) { Return (ToInteger (Buffer () { 1, 2, 3 })) }
    Method (T026) { Return (ToBuffer ("AB")) }
    Method (T027) { Return (ToBuffer (0x0102)) }
    Method (T028) { Return (ToString (Buffer () { 0x41, 0x42, 0, 0x43 }, Ones)) }
    Method (T029) { Return (ToString (Buffer () { 0x41, 0x42, 0x43 }, 2)) }
    Method (T030) { Return (Mid ("Hello world", 6, 5)) }
    Method (T031) { Return (Mid (Buffer () { 1, 2, 3, 4 }, 1, 10)) }
    Method (T032) { Return (SizeOf (STR1)) }
    Method (T033) { Local7 = Package (2) {} Local7 [0] = SizeOf (BUF1) Local7 [1] = SizeOf (PKG1) Return (Local7) }
    Method (T034) { Return (DerefOf (Index (PKG1, 1))) }
    Method (T035) { Return (DerefOf (Index (BUF1, 2))) }
    Method (T036) { Local0 = Buffer (3) { 9, 9, 9 } Local0 [1] = 0x55 Return (Local0) }
    Method (T037) { Local0 = Package (2) { 1, 2 } Local0 [1] = "x" Return (Local0) }
    Method (T038) { Local7 = Package (8) {} Local7 [0] = ObjectType (INT1) Local7 [1] = ObjectType (STR1) Local7 [2] = ObjectType (BUF1) Local7 [3] = ObjectType (PKG1) Local7 [4] = ObjectType (FB00) Local7 [5] = ObjectType (MTX1) Local7 [6] = ObjectType (EVT1) Local7 [7] = ObjectType (RAM1) Return (Local7) }
    Method (T039) { Return (Match (Package () { 1, 5, 9, 12 }, MGT, 4, MLT, 10, 0)) }
    Method (T040) { Return (Match (Package () { 1, 5, 9 }, MEQ, 7, MTR, 0, 0)) }
    Method (T041) { Return (FromBCD (0x1234)) }
    Method (T042) { Return (ToBCD (1234)) }
    Method (T043) { Local0 = 0 While (Local0 < 10) { Local0 += 3 } Return (Local0) }
    Method (T044) {
        Local0 = 0
        Local1 = 0
        While (1) {
            Local0++
            If (Local0 == 3) { Continue }
            If (Local0 > 6) { Break }
            Local1 += Local0
        }
        Return (Local1)
    }
    Method (T045) { If (0) { Return (1) } ElseIf (1) { Return (2) } Else { Return (3) } }
    Method (FACT, 1) { If (Arg0 <= 1) { Return (1) } Return (Arg0 * FACT (Arg0 - 1)) }
    Method (T046) { Return (FACT (10)) }
    Method (SETA, 1) { Arg0 = 0x77 }
    Method (T047) { Local0 = 1 SETA (RefOf (Local0)) Return (Local0) }
    Method (T048) { Store (5, CNT1) SETA (RefOf (CNT1)) Return (CNT1) }
    Method (T049) { Local0 = RefOf (INT1) Return (DerefOf (Local0)) }
    Method (T050) { Return (CondRefOf (\NONE)) }
    Method (T051) { Return (CondRefOf (INT1, Local0)) }
    Method (T052) { FB00 = 0x1AB Return (FB00) }
    Method (T053) { FB01 = 0xF FB02 = 3 Local7 = Package (3) {} Local7 [0] = FB00 Local7 [1] = FB01 Local7 [2] = FB02 Return (Local7) }
    Method (T054) { FW00 = 0x12345 FD00 = 0xDEADBEEF Local7 = Package (2) {} Local7 [0] = FW00 Local7 [1] = FD00 Return (Local7) }
    Method (T055) { FQ00 = 0x1122334455667788 Return (FQ00) }
    Method (T056) { FBIG = Buffer () { 1, 2, 3 } Return (FBIG) }
    Method (T057) { FO00 = 2 Return (FD00) }
    Method (T058) { FZ00 = 5 Local7 = Package (2) {} Local7 [0] = FZ00 Local7 [1] = FZ01 Return (Local7) }
    Method (T059) { FP00 = 0x3FF Return (FP00) }
    Method (T060) { IX10 = 0x5A IX11 = 0xBEEF Local7 = Package (3) {} Local7 [0] = IX10 Local7 [1] = IX11 Local7 [2] = INDX Return (Local7) }
    Method (T061) { BK08 = 0x66 Local7 = Package (2) {} Local7 [0] = BK08 Local7 [1] = BANK Return (Local7) }
    Method (T062) {
        Local0 = Buffer (8) {}
        CreateDWordField (Local0, 0, DW00)
        CreateWordField (Local0, 4, WW00)
        CreateBitField (Local0, 63, BT00)
        CreateField (Local0, 40, 4, NB00)
        DW00 = 0x11223344
        WW00 = 0x5566
        BT00 = 1
        NB00 = 0xF
        Return (Local0)
    }
    Method (T063) { Return (Acquire (MTX1, 0)) }
    Method (T064) { Acquire (MTX1, 0xFFFF) Release (MTX1) Return (1) }
    Method (T065) { Signal (EVT1) Return (Wait (EVT1, 0)) }
    Method (T066) { Reset (EVT1) Return (Wait (EVT1, 0)) }
    Method (T067) { Local0 = "abc" Local0 = 5 Return (Local0) }
    Method (T068) { Name (TMP1, "xyz") TMP1 = 0x41 Return (TMP1) }
    Method (T069) { Name (TMP2, Buffer (2) {}) TMP2 = 0x010203 Return (TMP2) }
    Method (T070) { Name (TMP3, 0) TMP3 = "0x10" Return (TMP3) }
    Method (T071) { Name (TMP4, 0) CopyObject ("str", TMP4) Return (TMP4) }
    Method (T072) { Name (TMP5, 0) TMP5 = Buffer () { 1, 2, 3, 4, 5, 6, 7, 8, 9 } Return (TMP5) }
    Method (T073) { Return (ConcatenateResTemplate (ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) },
                                                    ResourceTemplate () { IRQNoFlags () { 1 } })) }
    Method (T074) { Local0 = Package (3) { 1, 2, 3 } Local1 = Local0 Local1 [0] = 9
                    Local7 = Package (2) {} Local7 [0] = DerefOf (Local0 [0]) Local7 [1] = DerefOf (Local1 [0]) Return (Local7) }
    Method (T075) { Return (_OSI ("Windows 2015")) }
    Method (T076) { Return (_OSI ("Linux")) }
    Method (T077) { Return (\_REV) }
    Method (T078) { Local0 = 0 Local0 |= 0x0F Local0 &= 0x3C Local0 <<= 2 Local0 ^= 1 Return (Local0) }
    Method (T079) { Return (Ones + 1) }
    Method (T080) { Return (0 - 1) }
    Method (T081) { Return (Package (6) { 1, 2 }) }
    Method (T082) { Local0 = 2 + 2 Return (Package (Local0) { 7 }) }
    Method (T083) { Return (Buffer (2 + 3) { 1 }) }
    Method (T084) { Return (Buffer () { 1, 2, 3 }) }
    Method (T085) { Local0 = Buffer (0x40) {} Return (SizeOf (Local0)) }
    Method (T086) { Return (DerefOf (Index ("abc", 1))) }
    Method (T087) { Local0 = Index (Package () { 4, 5 }, 1) Return (DerefOf (Local0)) }
    Method (T088) { Local0 = "\\INT1" Return (DerefOf (Local0)) }
    Method (T089) { Store ("Hello", Debug) Return (Timer > 0) }
    Method (T090) { Local0 = 3 While (Local0) { Local0-- If (Local0 == 1) { Return (0x99) } } Return (0) }
    Method (T091) { Return (LNot (LEqual (Concatenate ("a", "b"), "ab"))) }
    Method (T092) { Return (Package () { Package () { 1, Package () { 2, 3 } }, "s", Buffer () { 4 } }) }
    Method (T093) { Local0 = Package () { INT1, STR1 } Return (Local0) }
    Method (T094) { Local1 = 0 For (Local0 = 0, Local0 < 5, Local0++) { Local1 += Local0 } Return (Local1) }
    Method (T095) {
        Switch (ToInteger (2)) {
            Case (1) { Return ("one") }
            Case (Package () { 2, 3 }) { Return ("two or three") }
            Default { Return ("other") }
        }
    }
    Method (T096) { Local0 = Buffer (4) { 0xFF, 0xFF, 0xFF, 0xFF } CreateWordField (Local0, 1, W1)
                    W1 = 0 Return (Local0) }
    Method (T097) { Return (Concatenate (Buffer () { 1 }, 0x0302)) }
    Method (T098) { Return (Multiply (0xFFFFFFFFFFFFFFFF, 2)) }
    Method (T099) { Return (Divide (0xFFFFFFFFFFFFFFFF, 0x100000000)) }
    Method (T100) { Return (ToHexString ("already a string")) }
}
