/* Dhrystone 2.1 (Reinhold Weicker, 1988), with 16-bit ints, for the program race.
 * Changes, the same in program.tsp: the two records are globals instead of malloc'd; the
 * record's union is a plain struct (only its first variant is ever used); the out
 * parameters that point into a record (Proc_3, Proc_6, Proc_7) return their value
 * instead, since Tisple's addr_of does not go through pointers; Number_Of_Runs comes from
 * the race (runs); no timing or printf: the values Dhrystone prints at the end are folded
 * into checksum. strcpy and strcmp are the C library's, as in the original. */
#include <string.h>
#include "race.h"

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;

typedef struct record {
    struct record *Ptr_Comp;
    Enumeration Discr;
    Enumeration Enum_Comp;
    int Int_Comp;
    char Str_Comp[31];
} Rec_Type, *Rec_Pointer;

typedef int Arr_1_Dim[50];
typedef int Arr_2_Dim[50][50];

static Rec_Type Glob_Rec, Next_Glob_Rec;
static Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
static int Int_Glob;
static char Bool_Glob;
static char Ch_1_Glob, Ch_2_Glob;
static Arr_1_Dim Arr_1_Glob;
static Arr_2_Dim Arr_2_Glob;
static char Str_1_Loc[31];
static char Str_2_Loc[31];

static int Proc_7(int Int_1_Par_Val, int Int_2_Par_Val) {
    int Int_Loc = Int_1_Par_Val + 2;
    return Int_2_Par_Val + Int_Loc;
}

static char Func_3(Enumeration Enum_Par_Val) {
    Enumeration Enum_Loc = Enum_Par_Val;
    if (Enum_Loc == Ident_3) return 1;
    return 0;
}

static Enumeration Proc_6(Enumeration Enum_Val_Par) {
    Enumeration Enum_Ref = Enum_Val_Par;
    if (!Func_3(Enum_Val_Par)) Enum_Ref = Ident_4;
    switch (Enum_Val_Par) {
    case Ident_1:
        Enum_Ref = Ident_1;
        break;
    case Ident_2:
        if (Int_Glob > 100) Enum_Ref = Ident_1;
        else Enum_Ref = Ident_4;
        break;
    case Ident_3:
        Enum_Ref = Ident_2;
        break;
    case Ident_4:
        break;
    case Ident_5:
        Enum_Ref = Ident_3;
        break;
    }
    return Enum_Ref;
}

static Rec_Pointer Proc_3(Rec_Pointer Ptr_Ref_Par) {
    if (Ptr_Glob != 0) Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
    Ptr_Glob->Int_Comp = Proc_7(10, Int_Glob);
    return Ptr_Ref_Par;
}

static void Proc_1(Rec_Pointer Ptr_Val_Par) {
    Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
    *Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
    Ptr_Val_Par->Int_Comp = 5;
    Next_Record->Int_Comp = Ptr_Val_Par->Int_Comp;
    Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
    Next_Record->Ptr_Comp = Proc_3(Next_Record->Ptr_Comp);
    if (Next_Record->Discr == Ident_1) {
        Next_Record->Int_Comp = 6;
        Next_Record->Enum_Comp = Proc_6(Ptr_Val_Par->Enum_Comp);
        Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
        Next_Record->Int_Comp = Proc_7(Next_Record->Int_Comp, 10);
    } else {
        *Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
    }
}

static void Proc_2(int *Int_Par_Ref) {
    int Int_Loc = *Int_Par_Ref + 10;
    Enumeration Enum_Loc = Ident_2;
    do {
        if (Ch_1_Glob == 'A') {
            Int_Loc -= 1;
            *Int_Par_Ref = Int_Loc - Int_Glob;
            Enum_Loc = Ident_1;
        }
    } while (Enum_Loc != Ident_1);
}

static void Proc_4(void) {
    char Bool_Loc = Ch_1_Glob == 'A';
    Bool_Glob = Bool_Loc | Bool_Glob;
    Ch_2_Glob = 'B';
}

static void Proc_5(void) {
    Ch_1_Glob = 'A';
    Bool_Glob = 0;
}

static void Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref, int Int_1_Par_Val,
                   int Int_2_Par_Val) {
    int Int_Index;
    int Int_Loc = Int_1_Par_Val + 5;
    Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
    Arr_1_Par_Ref[Int_Loc + 1] = Arr_1_Par_Ref[Int_Loc];
    Arr_1_Par_Ref[Int_Loc + 30] = Int_Loc;
    for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index)
        Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
    Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
    Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
    Int_Glob = 5;
}

static Enumeration Func_1(char Ch_1_Par_Val, char Ch_2_Par_Val) {
    char Ch_1_Loc = Ch_1_Par_Val;
    char Ch_2_Loc = Ch_1_Loc;
    if (Ch_2_Loc != Ch_2_Par_Val) return Ident_1;
    Ch_1_Glob = Ch_1_Loc;
    return Ident_2;
}

static char Func_2(char *Str_1_Par_Ref, char *Str_2_Par_Ref) {
    int Int_Loc = 2;
    char Ch_Loc = 0;
    while (Int_Loc <= 2) {
        if (Func_1(Str_1_Par_Ref[Int_Loc], Str_2_Par_Ref[Int_Loc + 1]) == Ident_1) {
            Ch_Loc = 'A';
            Int_Loc += 1;
        }
    }
    if (Ch_Loc >= 'W' && Ch_Loc < 'Z') Int_Loc = 7;
    if (Ch_Loc == 'R') return 1;
    if (strcmp(Str_1_Par_Ref, Str_2_Par_Ref) > 0) {
        Int_Loc += 7;
        Int_Glob = Int_Loc;
        return 1;
    }
    return 0;
}

static uint16_t sum_a, sum_b;

static void fold(uint16_t v) {
    sum_a += v;
    sum_b += sum_a;
}

static void fold_string(const char *s) {
    while (*s) fold((uint8_t)*s++);
}

static void fold_record(Rec_Pointer r) {
    fold(r->Discr);
    fold(r->Enum_Comp);
    fold(r->Int_Comp);
    fold_string(r->Str_Comp);
}

int main(void) {
    int Int_1_Loc, Int_2_Loc, Int_3_Loc;
    char Ch_Index;
    Enumeration Enum_Loc;
    uint16_t Run_Index, Number_Of_Runs;

    Next_Ptr_Glob = &Next_Glob_Rec;
    Ptr_Glob = &Glob_Rec;
    Ptr_Glob->Ptr_Comp = Next_Ptr_Glob;
    Ptr_Glob->Discr = Ident_1;
    Ptr_Glob->Enum_Comp = Ident_3;
    Ptr_Glob->Int_Comp = 40;
    strcpy(Ptr_Glob->Str_Comp, "DHRYSTONE PROGRAM, SOME STRING");
    strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");
    Arr_2_Glob[8][7] = 10;
    Number_Of_Runs = runs;
    Int_1_Loc = Int_2_Loc = Int_3_Loc = 0;
    Enum_Loc = Ident_1;

    for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index) {
        Proc_5();
        Proc_4();
        Int_1_Loc = 2;
        Int_2_Loc = 3;
        strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
        Enum_Loc = Ident_2;
        Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);
        while (Int_1_Loc < Int_2_Loc) {
            Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
            Int_3_Loc = Proc_7(Int_1_Loc, Int_2_Loc);
            Int_1_Loc += 1;
        }
        Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
        Proc_1(Ptr_Glob);
        for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index) {
            if (Enum_Loc == Func_1(Ch_Index, 'C')) {
                Enum_Loc = Proc_6(Ident_1);
                strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
                Int_2_Loc = Run_Index;
                Int_Glob = Run_Index;
            }
        }
        Int_2_Loc = Int_2_Loc * Int_1_Loc;
        Int_1_Loc = Int_2_Loc / Int_3_Loc;
        Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
        Proc_2(&Int_1_Loc);
    }

    sum_a = 0;
    sum_b = 0;
    fold(Int_Glob);
    fold(Bool_Glob);
    fold(Ch_1_Glob);
    fold(Ch_2_Glob);
    fold(Arr_1_Glob[8]);
    fold(Arr_2_Glob[8][7]);
    fold_record(Ptr_Glob);
    fold_record(Next_Ptr_Glob);
    fold(Int_1_Loc);
    fold(Int_2_Loc);
    fold(Int_3_Loc);
    fold(Enum_Loc);
    fold_string(Str_1_Loc);
    fold_string(Str_2_Loc);
    checksum = sum_a ^ sum_b;
    return 0;
}
