// The interpreter against Core's script_tests: every case and its verdict are Core's. A vector Core wrote by hand
// is the script sig, the script of the output, the flags and the error; the scripts are text in the notation of
// spendBuilder.h, the same bytes as Core's. Where a script is hashed or signed, the line is the output spent, the
// script sig, the witness if there is one, then the flags and the error. Where Core's TestBuilder signs, the test
// keys sign in the same places: its key0, key1 and key2 are K1, K2, K6

#include "spendBuilder.h"

using namespace SpendTest;
using enum BTC::EScriptError;
using enum BTC::EScriptFlag;

// The empty stack, pushes, what fails only when it runs
TEST(Interpreter, Pushes)
{
  // Test the test: we should have an empty stack after scriptSig evaluation
  interpret("", "DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 2", "2 EQUALVERIFY 1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "", EScriptP2SH | EScriptStrictEnc, Ok);
  // all bytes are significant, not only the last one
  interpret("<0100>", "", EScriptP2SH | EScriptStrictEnc, Ok);
  // equals zero when cast to Int64
  interpret("<000000000000000010>", "", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<0b>", "11 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<417a>", "'Az' EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // push 75 bytes
  interpret("<417a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a7a>",
            "'Azzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz' EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("PUSHDATA1<07>", "7 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("PUSHDATA2<08>", "8 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("PUSHDATA4<09>", "9 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("PUSHDATA1<>", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("PUSHDATA2<>", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("PUSHDATA4<>", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 1000 ADD", "999 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // RESERVED is fine if not executed
  interpret("0", "IF RESERVED ENDIF 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "15 ADD 16 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  // VER non-functional (ok if not executed)
  interpret("0", "IF VER ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF RESERVED RESERVED1 RESERVED2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
}

// IF, NOTIF, ELSE, ENDIF, VERIFY, RETURN
TEST(Interpreter, Conditionals)
{
  interpret("1", "DUP IF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "IF 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "DUP IF ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "IF 1 ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1", "IF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "IF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1", "IF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "IF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "NOTIF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1", "NOTIF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "NOTIF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1", "NOTIF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  // Multiple ELSE's are valid and execution inverts on each ELSE encountered
  interpret("0", "IF 0 ELSE 1 ELSE 0 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "IF 1 ELSE 0 ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "IF ELSE 0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "IF 1 ELSE 0 ELSE 1 ENDIF ADD 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'' 1", "IF (SHA1 ELSE ELSE)*19 SHA1 ENDIF <68ca4fec736264c13b859bac43d5173df6871682> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "NOTIF 0 ELSE 1 ELSE 0 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "NOTIF 1 ELSE 0 ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "NOTIF ELSE 0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "NOTIF 1 ELSE 0 ELSE 1 ENDIF ADD 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'' 0", "NOTIF (SHA1 ELSE ELSE)*19 SHA1 ENDIF <68ca4fec736264c13b859bac43d5173df6871682> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // Nested ELSE ELSE
  interpret("0", "IF 1 IF RETURN ELSE RETURN ELSE RETURN ENDIF ELSE 1 IF 1 ELSE RETURN ELSE 1 ENDIF ELSE RETURN ENDIF ADD 2 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "NOTIF 0 NOTIF RETURN ELSE RETURN ELSE RETURN ENDIF ELSE 0 NOTIF 1 ELSE RETURN ELSE 1 ENDIF ELSE RETURN ENDIF ADD 2 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  // RETURN only works if executed
  interpret("0", "IF RETURN ENDIF 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1", "VERIFY", EScriptP2SH | EScriptStrictEnc, Ok);
  // values >4 bytes can be cast to boolean
  interpret("1 <0100000000>", "VERIFY", EScriptP2SH | EScriptStrictEnc, Ok);
  // negative 0 is false
  interpret("1 <80>", "IF 0 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
}

// The stack and the alt stack
TEST(Interpreter, StackOps)
{
  interpret("10 0 11 TOALTSTACK DROP FROMALTSTACK", "ADD 21 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'gavin_was_here' TOALTSTACK 11 FROMALTSTACK", "'gavin_was_here' EQUALVERIFY 11 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 IFDUP", "DEPTH 1 EQUALVERIFY 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 IFDUP", "DEPTH 2 EQUALVERIFY 1 EQUALVERIFY 1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // IFDUP dups non ints
  interpret("<0100000000> IFDUP", "DEPTH 2 EQUALVERIFY <0100000000> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 DROP", "DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "DUP 1 ADD 1 EQUALVERIFY 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1", "NIP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "OVER DEPTH 3 EQUALVERIFY", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "0 PICK 20 EQUALVERIFY DEPTH 3 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "1 PICK 21 EQUALVERIFY DEPTH 3 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "2 PICK 22 EQUALVERIFY DEPTH 3 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "0 ROLL 20 EQUALVERIFY DEPTH 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "1 ROLL 21 EQUALVERIFY DEPTH 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "2 ROLL 22 EQUALVERIFY DEPTH 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "ROT 22 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "ROT DROP 20 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "ROT DROP DROP 21 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "ROT ROT 21 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("22 21 20", "ROT ROT ROT 20 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT 24 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT DROP 25 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT 2DROP 20 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT 2DROP DROP 21 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT 2DROP 2DROP 22 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT 2DROP 2DROP DROP 23 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT 2ROT 22 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("25 24 23 22 21 20", "2ROT 2ROT 2ROT 20 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "SWAP 1 EQUALVERIFY 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1", "TUCK DEPTH 3 EQUALVERIFY SWAP 2DROP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("13 14", "2DUP ROT EQUALVERIFY EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 0 1 2", "3DUP DEPTH 7 EQUALVERIFY ADD ADD 3 EQUALVERIFY 2DROP 0 EQUALVERIFY", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 2 3 5", "2OVER ADD ADD 8 EQUALVERIFY ADD ADD 6 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 3 5 7", "2SWAP ADD 4 EQUALVERIFY ADD 12 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
}

// SIZE
TEST(Interpreter, Size)
{
  interpret("0", "SIZE 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "SIZE 1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("127", "SIZE 1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("128", "SIZE 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("32767", "SIZE 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("32768", "SIZE 3 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("8388607", "SIZE 3 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("8388608", "SIZE 4 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647", "SIZE 4 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483648", "SIZE 5 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffff7f>", "SIZE 5 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<000000008000>", "SIZE 6 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffffffffff7f>", "SIZE 8 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1", "SIZE 1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-127", "SIZE 1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-128", "SIZE 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-32767", "SIZE 2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-32768", "SIZE 3 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-8388607", "SIZE 3 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-8388608", "SIZE 4 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-2147483647", "SIZE 4 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-2147483648", "SIZE 5 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffffff>", "SIZE 5 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<000000008080>", "SIZE 6 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffffffffffff>", "SIZE 8 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'abcdefghijklmnopqrstuvwxyz'", "SIZE 26 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // SIZE does not consume argument
  interpret("42", "SIZE 1 EQUALVERIFY 42 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
}

// Numbers: arithmetic, comparison, WITHIN
TEST(Interpreter, Arithmetic)
{
  interpret("2 -2 ADD", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647 -2147483647 ADD", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 -1 ADD", "-2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1 ADD", "2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1ADD", "2 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("111 1SUB", "110 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("111 1 ADD 12 SUB", "100 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 ABS", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("16 ABS", "16 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-16 ABS", "-16 NEGATE EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 NOT", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 NOT", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 NOT", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0NOTEQUAL", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0NOTEQUAL", "1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("111 0NOTEQUAL", "1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-111 0NOTEQUAL", "1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1 BOOLAND", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0 BOOLAND", "NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1 BOOLAND", "NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0 BOOLAND", "NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("16 17 BOOLAND", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 1 BOOLOR", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0 BOOLOR", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1 BOOLOR", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0 BOOLOR", "NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  // <80> is negative zero, <00> a zero that is not minimal, <81> is -1
  interpret("<80>", "DUP BOOLOR", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("<00>", "DUP BOOLOR", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("<81>", "DUP BOOLOR", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<80>", "DUP BOOLAND", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("<00>", "DUP BOOLAND", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("<81>", "DUP BOOLAND", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<00>", "NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<80>", "NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<81>", "NOT", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("<80> 0", "NUMEQUAL", EScriptP2SH, Ok);
  interpret("<00> 0", "NUMEQUAL", EScriptP2SH, Ok);
  interpret("<0000> 0", "NUMEQUAL", EScriptP2SH, Ok);
  interpret("16 17 BOOLOR", "NOP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 10 1 ADD", "NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 10 1 ADD", "NUMEQUALVERIFY 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 10 1 ADD", "NUMNOTEQUAL NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("111 10 1 ADD", "NUMNOTEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 10", "LESSTHAN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("4 4", "LESSTHAN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("10 11", "LESSTHAN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 11", "LESSTHAN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 -10", "LESSTHAN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 10", "GREATERTHAN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("4 4", "GREATERTHAN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("10 11", "GREATERTHAN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 11", "GREATERTHAN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 -10", "GREATERTHAN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 10", "LESSTHANOREQUAL NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("4 4", "LESSTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("10 11", "LESSTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 11", "LESSTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 -10", "LESSTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 10", "GREATERTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("4 4", "GREATERTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("10 11", "GREATERTHANOREQUAL NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 11", "GREATERTHANOREQUAL NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-11 -10", "GREATERTHANOREQUAL NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0 MIN", "0 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1 MIN", "0 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 0 MIN", "-1 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 -2147483647 MIN", "-2147483647 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647 0 MAX", "2147483647 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 100 MAX", "100 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-100 0 MAX", "0 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 -2147483647 MAX", "0 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0 1", "WITHIN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0 1", "WITHIN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 -2147483647 2147483647", "WITHIN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 -100 100", "WITHIN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11 -100 100", "WITHIN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-2147483647 -100 100", "WITHIN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647 -100 100", "WITHIN NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647 2147483647 SUB", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // >32 bit EQUAL is valid
  interpret("2147483647 DUP ADD", "4294967294 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647 NEGATE DUP ADD", "-4294967294 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("''", "RIPEMD160 <9c1185a5c5e9fc54612808977ee8f548b2258d31> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'a'", "RIPEMD160 <0bdc9d2d256b3ee9daae347be6f4dc835a467ffe> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'abcdefghijklmnopqrstuvwxyz'", "RIPEMD160 <f71c27109c692c1b56bbdceb5b9d2865b3708dbc> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("''", "SHA1 <da39a3ee5e6b4b0d3255bfef95601890afd80709> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
}

// RIPEMD160, SHA1, SHA256, HASH160, HASH256
TEST(Interpreter, Hashes)
{
  interpret("'a'", "SHA1 <86f7e437faa5a7fce15d1ddcb9eaeaea377667b8> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'abcdefghijklmnopqrstuvwxyz'", "SHA1 <32d10c7b8cf96570ca04ce37f2a19d84240d3a89> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("''", "SHA256 <e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'a'", "SHA256 <ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'abcdefghijklmnopqrstuvwxyz'", "SHA256 <71c480df93d6ae2f1efad1447c66c9525e316218cf51fc8d9ed832f2daf18b73> EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("''", "DUP HASH160 SWAP SHA256 RIPEMD160 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("''", "DUP HASH256 SWAP SHA256 SHA256 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("''", "NOP HASH160 <b472a266d0bd89c13706a4132ccfb16f7c3b9fcb> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'a'", "HASH160 NOP <994355199e516ff76c4fa4aab39337b9d84cf12b> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'abcdefghijklmnopqrstuvwxyz'", "HASH160 PUSHDATA1<c286a1af0947f58d1ad787385b1c2c4a976f9e71> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("''", "HASH256 <5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'a'", "HASH256 <bf5d3affb73efd2ec6c36ad3112dd933efed63c4e1cbffcfa88e2759c144f2d8> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'abcdefghijklmnopqrstuvwxyz'", "HASH256 PUSHDATA1<ca139bc10c2f660da42666f72e89a225936fc60f193c161124a672050c434671> EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
}

// NOPs, opcodes unknown and not run, the limits of a stack and a script
TEST(Interpreter, NopsAndLimits)
{
  interpret("1", "NOP1 CHECKLOCKTIMEVERIFY CHECKSEQUENCEVERIFY NOP4 NOP5 NOP6 NOP7 NOP8 NOP9 NOP10 1 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("'NOP_1_to_10' NOP1 CHECKLOCKTIMEVERIFY CHECKSEQUENCEVERIFY NOP4 NOP5 NOP6 NOP7 NOP8 NOP9 NOP10", "'NOP_1_to_10' EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  // Discourage NOPx flag allows OP_NOP
  interpret("1", "NOP", EScriptP2SH | EScriptStrictEnc | EScriptDiscourageUpgradableNops, Ok);
  // Discouraged NOPs are allowed if not executed
  interpret("0", "IF NOP10 ENDIF 1", EScriptP2SH | EScriptStrictEnc | EScriptDiscourageUpgradableNops, Ok);
  // opcodes above MAX_OPCODE invalid if executed
  interpret("0", "IF CHECKSIGADD ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xbb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xbc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xbd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xbe ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xbf ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xc9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xca ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xcb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xcc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xcd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xce ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xcf ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xd9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xda ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xdb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xdc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xdd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xde ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xdf ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xe9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xea ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xeb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xec ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xed ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xee ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xef ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xf9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xfa ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xfb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xfc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xfd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xfe ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "IF 0xff ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "<62*520>", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "NOP*201", EScriptP2SH | EScriptStrictEnc, Ok);
  // 1,000 stack size
  interpret("1 2 3 4 5 3DUP*165", "1 2 3 4 5 3DUP*165", EScriptP2SH | EScriptStrictEnc, Ok);
  // 1,000 stack size (altstack cleared between scriptSig/scriptPubKey)
  interpret("1 TOALTSTACK 2 TOALTSTACK 3 4 5 3DUP*165", "1 2 3 4 5 6 7 3DUP*165", EScriptP2SH | EScriptStrictEnc, Ok);
  // Max-size (10,000-byte), max-push (520 bytes), max-opcodes (201), max stack size (1,000 items)
  interpret("<61*382> <62*520>*18 3DUP*201", "<61*382> <62*520>*18 3DUP*119 2DUP NOP*81", EScriptP2SH | EScriptStrictEnc, Ok);
  // >201 opcodes, but RESERVED doesn't count towards opcode limit
  interpret("0", "IF RESERVED*200 ENDIF 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "1", EScriptP2SH | EScriptStrictEnc, Ok);
}

// The bytes of numbers
TEST(Interpreter, NumberEncodings)
{
  interpret("1", "<01> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("127", "<7f> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // Leave room for the sign bit
  interpret("128", "<8000> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("32767", "<ff7f> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("32768", "<008000> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("8388607", "<ffff7f> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("8388608", "<00008000> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647", "<ffffff7f> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483648", "<0000008000> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffff7f>", "<ffffffff7f> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<000000008000>", "<000000008000> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffffffffff7f>", "<ffffffffffffff7f> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // Numbers are little-endian with the MSB being a sign bit
  interpret("-1", "<81> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-127", "<ff> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-128", "<8080> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-32767", "<ffff> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-32768", "<008080> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-8388607", "<ffffff> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-8388608", "<00008080> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-2147483647", "<ffffffff> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-2147483648", "<0000008080> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-4294967295", "<ffffffff80> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffffff>", "<ffffffffff> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<000000008080>", "<000000008080> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<ffffffffffffffff>", "<ffffffffffffffff> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // We can do math on 4-byte integers, and compare 5-byte ones
  interpret("2147483647", "1ADD 2147483648 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2147483647", "1ADD 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-2147483647", "1ADD 1", EScriptP2SH | EScriptStrictEnc, Ok);
  // Not the same byte array...
  interpret("1", "<0100> EQUAL NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  // ... but they are numerically equal
  interpret("1", "<0100> NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("11", "PUSHDATA1<0b0000> NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "<80> EQUAL NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  // Zero numerically equals negative zero
  interpret("0", "<80> NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "<0080> NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<000080>", "<00000080> NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<100080>", "<10000080> NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("<100000>", "<10000000> NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // The if(stack.size() < N) check of each opcode, against copy-and-paste errors
  interpret("NOP", "NOP 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "IF 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
}

// A vector to every opcode
TEST(Interpreter, EveryOpcode)
{
  interpret("0", "NOTIF 1 ENDIF", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "VERIFY 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "TOALTSTACK 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "TOALTSTACK FROMALTSTACK", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "2DROP 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1", "2DUP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0 1", "3DUP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1 0 0", "2OVER", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1 0 0 0 0", "2ROT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1 0 0", "2SWAP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "IFDUP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "DEPTH 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "DROP 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "DUP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1", "NIP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "OVER", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0 0 0 3", "PICK", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "PICK", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0 0 0 3", "ROLL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "ROLL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0 0", "ROT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "SWAP", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 1", "TUCK", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "SIZE", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "EQUALVERIFY 1", EScriptP2SH | EScriptStrictEnc, Ok);
  // OP_0 and bools must have identical byte representations
  interpret("0 0 1", "EQUAL EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "1ADD", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("2", "1SUB", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1", "NEGATE", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1", "ABS", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "NOT", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1", "0NOTEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "ADD", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "SUB", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 -1", "BOOLAND", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 0", "BOOLOR", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "NUMEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "NUMEQUALVERIFY 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 0", "NUMNOTEQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 0", "LESSTHAN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "GREATERTHAN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "LESSTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0 0", "GREATERTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 0", "MIN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1 0", "MAX", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("-1 -1 0", "WITHIN", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "RIPEMD160", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "SHA1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "SHA256", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "HASH160", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("0", "HASH256", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "CODESEPARATOR 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP1 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "CHECKLOCKTIMEVERIFY 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "CHECKSEQUENCEVERIFY 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP4 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP5 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP6 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP7 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP8 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP9 1", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("NOP", "NOP10 1", EScriptP2SH | EScriptStrictEnc, Ok);
}

// CHECKMULTISIG: the counts of keys and signatures, the opcodes it counts
TEST(Interpreter, CheckMultisigCounts)
{
  // CHECKMULTISIG is allowed to have zero keys and/or sigs
  interpret("", "0 0 0 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 0 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // Zero sigs means no sigs are checked
  interpret("", "0 0 0 1 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 0 1 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 0 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 0 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 0 1 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 0 1 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // Test from up to 20 pubkeys, all not checked
  interpret("", "0 0 'a' 'b' 2 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 3 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 4 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 5 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 6 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 7 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 8 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 9 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 10 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 11 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 12 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 13 CHECKMULTISIG VERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 14 CHECKMULTISIG VERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 15 CHECKMULTISIG VERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 16 CHECKMULTISIG VERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 17 CHECKMULTISIG VERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 18 CHECKMULTISIG VERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 19 CHECKMULTISIG VERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 't' 20 CHECKMULTISIG VERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 1 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 2 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 3 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 4 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 5 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 6 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 7 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 8 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 9 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 10 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 11 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 12 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 13 CHECKMULTISIGVERIFY DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 14 CHECKMULTISIGVERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 15 CHECKMULTISIGVERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 16 CHECKMULTISIGVERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 17 CHECKMULTISIGVERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 18 CHECKMULTISIGVERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 19 CHECKMULTISIGVERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("", "0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 't' 20 CHECKMULTISIGVERIFY DEPTH 0 EQUAL",
            EScriptP2SH | EScriptStrictEnc, Ok);
  // nOpCount is incremented by the number of keys evaluated in addition to the usual one op per op. In this case we have zero keys, so we can execute 201 CHECKMULTISIGS
  interpret("", "0 (0 0 CHECKMULTISIG)*201", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "(0 0 0 CHECKMULTISIGVERIFY)*201", EScriptP2SH | EScriptStrictEnc, Ok);
  // Even though there are no signatures being checked nOpCount is incremented by the number of keys.
  interpret("", "NOP*12 (0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 't' 20 CHECKMULTISIG)*9",
            EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("1", "NOP*12 (0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 't' 20 CHECKMULTISIGVERIFY)*9",
            EScriptP2SH | EScriptStrictEnc, Ok);
}

// P2SH at its simplest; the forms of a push
TEST(Interpreter, PushForms)
{
  interpret(P2SH(Script("1")), ScriptSig("0") << Redeem(), EScriptP2SH | EScriptStrictEnc, Ok);
  interpret(P2SH(Script("1")), ScriptSig("PUSHDATA1<>") << Redeem(), EScriptP2SH | EScriptStrictEnc, Ok);
  // Basic PUSH signedness check
  interpret("<42*64>", "PUSHDATA2<42*64> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  interpret("PUSHDATA1<42*64>", "PUSHDATA2<42*64> EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // all PUSHDATA forms are equivalent
  interpret("PUSHDATA1<11*75>", "<11*75> EQUAL", EScriptNone, Ok);
  interpret("PUSHDATA2<11*255>", "PUSHDATA1<11*255> EQUAL", EScriptNone, Ok);
  interpret("0", "SIZE 0 EQUAL", EScriptP2SH | EScriptStrictEnc, Ok);
  // OP_1NEGATE and OP_1 to OP_16 push 0x81 and 0x01 to 0x10
  interpret("<81>", "-1 EQUAL", EScriptNone, Ok);
  interpret("<01>", "1 EQUAL", EScriptNone, Ok);
  interpret("<02>", "2 EQUAL", EScriptNone, Ok);
  interpret("<03>", "3 EQUAL", EScriptNone, Ok);
  interpret("<04>", "4 EQUAL", EScriptNone, Ok);
  interpret("<05>", "5 EQUAL", EScriptNone, Ok);
  interpret("<06>", "6 EQUAL", EScriptNone, Ok);
  interpret("<07>", "7 EQUAL", EScriptNone, Ok);
  interpret("<08>", "8 EQUAL", EScriptNone, Ok);
  interpret("<09>", "9 EQUAL", EScriptNone, Ok);
  interpret("<0a>", "10 EQUAL", EScriptNone, Ok);
  interpret("<0b>", "11 EQUAL", EScriptNone, Ok);
  interpret("<0c>", "12 EQUAL", EScriptNone, Ok);
  interpret("<0d>", "13 EQUAL", EScriptNone, Ok);
  interpret("<0e>", "14 EQUAL", EScriptNone, Ok);
  interpret("<0f>", "15 EQUAL", EScriptNone, Ok);
  interpret("<10>", "16 EQUAL", EScriptNone, Ok);
  // Equivalency of different numeric encodings
  interpret("<8000>", "128 NUMEQUAL", EScriptNone, Ok);
  interpret("<00>", "0 NUMEQUAL", EScriptNone, Ok);
  interpret("<80>", "0 NUMEQUAL", EScriptNone, Ok);
  interpret("<0080>", "0 NUMEQUAL", EScriptNone, Ok);
  interpret("<0500>", "5 NUMEQUAL", EScriptNone, Ok);
  interpret("<ff7f80>", "<ffff> NUMEQUAL", EScriptNone, Ok);
  interpret("<ff7f00>", "<ff7f> NUMEQUAL", EScriptNone, Ok);
  interpret("<ffff7f80>", "<ffffff> NUMEQUAL", EScriptNone, Ok);
  interpret("<ffff7f00>", "<ffff7f> NUMEQUAL", EScriptNone, Ok);
}

// MINIMALDATA where it does not apply
TEST(Interpreter, MinimalDataValid)
{
  // Unevaluated non-minimal pushes are ignored
  interpret("0 IF PUSHDATA1<> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF PUSHDATA2<> ENDIF 1", "", EScriptMinimalData, Ok);
  // Core means PUSHDATA4 here, but its vector starts with 0x4c: PUSHDATA1 of nothing and three OP_0
  interpret("0 IF PUSHDATA1<> 0 0 0 ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <81> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <01> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <02> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <03> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <04> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <05> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <06> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <07> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <08> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <09> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <0a> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <0b> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <0c> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <0d> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <0e> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <0f> ENDIF 1", "", EScriptMinimalData, Ok);
  interpret("0 IF <10> ENDIF 1", "", EScriptMinimalData, Ok);
  // Numeric minimaldata rules are only applied when a stack item is numerically evaluated; the push itself is allowed
  interpret("<00>", "1", EScriptMinimalData, Ok);
  interpret("<80>", "1", EScriptMinimalData, Ok);
  interpret("<0180>", "1", EScriptMinimalData, Ok);
  interpret("<0100>", "1", EScriptMinimalData, Ok);
  interpret("<0200>", "1", EScriptMinimalData, Ok);
  interpret("<0300>", "1", EScriptMinimalData, Ok);
  interpret("<0400>", "1", EScriptMinimalData, Ok);
  interpret("<0500>", "1", EScriptMinimalData, Ok);
  interpret("<0600>", "1", EScriptMinimalData, Ok);
  interpret("<0700>", "1", EScriptMinimalData, Ok);
  interpret("<0800>", "1", EScriptMinimalData, Ok);
  interpret("<0900>", "1", EScriptMinimalData, Ok);
  interpret("<0a00>", "1", EScriptMinimalData, Ok);
  interpret("<0b00>", "1", EScriptMinimalData, Ok);
  interpret("<0c00>", "1", EScriptMinimalData, Ok);
  interpret("<0d00>", "1", EScriptMinimalData, Ok);
  interpret("<0e00>", "1", EScriptMinimalData, Ok);
  interpret("<0f00>", "1", EScriptMinimalData, Ok);
  interpret("<1000>", "1", EScriptMinimalData, Ok);
  // Every opcode that takes a number, its operands minimal: the failing versions are in MinimalData
  interpret("1 <0000>", "PICK DROP", EScriptNone, Ok);
  interpret("1 <0000>", "ROLL DROP 1", EScriptNone, Ok);
  interpret("<0000>", "1ADD DROP 1", EScriptNone, Ok);
  interpret("<0000>", "1SUB DROP 1", EScriptNone, Ok);
  interpret("<0000>", "NEGATE DROP 1", EScriptNone, Ok);
  interpret("<0000>", "ABS DROP 1", EScriptNone, Ok);
  interpret("<0000>", "NOT DROP 1", EScriptNone, Ok);
  interpret("<0000>", "0NOTEQUAL DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "ADD DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "ADD DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "SUB DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "SUB DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "BOOLAND DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "BOOLAND DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "BOOLOR DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "BOOLOR DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "NUMEQUAL DROP 1", EScriptNone, Ok);
  interpret("<0000> 1", "NUMEQUAL DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "NUMEQUALVERIFY 1", EScriptNone, Ok);
  interpret("<0000> 0", "NUMEQUALVERIFY 1", EScriptNone, Ok);
  interpret("0 <0000>", "NUMNOTEQUAL DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "NUMNOTEQUAL DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "LESSTHAN DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "LESSTHAN DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "GREATERTHAN DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "GREATERTHAN DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "LESSTHANOREQUAL DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "LESSTHANOREQUAL DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "GREATERTHANOREQUAL DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "GREATERTHANOREQUAL DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "MIN DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "MIN DROP 1", EScriptNone, Ok);
  interpret("0 <0000>", "MAX DROP 1", EScriptNone, Ok);
  interpret("<0000> 0", "MAX DROP 1", EScriptNone, Ok);
  interpret("<0000> 0 0", "WITHIN DROP 1", EScriptNone, Ok);
  interpret("0 <0000> 0", "WITHIN DROP 1", EScriptNone, Ok);
  interpret("0 0 <0000>", "WITHIN DROP 1", EScriptNone, Ok);
  interpret("0 0 <0000>", "CHECKMULTISIG DROP 1", EScriptNone, Ok);
  interpret("0 <0000> 0", "CHECKMULTISIG DROP 1", EScriptNone, Ok);
  interpret("0 <0000> 0 1", "CHECKMULTISIG DROP 1", EScriptNone, Ok);
  interpret("0 0 <0000>", "CHECKMULTISIGVERIFY 1", EScriptNone, Ok);
  interpret("0 <0000> 0", "CHECKMULTISIGVERIFY 1", EScriptNone, Ok);
}

// The empty signature, the order CHECKMULTISIG takes keys and signatures in, DERSIG
TEST(Interpreter, SignatureEncodingValid)
{
  // While not really correctly DER encoded, the empty signature is allowed by
  // STRICTENC to provide a compact way to provide a deliberately invalid signature.
  interpret("0", "<02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> CHECKSIG NOT", EScriptStrictEnc, Ok);
  interpret("0 0", "1 <02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> 1 CHECKMULTISIG NOT", EScriptStrictEnc, Ok);
  // CHECKMULTISIG evaluation order tests. CHECKMULTISIG evaluates signatures and
  // pubkeys in a specific order, and will exit early if the number of signatures
  // left to check is greater than the number of keys left. As STRICTENC fails the
  // script when it reaches an invalidly encoded signature or pubkey, we can use it
  // to test the exact order in which signatures and pubkeys are evaluated by
  // distinguishing CHECKMULTISIG returning false on the stack and the script as a
  // whole failing.
  // 2-of-2 CHECKMULTISIG NOT with the second pubkey invalid, and both signatures validly encoded. Valid pubkey fails, and CHECKMULTISIG exits early, prior to evaluation of second invalid pubkey.
  interpret("0 "
            "<3044022044dc17b0887c161bb67ba9635bf758735bdde503e4b0a0987f587f14a4e1143d022009a215772d49a85dae40d8ca03955af26ad3978a0ff965faa12915e9586249a501> "
            "<3044022044dc17b0887c161bb67ba9635bf758735bdde503e4b0a0987f587f14a4e1143d022009a215772d49a85dae40d8ca03955af26ad3978a0ff965faa12915e9586249a501>",
            "2 0 <02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> 2 CHECKMULTISIG NOT", EScriptStrictEnc, Ok);
  // 2-of-2 CHECKMULTISIG NOT with both pubkeys valid, but second signature invalid. Valid pubkey fails, and CHECKMULTISIG exits early, prior to evaluation of second invalid signature.
  interpret("0 0 "
            "<3044022044dc17b0887c161bb67ba9635bf758735bdde503e4b0a0987f587f14a4e1143d022009a215772d49a85dae40d8ca03955af26ad3978a0ff965faa12915e9586249a501>",
            "2 <02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> "
            "<02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> 2 CHECKMULTISIG NOT", EScriptStrictEnc, Ok);
  // Overly long signature is correctly encoded
  interpret("<00*74>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // Missing S is correctly encoded
  interpret("<30220220000000000000000000000000000000000000000000000000000000000000000000>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // S with invalid S length is correctly encoded
  interpret("<3024021077777777777777777777777777777777020a7777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // Non-integer R is correctly encoded
  interpret("<302403107777777777777777777777777777777702107777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // Non-integer S is correctly encoded
  interpret("<302402107777777777777777777777777777777703107777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // Zero-length R is correctly encoded
  interpret("<3014020002107777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // Zero-length S is correctly encoded for DERSIG
  interpret("<3014021077777777777777777777777777777777020001>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // Negative S is correctly encoded
  interpret("<302402107777777777777777777777777777777702108777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptNone, Ok);
  // CSV passes if stack top bit 1 << 31 is set
  interpret("2147483648", "CHECKSEQUENCEVERIFY", EScriptCheckSequenceVerify, Ok);
}

// What leaves the stack empty or false
TEST(Interpreter, InvalidBasics)
{
  // Test the test: we should have an empty stack after scriptSig evaluation
  interpret("", "DEPTH", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("", "", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("", "NOP", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("", "NOP DEPTH", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "DEPTH", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "NOP", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "NOP DEPTH", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("DEPTH", "", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  // PUSHDATA1 with not enough bytes: the opcode, the length, and fewer bytes than it says
  interpret("PUSHDATA1 0x01", "<61>", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // PUSHDATA2 with not enough bytes
  interpret("PUSHDATA2 0x0200 0xff", "<61>", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // PUSHDATA4 with not enough bytes
  interpret("PUSHDATA4 0x03000000 0xffff", "<61>", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF RESERVED ENDIF 1", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("2", "15 ADD 16 EQUAL", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0", "NOP", EScriptP2SH | EScriptStrictEnc, EvalFalse);
}

// Conditionals that do not balance, VERIF, RETURN
TEST(Interpreter, InvalidConditionals)
{
  interpret("1", "IF VER ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // VERIF and VERNOTIF are illegal everywhere, run or not
  interpret("0", "IF VERIF ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("0", "IF ELSE 1 ELSE VERIF ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("0", "IF VERNOTIF ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("0", "IF ELSE 1 ELSE VERNOTIF ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // IF/ENDIF can't span scriptSig/scriptPubKey
  interpret("1 IF", "1 ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1 IF 0 ENDIF", "1 ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1 ELSE 0 ENDIF", "1", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("0 NOTIF", "123", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("0", "DUP IF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0", "IF 1 ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0", "DUP IF ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0", "IF 1 ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0", "NOTIF ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0 1", "IF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0 0", "IF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("1 0", "IF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0 1", "IF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0 0", "NOTIF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0 1", "NOTIF IF 1 ELSE 0 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("1 1", "NOTIF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("0 0", "NOTIF IF 1 ELSE 0 ENDIF ELSE IF 0 ELSE 1 ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("1", "IF RETURN ELSE ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, OpReturn);
  interpret("1", "IF 1 ELSE ELSE RETURN ENDIF", EScriptP2SH | EScriptStrictEnc, OpReturn);
  interpret("1", "ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "ENDIF ELSE", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "ENDIF ELSE IF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "IF ELSE ENDIF ELSE", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "IF ELSE ENDIF ELSE ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "IF ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "IF ELSE ELSE ENDIF ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  // Core's output is RETURN itself. No utxo keeps an output that starts with it: here it is the redeem script
  interpret(P2SH(Script("RETURN")), ScriptSig("1") << Redeem(), EScriptP2SH | EScriptStrictEnc, OpReturn);
  interpret("1", "DUP IF RETURN ENDIF", EScriptP2SH | EScriptStrictEnc, OpReturn);
  // canonical prunable txout format, the redeem script again
  interpret(P2SH(Script("RETURN 'data'")), ScriptSig("1") << Redeem(), EScriptP2SH | EScriptStrictEnc, OpReturn);
  // Core's output is RETURN ENDIF 1, prunable; the script sig fails on its own, its IF does not reach the output
  interpret("0 IF", "ENDIF 1", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
}

// Stack operations with too little on the stack
TEST(Interpreter, InvalidStackOps)
{
  interpret("0", "VERIFY 1", EScriptP2SH | EScriptStrictEnc, Verify);
  interpret("1", "VERIFY", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("1", "VERIFY 0", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  // alt stack not shared between sig/pubkey
  interpret("1 TOALTSTACK", "FROMALTSTACK 1", EScriptP2SH | EScriptStrictEnc, InvalidAltstackOperation);
  interpret("IFDUP", "DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("DROP", "DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("DUP", "DEPTH 0 EQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "DUP 1 ADD 2 EQUALVERIFY 0 EQUAL", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "NIP", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "1 NIP", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "1 0 NIP", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "OVER 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "OVER", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("0 1", "OVER DEPTH 3 EQUALVERIFY", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("19 20 21", "PICK 19 EQUALVERIFY DEPTH 2 EQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "0 PICK", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "-1 PICK", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("19 20 21", "0 PICK 20 EQUALVERIFY DEPTH 3 EQUAL", EScriptP2SH | EScriptStrictEnc, EqualVerify);
  interpret("19 20 21", "1 PICK 21 EQUALVERIFY DEPTH 3 EQUAL", EScriptP2SH | EScriptStrictEnc, EqualVerify);
  interpret("19 20 21", "2 PICK 22 EQUALVERIFY DEPTH 3 EQUAL", EScriptP2SH | EScriptStrictEnc, EqualVerify);
  interpret("NOP", "0 ROLL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "-1 ROLL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("19 20 21", "0 ROLL 20 EQUALVERIFY DEPTH 2 EQUAL", EScriptP2SH | EScriptStrictEnc, EqualVerify);
  interpret("19 20 21", "1 ROLL 21 EQUALVERIFY DEPTH 2 EQUAL", EScriptP2SH | EScriptStrictEnc, EqualVerify);
  interpret("19 20 21", "2 ROLL 22 EQUALVERIFY DEPTH 2 EQUAL", EScriptP2SH | EScriptStrictEnc, EqualVerify);
  interpret("NOP", "ROT 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "1 ROT 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "1 2 ROT 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "0 1 2 ROT", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "SWAP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "SWAP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("0 1", "SWAP 1 EQUALVERIFY", EScriptP2SH | EScriptStrictEnc, EqualVerify);
  interpret("NOP", "TUCK 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "TUCK 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 0", "TUCK DEPTH 3 EQUALVERIFY SWAP 2DROP", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("NOP", "2DUP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "2DUP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "3DUP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "3DUP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 2", "3DUP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "2OVER 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "2 3 2OVER 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "2SWAP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "2 3 2SWAP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "SIZE 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
}

// The disabled opcodes (CVE-2010-5137)
TEST(Interpreter, DisabledOpcodes)
{
  interpret("'a' 'b'", "CAT", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("'a' 'b' 0", "IF CAT ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("'abc' 1 1", "SUBSTR", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("'abc' 1 1 0", "IF SUBSTR ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("'abc' 2 0", "IF LEFT ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("'abc' 2 0", "IF RIGHT ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("'abc'", "IF INVERT ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("1 2 0 IF AND ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("1 2 0 IF OR ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("1 2 0 IF XOR ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 0 IF 2MUL ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 0 IF 2DIV ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2 0 IF MUL ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2 0 IF DIV ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2 0 IF MOD ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2 0 IF LSHIFT ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2 0 IF RSHIFT ELSE 1 ENDIF", "NOP", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("", "EQUAL NOT", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("0", "EQUAL NOT", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("0 1", "EQUAL", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("1 1 ADD", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("11 1 ADD 12 SUB", "11 EQUAL", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  // arithmetic operands must be in range [-2^31...2^31]
  interpret("2147483648 0 ADD", "NOP", EScriptP2SH | EScriptStrictEnc, UnknownError);
  interpret("-2147483648 0 ADD", "NOP", EScriptP2SH | EScriptStrictEnc, UnknownError);
  // NUMEQUAL must be in numeric range
  interpret("2147483647 DUP ADD", "4294967294 NUMEQUAL", EScriptP2SH | EScriptStrictEnc, UnknownError);
  // NOT is an arithmetic operand
  interpret("'abcdef' NOT", "0 EQUAL", EScriptP2SH | EScriptStrictEnc, UnknownError);
  interpret("2 DUP MUL", "4 EQUAL", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 DUP DIV", "1 EQUAL", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2MUL", "4 EQUAL", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2DIV", "1 EQUAL", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("7 3 MOD", "1 EQUAL", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 2 LSHIFT", "8 EQUAL", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("2 1 RSHIFT", "1 EQUAL", EScriptP2SH | EScriptStrictEnc, DisabledOpcode);
  interpret("1", "NOP1 CHECKLOCKTIMEVERIFY CHECKSEQUENCEVERIFY NOP4 NOP5 NOP6 NOP7 NOP8 NOP9 NOP10 2 EQUAL",
            EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("'NOP_1_to_10' NOP1 CHECKLOCKTIMEVERIFY CHECKSEQUENCEVERIFY NOP4 NOP5 NOP6 NOP7 NOP8 NOP9 NOP10", "'NOP_1_to_11' EQUAL",
            EScriptP2SH | EScriptStrictEnc, EvalFalse);
}

// DISCOURAGE_UPGRADABLE_NOPS
TEST(Interpreter, DiscouragedNops)
{
  interpret("1", "NOP1", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("1", "NOP4", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("1", "NOP5", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("1", "NOP6", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("1", "NOP7", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("1", "NOP8", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("1", "NOP9", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("1", "NOP10", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("NOP10", "1", EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret(P2SH(Script("NOP10")), ScriptSig("1") << Redeem(), EScriptP2SH | EScriptDiscourageUpgradableNops, DiscourageUpgradableNops);
  interpret("RESERVED", "1", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // opcodes above MAX_OPCODE invalid if executed
  interpret("1", "IF CHECKSIGADD ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
}

// Opcodes with no meaning, run
TEST(Interpreter, BadOpcodes)
{
  interpret("1", "IF 0xbb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xbc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xbd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xbe ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xbf ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xc9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xca ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xcb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xcc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xcd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xce ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xcf ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xd9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xda ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xdb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xdc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xdd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xde ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xdf ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xe9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xea ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xeb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xec ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xed ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xee ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xef ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf0 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf1 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf2 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf3 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf4 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf5 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf6 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf7 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf8 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xf9 ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xfa ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xfb ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xfc ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xfd ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xfe ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "IF 0xff ELSE 1 ENDIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // invalid because scriptSig and scriptPubKey are processed separately
  interpret("1 IF 1 ELSE", "0xff ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
}

// Over the limits: a push, the opcodes, the stack
TEST(Interpreter, InvalidLimits)
{
  interpret("NOP", "RIPEMD160", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "SHA1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "SHA256", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "HASH160", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "HASH256", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "<62*521>", EScriptP2SH | EScriptStrictEnc, PushSize);
  // >520 byte push in non-executed IF branch
  interpret("0", "IF <62*521> ENDIF 1", EScriptP2SH | EScriptStrictEnc, PushSize);
  interpret("1", "NOP*202", EScriptP2SH | EScriptStrictEnc, OpCount);
  // >201 opcodes including non-executed IF branch
  interpret("0", "IF NOP*200 ENDIF 1", EScriptP2SH | EScriptStrictEnc, OpCount);
  // >1,000 stack size
  interpret("1 2 3 4 5 3DUP*165", "1 2 3 4 5 6 3DUP*165", EScriptP2SH | EScriptStrictEnc, StackSize);
  // >1,000 stack+altstack size
  interpret("1 2 3 4 5 3DUP*165", "1 TOALTSTACK 2 TOALTSTACK 3 4 5 6 3DUP*165", EScriptP2SH | EScriptStrictEnc, StackSize);
  // 10,001-byte scriptPubKey
  interpret("NOP", "0 <61*382> <62*520>*18 3DUP*119 2DUP NOP*81", EScriptP2SH | EScriptStrictEnc, ScriptSize);
}

// Reserved opcodes, numbers out of range, operands missing
TEST(Interpreter, InvalidOperands)
{
  interpret("NOP1", "NOP10", EScriptP2SH | EScriptStrictEnc, EvalFalse);
  interpret("1", "VER", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "VERIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "VERNOTIF", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "RESERVED", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "RESERVED1", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("1", "RESERVED2", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // CHECKSIGADD is past the last opcode of a script that is not tapscript
  interpret("1", "CHECKSIGADD", EScriptP2SH | EScriptStrictEnc, BadOpcode);
  // We cannot do math on 5-byte integers
  interpret("2147483648", "1ADD 1", EScriptP2SH | EScriptStrictEnc, UnknownError);
  interpret("2147483648", "NEGATE 1", EScriptP2SH | EScriptStrictEnc, UnknownError);
  // Because we use a sign bit, -2147483648 is also 5 bytes
  interpret("-2147483648", "1ADD 1", EScriptP2SH | EScriptStrictEnc, UnknownError);
  // We cannot do math on 5-byte integers, even if the result is 4-bytes
  interpret("2147483647", "1ADD 1SUB 1", EScriptP2SH | EScriptStrictEnc, UnknownError);
  interpret("2147483648", "1SUB 1", EScriptP2SH | EScriptStrictEnc, UnknownError);
  // We cannot do BOOLOR on 5-byte integers (but we can still do IF etc)
  interpret("2147483648 1", "BOOLOR 1", EScriptP2SH | EScriptStrictEnc, UnknownError);
  // We cannot do BOOLAND on 5-byte integers
  interpret("2147483648 1", "BOOLAND 1", EScriptP2SH | EScriptStrictEnc, UnknownError);
  interpret("1", "1 ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("1", "IF 1", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  // IFs don't carry over
  interpret("1 IF 1", "ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  // The if(stack.size() < N) check of each opcode, against copy-and-paste errors
  interpret("NOP", "IF 1 ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("NOP", "NOTIF 1 ENDIF", EScriptP2SH | EScriptStrictEnc, UnbalancedConditional);
  interpret("NOP", "VERIFY 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "TOALTSTACK 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "FROMALTSTACK", EScriptP2SH | EScriptStrictEnc, InvalidAltstackOperation);
  interpret("1", "2DROP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "2DUP", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1", "3DUP", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1 1", "2OVER", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1 1 1 1", "2ROT", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1 1", "2SWAP", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "IFDUP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "DROP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "DUP 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "NIP", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "OVER", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1 1 3", "PICK", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("0", "PICK 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1 1 3", "ROLL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("0", "ROLL 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1", "ROT", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "SWAP", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "TUCK", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "SIZE 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "EQUAL 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "EQUALVERIFY 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "1ADD 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "1SUB 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "NEGATE 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "ABS 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "NOT 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "0NOTEQUAL 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "ADD", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "SUB", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "BOOLAND", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "BOOLOR", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "NUMEQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "NUMEQUALVERIFY 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "NUMNOTEQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "LESSTHAN", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "GREATERTHAN", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "LESSTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "GREATERTHANOREQUAL", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "MIN", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1", "MAX", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("1 1", "WITHIN", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "RIPEMD160 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "SHA1 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "SHA256 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "HASH160 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  interpret("NOP", "HASH256 1", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
}

// CHECKSIG and CHECKMULTISIG with bad operands, SIGPUSHONLY
TEST(Interpreter, InvalidCheckSig)
{
  interpret("", "CHECKSIG NOT", EScriptStrictEnc, InvalidStackOperation);
  interpret("0", "CHECKSIG NOT", EScriptStrictEnc, InvalidStackOperation);
  interpret("", "CHECKMULTISIG NOT", EScriptStrictEnc, InvalidStackOperation);
  interpret("", "-1 CHECKMULTISIG NOT", EScriptStrictEnc, PubKeyCount);
  interpret("", "1 CHECKMULTISIG NOT", EScriptStrictEnc, InvalidStackOperation);
  interpret("", "-1 0 CHECKMULTISIG NOT", EScriptStrictEnc, SigCount);
  interpret("", "1 'pk1' 1 CHECKMULTISIG NOT", EScriptStrictEnc, InvalidStackOperation);
  // CHECKMULTISIG must push false to stack when signature is invalid when NOT in strict enc mode
  interpret("", "'dummy' 'sig1' 1 'pk1' 1 CHECKMULTISIG IF 1 ENDIF", EScriptNone, EvalFalse);
  // 202 CHECKMULTISIGS, fails due to 201 op limit
  interpret("", "0 (0 0 CHECKMULTISIG)*202", EScriptP2SH | EScriptStrictEnc, OpCount);
  interpret("1", "0 (0 0 CHECKMULTISIGVERIFY)*202", EScriptP2SH | EScriptStrictEnc, InvalidStackOperation);
  // Fails due to 201 script operation limit
  interpret("", "NOP*13 (0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 't' 20 CHECKMULTISIG)*9",
            EScriptP2SH | EScriptStrictEnc, OpCount);
  interpret("1", "NOP*13 (0 0 'a' 'b' 'c' 'd' 'e' 'f' 'g' 'h' 'i' 'j' 'k' 'l' 'm' 'n' 'o' 'p' 'q' 'r' 's' 't' 20 CHECKMULTISIGVERIFY)*9",
            EScriptP2SH | EScriptStrictEnc, OpCount);
  // nPubKeys > 20
  interpret("0 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21", "21 CHECKMULTISIG 1", EScriptP2SH | EScriptStrictEnc, PubKeyCount);
  // nSigs > nPubKeys
  interpret("0 'sig' 1 0", "CHECKMULTISIG 1", EScriptP2SH | EScriptStrictEnc, SigCount);
  // Tests for Script.IsPushOnly()
  interpret(P2SH(Script("1")), ScriptSig("NOP") << Redeem(), EScriptP2SH | EScriptStrictEnc, SigPushOnly);
  interpret(P2SH(Script("1")), ScriptSig("NOP1") << Redeem(), EScriptP2SH | EScriptStrictEnc, SigPushOnly);
  interpret(P2SH(Script("RESERVED")), ScriptSig("0") << Redeem(), EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret(P2SH(Script("VER")), ScriptSig("0") << Redeem(), EScriptP2SH | EScriptStrictEnc, BadOpcode);
  interpret("0", "'00' EQUAL", EScriptP2SH | EScriptStrictEnc, EvalFalse);
}

// MINIMALDATA: pushes and numbers
TEST(Interpreter, MinimalData)
{
  // MINIMALDATA enforcement for PUSHDATAs
  // Empty vector minimally represented by OP_0
  interpret("PUSHDATA1<>", "DROP 1", EScriptMinimalData, MinimalData);
  // -1 minimally represented by OP_1NEGATE
  interpret("<81>", "DROP 1", EScriptMinimalData, MinimalData);
  // 1 to 16 minimally represented by OP_1 to OP_16
  interpret("<01>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<02>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<03>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<04>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<05>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<06>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<07>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<08>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<09>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<0a>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<0b>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<0c>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<0d>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<0e>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<0f>", "DROP 1", EScriptMinimalData, MinimalData);
  interpret("<10>", "DROP 1", EScriptMinimalData, MinimalData);
  // PUSHDATA1 of 72 bytes minimally represented by direct push
  interpret("PUSHDATA1<11*72>", "DROP 1", EScriptMinimalData, MinimalData);
  // PUSHDATA2 of 255 bytes minimally represented by PUSHDATA1
  interpret("PUSHDATA2<11*255>", "DROP 1", EScriptMinimalData, MinimalData);
  // PUSHDATA4 of 256 bytes minimally represented by PUSHDATA2
  interpret("PUSHDATA4<11*256>", "DROP 1", EScriptMinimalData, MinimalData);
  // MINIMALDATA enforcement for numeric arguments
  interpret("<00>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<80>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0080>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0500>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<050000>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0580>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<050080>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  // Minimal encoding is 0xffff
  interpret("<ff7f80>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  // Minimal encoding is 0xff7f
  interpret("<ff7f00>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  // Minimal encoding is 0xffffff
  interpret("<ffff7f80>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  // Minimal encoding is 0xffff7f
  interpret("<ffff7f00>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  // Test every numeric-accepting opcode for correct handling of the numeric minimal encoding rule
  interpret("1 <0000>", "PICK DROP", EScriptMinimalData, UnknownError);
  interpret("1 <0000>", "ROLL DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000>", "1ADD DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000>", "1SUB DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000>", "NEGATE DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000>", "ABS DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000>", "NOT DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000>", "0NOTEQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "ADD DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "ADD DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "SUB DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "SUB DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "BOOLAND DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "BOOLAND DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "BOOLOR DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "BOOLOR DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "NUMEQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 1", "NUMEQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "NUMEQUALVERIFY 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "NUMEQUALVERIFY 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "NUMNOTEQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "NUMNOTEQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "LESSTHAN DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "LESSTHAN DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "GREATERTHAN DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "GREATERTHAN DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "LESSTHANOREQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "LESSTHANOREQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "GREATERTHANOREQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "GREATERTHANOREQUAL DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "MIN DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "MIN DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000>", "MAX DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0", "MAX DROP 1", EScriptMinimalData, UnknownError);
  interpret("<0000> 0 0", "WITHIN DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000> 0", "WITHIN DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 0 <0000>", "WITHIN DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 0 <0000>", "CHECKMULTISIG DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000> 0", "CHECKMULTISIG DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000> 0 1", "CHECKMULTISIG DROP 1", EScriptMinimalData, UnknownError);
  interpret("0 0 <0000>", "CHECKMULTISIGVERIFY 1", EScriptMinimalData, UnknownError);
  interpret("0 <0000> 0", "CHECKMULTISIGVERIFY 1", EScriptMinimalData, UnknownError);
}

// The order of CHECKMULTISIG again, DERSIG
TEST(Interpreter, SignatureEncodingInvalid)
{
  // Order of CHECKMULTISIG evaluation tests, inverted by swapping the order of
  // pubkeys/signatures so they fail due to the STRICTENC rules on validly encoded
  // signatures and pubkeys.
  // 2-of-2 CHECKMULTISIG NOT with the first pubkey invalid, and both signatures validly encoded.
  interpret("0 "
            "<3044022044dc17b0887c161bb67ba9635bf758735bdde503e4b0a0987f587f14a4e1143d022009a215772d49a85dae40d8ca03955af26ad3978a0ff965faa12915e9586249a501> "
            "<3044022044dc17b0887c161bb67ba9635bf758735bdde503e4b0a0987f587f14a4e1143d022009a215772d49a85dae40d8ca03955af26ad3978a0ff965faa12915e9586249a501>",
            "2 <02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> 0 2 CHECKMULTISIG NOT", EScriptStrictEnc, PubKeyType);
  // 2-of-2 CHECKMULTISIG NOT with both pubkeys valid, but first signature invalid.
  interpret("0 "
            "<3044022044dc17b0887c161bb67ba9635bf758735bdde503e4b0a0987f587f14a4e1143d022009a215772d49a85dae40d8ca03955af26ad3978a0ff965faa12915e9586249a501> "
            "1", "2 <02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> "
            "<02865c40293a680cb9c020e7b1e106d8c1916d3cef99aa431a56d253e69256dac0> 2 CHECKMULTISIG NOT", EScriptStrictEnc, SigDer);
  // 2-of-3 with one valid and one invalid signature due to parse error, nSigs > validSigs
  interpret("0 "
            "<304402205451ce65ad844dbb978b8bdedf5082e33b43cae8279c30f2c74d9e9ee49a94f802203fe95a7ccf74da7a232ee523ef4a53cb4d14bdd16289680cdb97a63819b8f42f01> "
            "<304402205451ce65ad844dbb978b8bdedf5082e33b43cae8279c30f2c74d9e9ee49a94f802203fe95a7ccf74da7a232ee523ef4a53cb4d14bdd16289680cdb97a63819b8f42f>",
            "2 <02a673638cb9587cb68ea08dbef685c6f2d2a751a8b3c6f2a7e9a4999e6e4bfaf5> "
            "<02a673638cb9587cb68ea08dbef685c6f2d2a751a8b3c6f2a7e9a4999e6e4bfaf5> "
            "<02a673638cb9587cb68ea08dbef685c6f2d2a751a8b3c6f2a7e9a4999e6e4bfaf5> 3 CHECKMULTISIG", EScriptP2SH | EScriptStrictEnc, SigDer);
  // Overly long signature is incorrectly encoded for DERSIG
  interpret("<00*74>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
  // Missing S is incorrectly encoded for DERSIG
  interpret("<30220220000000000000000000000000000000000000000000000000000000000000000000>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
  // S with invalid S length is incorrectly encoded for DERSIG
  interpret("<3024021077777777777777777777777777777777020a7777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
  // Non-integer R is incorrectly encoded for DERSIG
  interpret("<302403107777777777777777777777777777777702107777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
  // Non-integer S is incorrectly encoded for DERSIG
  interpret("<302402107777777777777777777777777777777703107777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
  // Zero-length R is incorrectly encoded for DERSIG
  interpret("<3014020002107777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
  // Zero-length S is incorrectly encoded for DERSIG
  interpret("<3014021077777777777777777777777777777777020001>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
  // Negative S is incorrectly encoded for DERSIG
  interpret("<302402107777777777777777777777777777777702108777777777777777777777777777777701>", "0 CHECKSIG NOT", EScriptDerSig, SigDer);
}

// Witness programs at their simplest
TEST(Interpreter, WitnessBasics)
{
  interpret(P2WSH(Script("0")), ScriptSig(), Witness() << WitnessScript(), EScriptP2SH | EScriptWitness, EvalFalse);
  // Witness script hash mismatch
  interpret(Bare(Script("0 <6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d>")), ScriptSig(), Witness() << "51"_hex,
            EScriptP2SH | EScriptWitness, WitnessProgramMismatch);
  interpret(P2WSH(Script("0")), ScriptSig(), Witness() << WitnessScript(), EScriptNone, Ok);
  // Witness script hash mismatch without WITNESS
  interpret(Bare(Script("0 <6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d>")), ScriptSig(), Witness() << "51"_hex,
            EScriptNone, Ok);
  // OP_1NEGATE does not introduce a witness program
  interpret(Bare(Script("-1 <1234>")), ScriptSig(), Witness() << "51"_hex, EScriptP2SH | EScriptWitness, WitnessUnexpected);
  // OP_1 does introduce a witness program
  interpret(Bare(Script("1 <1234>")), ScriptSig("0"), Witness() << "51"_hex, EScriptP2SH | EScriptWitness, WitnessMalleated);
  // OP_16 does introduce a witness program
  interpret(Bare(Script("16 <1234>")), ScriptSig("0"), Witness() << "51"_hex, EScriptP2SH | EScriptWitness, WitnessMalleated);
  // NOP does not introduce a witness program
  interpret(Bare(Script("NOP <1234>")), ScriptSig(), Witness() << "51"_hex, EScriptP2SH | EScriptWitness, WitnessUnexpected);
}

// P2PK, P2PKH, P2SH and bare multisig with real signatures
TEST(Interpreter, SignedTemplates)
{
  interpret(P2PK(K1u), ScriptSig() << Sig(), EScriptNone, Ok);
  interpret(P2PK(K1u), ScriptSig() << Sig().FlipR(), EScriptNone, EvalFalse);
  interpret(P2PKH(K2), ScriptSig() << Sig() << K2, EScriptNone, Ok);
  interpret(P2PKH(K6), ScriptSig() << Sig() << K2, EScriptNone, EqualVerify);
  interpret(P2PK(K2u), ScriptSig() << Sig(SIGHASH_ALL | SIGHASH_ANYONECANPAY), EScriptNone, Ok);
  interpret(P2PK(K2u), ScriptSig() << Sig(SIGHASH_ALL | SIGHASH_ANYONECANPAY).AsType(SIGHASH_ALL), EScriptNone, EvalFalse);
  interpret(P2SH(P2PK(K1)), ScriptSig() << Sig() << Redeem(), EScriptP2SH, Ok);
  interpret(P2SH(P2PK(K1)), ScriptSig() << Sig() << Code(Script() << K2 << OP_CHECKSIG), EScriptP2SH, EvalFalse);
  interpret(P2SH(P2PKH(K1u)), ScriptSig() << Sig() << K1u << Redeem(), EScriptP2SH, Ok);
  // A bad signature, but without P2SH the script hashed does not run
  interpret(P2SH(P2PKH(K2u)), ScriptSig() << Sig().By(K1).FlipR() << Redeem(), EScriptNone, Ok);
  interpret(P2SH(P2PKH(K2u)), ScriptSig() << Sig().By(K1).FlipR() << Redeem(), EScriptP2SH, EqualVerify);
  interpret(Bare(Script() << 3 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K1) << Sig().By(K2) << Sig().By(K6),
            EScriptNone, Ok);
  interpret(Bare(Script() << 3 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K1) << Sig().By(K2) << 0,
            EScriptNone, EvalFalse);
  interpret(P2SH(Script() << 2 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2) << Sig().By(K6) << Redeem(),
            EScriptP2SH, Ok);
  interpret(P2SH(Script() << 2 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2) << 0 << Redeem(),
            EScriptP2SH, EvalFalse);
}

// DERSIG on real signatures: the padding of R and S, BIP66's examples, a hash type of two bytes
TEST(Interpreter, Bip66)
{
  interpret(P2PK(K2), ScriptSig() << Sig().PadR(), EScriptNone, Ok);
  interpret(P2PK(K2), ScriptSig() << Sig().PadR(), EScriptDerSig, SigDer);
  interpret(P2PK(K2), ScriptSig() << Sig().PadS(), EScriptNone, Ok);
  interpret(P2PK(K2), ScriptSig() << Sig().PadS(), EScriptDerSig, SigDer);
  interpret(P2PK(K2), ScriptSig() << Sig().HighR().UnpadR(), EScriptNone, Ok);
  interpret(P2PK(K2), ScriptSig() << Sig().HighR().UnpadR(), EScriptDerSig, SigDer);
  interpret(Bare(Script() << K6 << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K6).PadR().FlipR(), EScriptNone, Ok);
  interpret(Bare(Script() << K6 << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K6).PadR().FlipR(), EScriptDerSig, SigDer);
  interpret(Bare(Script() << K6 << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K6).PadR(), EScriptNone, EvalFalse);
  interpret(Bare(Script() << K6 << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K6).PadR(), EScriptDerSig, SigDer);
  // BIP66 example 1
  interpret(P2PK(K2), ScriptSig() << Sig().HighR().UnpadR(), EScriptNone, Ok);
  interpret(P2PK(K2), ScriptSig() << Sig().HighR().UnpadR(), EScriptDerSig, SigDer);
  // BIP66 example 2
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K2).HighR().UnpadR(), EScriptNone, EvalFalse);
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K2).HighR().UnpadR(), EScriptDerSig, SigDer);
  // BIP66 example 3
  interpret(P2PK(K2), ScriptSig() << 0, EScriptNone, EvalFalse);
  interpret(P2PK(K2), ScriptSig() << 0, EScriptDerSig, EvalFalse);
  // BIP66 example 4
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << 0, EScriptNone, Ok);
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << 0, EScriptDerSig, Ok);
  // Example 4 with a signature that is DER and not empty
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << "300602010102010101"_hex, EScriptDerSig, Ok);
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << 0, EScriptDerSig | EScriptNullFail, Ok);
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << "300602010102010101"_hex, EScriptDerSig | EScriptNullFail,
            SigNullFail);
  // BIP66 example 5
  interpret(P2PK(K2), ScriptSig() << 1, EScriptNone, EvalFalse);
  interpret(P2PK(K2), ScriptSig() << 1, EScriptDerSig, SigDer);
  // BIP66 example 6
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << 1, EScriptNone, Ok);
  interpret(Bare(Script() << K2 << OP_CHECKSIG << OP_NOT), ScriptSig() << 1, EScriptDerSig, SigDer);
  // BIP66 example 7
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << Sig().By(K6),
            EScriptNone, Ok);
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << Sig().By(K6),
            EScriptDerSig, SigDer);
  // BIP66 example 8
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << Sig().By(K6),
            EScriptNone, EvalFalse);
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << Sig().By(K6),
            EScriptDerSig, SigDer);
  // BIP66 example 9
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << 0 << Sig().By(K6).HighR().UnpadR(), EScriptNone, EvalFalse);
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << 0 << Sig().By(K6).HighR().UnpadR(), EScriptDerSig, SigDer);
  // BIP66 example 10
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 0 << 0 << Sig().By(K6).HighR().UnpadR(),
            EScriptNone, Ok);
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 0 << 0 << Sig().By(K6).HighR().UnpadR(),
            EScriptDerSig, SigDer);
  // BIP66 example 11
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << 0, EScriptNone, EvalFalse);
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << 0,
            EScriptDerSig, EvalFalse);
  // BIP66 example 12
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << 0,
            EScriptNone, Ok);
  interpret(Bare(Script() << 2 << K2 << K6 << 2 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 0 << Sig().By(K2).HighR().UnpadR() << 0,
            EScriptDerSig, Ok);
  interpret(P2PK(K6), ScriptSig() << Sig().DoubleType(), EScriptNone, Ok);
  interpret(P2PK(K6), ScriptSig() << Sig().DoubleType(), EScriptDerSig, SigDer);
}

// LOW_S; STRICTENC on hybrid keys and undefined hash types
TEST(Interpreter, LowSAndStrictEnc)
{
  interpret(P2PK(K6), ScriptSig() << Sig().HighS(), EScriptNone, Ok);
  interpret(P2PK(K6), ScriptSig() << Sig().HighS(), EScriptLowS, SigHighS);
  interpret(P2PK(K1h), ScriptSig() << Sig(), EScriptNone, Ok);
  interpret(P2PK(K1h), ScriptSig() << Sig(), EScriptStrictEnc, PubKeyType);
  interpret(Bare(Script() << K1h << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K1), EScriptNone, EvalFalse);
  interpret(Bare(Script() << K1h << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K1), EScriptStrictEnc, PubKeyType);
  interpret(Bare(Script() << K1h << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K1).FlipR(), EScriptNone, Ok);
  interpret(Bare(Script() << K1h << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K1).FlipR(), EScriptStrictEnc, PubKeyType);
  // CHECKMULTISIG takes the keys from the last one: a hybrid key counts only when it is taken
  interpret(Bare(Script() << 1 << K1h << K2 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2), EScriptNone, Ok);
  interpret(Bare(Script() << 1 << K1h << K2 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2), EScriptStrictEnc, Ok);
  interpret(Bare(Script() << 1 << K2 << K1h << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2), EScriptStrictEnc, PubKeyType);
  // 5 is not a hash type
  interpret(P2PK(K2u), ScriptSig() << Sig(5), EScriptNone, Ok);
  interpret(P2PK(K2u), ScriptSig() << Sig(5), EScriptStrictEnc, SigHashtype);
  interpret(Bare(Script() << K2u << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig(5).By(K2).FlipR(), EScriptNone, Ok);
  interpret(Bare(Script() << K2u << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig(5).By(K2).FlipR(), EScriptStrictEnc, SigHashtype);
}

// NULLDUMMY, SIGPUSHONLY and what P2SH asks of a script sig, CLEANSTACK
TEST(Interpreter, NullDummyPushOnlyCleanStack)
{
  interpret(Bare(Script() << 3 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG), ScriptSig() << 1 << Sig().By(K1) << Sig().By(K2) << Sig().By(K6),
            EScriptNone, Ok);
  interpret(Bare(Script() << 3 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG), ScriptSig() << 1 << Sig().By(K1) << Sig().By(K2) << Sig().By(K6),
            EScriptNullDummy, SigNullDummy);
  interpret(Bare(Script() << 3 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 1 << Sig().By(K1) << Sig().By(K2)
              << Sig().By(K6).FlipR(), EScriptNone, Ok);
  interpret(Bare(Script() << 3 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG << OP_NOT), ScriptSig() << 1 << Sig().By(K1) << Sig().By(K2)
              << Sig().By(K6).FlipR(), EScriptNullDummy, SigNullDummy);
  interpret(Bare(Script() << 2 << K2 << K2 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2) << OP_DUP, EScriptNone, Ok);
  interpret(Bare(Script() << 2 << K2 << K2 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2) << OP_DUP, EScriptSigPushOnly, SigPushOnly);
  // A script sig that is not push only: P2SH refuses it where it spends P2SH, SIGPUSHONLY everywhere
  interpret(P2SH(P2PK(K6)), ScriptSig() << Sig() << OP_NOP8 << Redeem(), EScriptNone, Ok);
  interpret(P2PK(K6), ScriptSig() << Sig() << OP_NOP8, EScriptNone, Ok);
  interpret(P2SH(P2PK(K6)), ScriptSig() << Sig() << OP_NOP8 << Redeem(), EScriptP2SH, SigPushOnly);
  interpret(P2SH(P2PK(K6)), ScriptSig() << Sig() << OP_NOP8 << Redeem(), EScriptSigPushOnly, SigPushOnly);
  interpret(Bare(Script() << 2 << K2 << K2 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K2) << Sig().By(K2), EScriptSigPushOnly, Ok);
  interpret(P2PK(K1u), ScriptSig() << 11 << Sig(), EScriptP2SH, Ok);
  interpret(P2PK(K1u), ScriptSig() << 11 << Sig(), EScriptCleanStack | EScriptP2SH, CleanStack);
  interpret(P2SH(P2PK(K1u)), ScriptSig() << 11 << Sig() << Redeem(), EScriptP2SH, Ok);
  interpret(P2SH(P2PK(K1u)), ScriptSig() << 11 << Sig() << Redeem(), EScriptCleanStack | EScriptP2SH, CleanStack);
  interpret(P2SH(P2PK(K1u)), ScriptSig() << Sig() << Redeem(), EScriptCleanStack | EScriptP2SH, Ok);
}

// Witness v0: P2WSH and P2WPKH, bare and in P2SH; the wrong key, the wrong amount, programs that do not fit
TEST(Interpreter, WitnessV0)
{
  interpret(P2WSH(Script() << K1u << OP_CHECKSIG), ScriptSig(), Witness() << Sig().By(K1) << WitnessScript(), EScriptWitness | EScriptP2SH, Ok);
  interpret(P2WPKH(K1u), ScriptSig(), Witness() << Sig() << K1u, EScriptWitness | EScriptP2SH, Ok);
  interpret(P2SH(P2WSH(Script() << K1u << OP_CHECKSIG)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH, Ok);
  interpret(P2SH(P2WPKH(K1u)), ScriptSig() << Redeem(), Witness() << Sig() << K1u, EScriptWitness | EScriptP2SH, Ok);
  interpret(P2WSH(Script() << K2u << OP_CHECKSIG), ScriptSig(), Witness() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(P2WPKH(K2u), ScriptSig(), Witness() << Sig().By(K1) << K2u, EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(P2SH(P2WSH(Script() << K2u << OP_CHECKSIG)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(P2SH(P2WPKH(K2u)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << K2u, EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(P2WSH(Script() << K2u << OP_CHECKSIG), ScriptSig(), Witness() << Sig().By(K1) << WitnessScript(), EScriptP2SH, Ok);
  interpret(P2WPKH(K2u), ScriptSig(), Witness() << Sig().By(K1) << K2u, EScriptP2SH, Ok);
  interpret(P2SH(P2WSH(Script() << K2u << OP_CHECKSIG)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << WitnessScript(), EScriptP2SH, Ok);
  interpret(P2SH(P2WPKH(K2u)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << K2u, EScriptP2SH, Ok);
  interpret(CTx{.Then = SpentAmount(), .Version = 1, .Sequence = 0xffffffff}, P2WSH(Script() << K1u << OP_CHECKSIG), ScriptSig(), Witness()
              << Sig().By(K1) << WitnessScript(), EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(CTx{.Then = SpentAmount(), .Version = 1, .Sequence = 0xffffffff}, P2WPKH(K1u), ScriptSig(), Witness() << Sig() << K1u,
            EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(CTx{.Then = SpentAmount(), .Version = 1, .Sequence = 0xffffffff}, P2SH(P2WSH(Script() << K1u << OP_CHECKSIG)), ScriptSig() << Redeem(),
            Witness() << Sig().By(K1) << WitnessScript(), EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(CTx{.Then = SpentAmount(), .Version = 1, .Sequence = 0xffffffff}, P2SH(P2WPKH(K1u)), ScriptSig() << Redeem(), Witness() << Sig() << K1u,
            EScriptWitness | EScriptP2SH, EvalFalse);
  interpret(WitnessProgram(1, Hash160(K1u)), ScriptSig(), Witness() << Sig().By(K1) << K1u,
            EScriptWitness | EScriptP2SH | EScriptDiscourageUpgradableWitnessProgram, DiscourageUpgradableWitnessProgram);
  interpret(WitnessProgram(0, Bytes(31, 0xb3)), ScriptSig(), Witness() << Sig().By(K1) << K1u,
            EScriptWitness | EScriptP2SH, WitnessProgramWrongLength);
  interpret(P2WSH(Script() << K1u << OP_CHECKSIG), ScriptSig(), Witness(), EScriptWitness | EScriptP2SH, WitnessProgramWitnessEmpty);
  interpret(P2WSH(Script() << K1u << OP_CHECKSIG), ScriptSig(), Witness() << Sig().By(K1) << Code(Script() << K2u << OP_CHECKSIG),
            EScriptWitness | EScriptP2SH, WitnessProgramMismatch);
  interpret(P2WPKH(K1u), ScriptSig(), Witness() << Sig() << K1u << Bytes(), EScriptWitness | EScriptP2SH, WitnessProgramMismatch);
  interpret(P2WPKH(K1u), ScriptSig() << 11, Witness() << Sig() << K1u, EScriptWitness | EScriptP2SH, WitnessMalleated);
  interpret(P2SH(P2WPKH(K2u)), ScriptSig() << 11 << Redeem(), Witness() << Sig().By(K1) << K2u, EScriptWitness | EScriptP2SH, WitnessMalleatedP2SH);
  interpret(P2PK(K1u), ScriptSig() << Sig(), Witness() << Bytes(), EScriptWitness | EScriptP2SH, WitnessUnexpected);
}

// WITNESS_PUBKEYTYPE: compressed keys only, in every key CHECKMULTISIG gets to
TEST(Interpreter, WitnessPubKeyType)
{
  interpret(P2WSH(Script() << K1 << OP_CHECKSIG), ScriptSig(), Witness() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2WPKH(K1), ScriptSig(), Witness() << Sig() << K1, EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2SH(P2WSH(Script() << K1 << OP_CHECKSIG)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2SH(P2WPKH(K1)), ScriptSig() << Redeem(), Witness() << Sig() << K1, EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2WSH(Script() << K1u << OP_CHECKSIG), ScriptSig(), Witness() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2WPKH(K1u), ScriptSig(), Witness() << Sig() << K1u, EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2SH(P2WSH(Script() << K1u << OP_CHECKSIG)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2SH(P2WPKH(K1u)), ScriptSig() << Redeem(), Witness() << Sig() << K1u,
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  // CHECKMULTISIG takes the keys from the last one: a key has to be compressed only when it is taken
  interpret(P2WSH(Script() << 1 << K2 << K1 << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2SH(P2WSH(Script() << 1 << K2 << K1 << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K1)
              << WitnessScript(), EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2WSH(Script() << 1 << K2 << K1 << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K2) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2SH(P2WSH(Script() << 1 << K2 << K1 << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K2)
              << WitnessScript(), EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH, Ok);
  interpret(P2SH(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K1)
              << WitnessScript(), EScriptWitness | EScriptP2SH, Ok);
  interpret(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2SH(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K1)
              << WitnessScript(), EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K2) << WitnessScript(),
            EScriptWitness | EScriptP2SH, Ok);
  interpret(P2SH(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K2)
              << WitnessScript(), EScriptWitness | EScriptP2SH, Ok);
  interpret(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K2) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2SH(P2WSH(Script() << 1 << K2 << K1u << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K2)
              << WitnessScript(), EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH, Ok);
  interpret(P2SH(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K1)
              << WitnessScript(), EScriptWitness | EScriptP2SH, Ok);
  interpret(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K1) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2SH(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K1)
              << WitnessScript(), EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, Ok);
  interpret(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K2) << WitnessScript(),
            EScriptWitness | EScriptP2SH, Ok);
  interpret(P2SH(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K2)
              << WitnessScript(), EScriptWitness | EScriptP2SH, Ok);
  interpret(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG), ScriptSig(), Witness() << Bytes() << Sig().By(K2) << WitnessScript(),
            EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
  interpret(P2SH(P2WSH(Script() << 1 << K2u << K1 << 2 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(), Witness() << Bytes() << Sig().By(K2)
              << WitnessScript(), EScriptWitness | EScriptP2SH | EScriptWitnessPubKeyType, WitnessPubKeyType);
}

// CHECKSEQUENCEVERIFY in a transaction of version 1
TEST(Interpreter, CheckSequenceVerify)
{
  interpret("", "CHECKSEQUENCEVERIFY", EScriptCheckSequenceVerify, InvalidStackOperation);
  interpret("-1", "CHECKSEQUENCEVERIFY", EScriptCheckSequenceVerify, NegativeLocktime);
  // CSV fails if stack top is not minimally encoded
  interpret("<00>", "CHECKSEQUENCEVERIFY", EScriptCheckSequenceVerify | EScriptMinimalData, UnknownError);
  // Bit 31 of the operand is clear: the sequence is checked, and a transaction of version 1 fails it
  interpret("0", "CHECKSEQUENCEVERIFY", EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  interpret("<0000000001>", "CHECKSEQUENCEVERIFY", EScriptCheckSequenceVerify, UnsatisfiedLocktime);
}

// MINIMALIF: segwit scripts only
TEST(Interpreter, MinimalIf)
{
  // MINIMALIF is not applied to non-segwit scripts
  interpret("1", "IF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret("2", "IF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret("<0100>", "IF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret("0", "IF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret("<00>", "IF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret("1", "NOTIF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret("2", "NOTIF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret("<0100>", "NOTIF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret("0", "NOTIF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret("<00>", "NOTIF 1 ENDIF", EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);

  interpret(P2SH(Script("IF 1 ENDIF")), ScriptSig("1") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2SH(Script("IF 1 ENDIF")), ScriptSig("2") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2SH(Script("IF 1 ENDIF")), ScriptSig("<0100>") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2SH(Script("IF 1 ENDIF")), ScriptSig("0") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret(P2SH(Script("IF 1 ENDIF")), ScriptSig("<00>") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret(P2SH(Script("IF 1 ENDIF")), ScriptSig() << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, UnbalancedConditional);

  interpret(P2SH(Script("NOTIF 1 ENDIF")), ScriptSig("1") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret(P2SH(Script("NOTIF 1 ENDIF")), ScriptSig("2") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret(P2SH(Script("NOTIF 1 ENDIF")), ScriptSig("<0100>") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, EvalFalse);
  interpret(P2SH(Script("NOTIF 1 ENDIF")), ScriptSig("0") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2SH(Script("NOTIF 1 ENDIF")), ScriptSig("<00>") << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2SH(Script("NOTIF 1 ENDIF")), ScriptSig() << Redeem(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, UnbalancedConditional);

  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "01"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "02"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "0100"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << Bytes() << WitnessScript(), EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "00"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "01"_hex << WitnessScript(), EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "02"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "0100"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << Bytes() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, CleanStack);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << "00"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << WitnessScript(), EScriptP2SH | EScriptWitness, UnbalancedConditional);
  interpret(P2WSH(Script("IF 1 ENDIF")), ScriptSig(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, UnbalancedConditional);

  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "01"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "02"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "0100"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << Bytes() << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "00"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "01"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, CleanStack);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "02"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "0100"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << Bytes() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << "00"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << WitnessScript(), EScriptP2SH | EScriptWitness, UnbalancedConditional);
  interpret(P2WSH(Script("NOTIF 1 ENDIF")), ScriptSig(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, UnbalancedConditional);

  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "01"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "02"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "0100"_hex << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << Bytes() << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "00"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "01"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "02"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "0100"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << Bytes() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, CleanStack);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "00"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness, UnbalancedConditional);
  interpret(P2SH(P2WSH(Script("IF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, UnbalancedConditional);

  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "01"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "02"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "0100"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << Bytes() << WitnessScript(), EScriptP2SH | EScriptWitness, Ok);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "00"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness, Ok);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "01"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, CleanStack);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "02"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "0100"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << Bytes() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, Ok);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << "00"_hex << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, MinimalIf);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness, UnbalancedConditional);
  interpret(P2SH(P2WSH(Script("NOTIF 1 ENDIF"))), ScriptSig() << Redeem(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness | EScriptMinimalIf, UnbalancedConditional);
}

// NULLFAIL covers every signature, and signatures only
TEST(Interpreter, NullFail)
{
  // NULLFAIL should cover all signatures and signatures only
  interpret("0*21", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig, Ok);
  interpret("0*21", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig | EScriptNullFail, Ok);
  interpret("1 0*20", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig | EScriptNullFail, Ok);
  interpret("1 0*20", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig | EScriptNullFail | EScriptNullDummy, SigNullDummy);
  // A signature that is DER and fails, wherever it stands among the empty ones
  interpret("0*20 <300602010102010101>", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig, Ok);
  interpret("0*20 <300602010102010101>", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig | EScriptNullFail, SigNullFail);
  interpret("0 <300602010102010101> 0*19", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig, Ok);
  interpret("0 <300602010102010101> 0*19", "20 1*20 20 CHECKMULTISIG NOT", EScriptDerSig | EScriptNullFail, SigNullFail);
}

// Core's script_PushData: every form of a push leaves the same value, a push cut short is a bad opcode
TEST(Interpreter, PushData)
{
  const char *same = "<5a> EQUAL";
  interpret("<5a>", same, EScriptP2SH, Ok);
  interpret("PUSHDATA1<5a>", same, EScriptP2SH, Ok);
  interpret("PUSHDATA2<5a>", same, EScriptP2SH, Ok);
  interpret("PUSHDATA4<5a>", same, EScriptP2SH, Ok);
  // The length of one byte and no byte after it
  interpret("PUSHDATA1 0x01", same, EScriptP2SH, BadOpcode);
  interpret("PUSHDATA2 0x0100", same, EScriptP2SH, BadOpcode);
  interpret("PUSHDATA4 0x01000000", same, EScriptP2SH, BadOpcode);
}

// Core's script_cltv_truncated
TEST(Interpreter, CltvWithoutOperand)
{
  interpret("", "CHECKLOCKTIMEVERIFY", EScriptCheckLockTimeVerify, InvalidStackOperation);
}

// Core's script_CHECKMULTISIG12 and script_CHECKMULTISIG23, under P2SH and STRICTENC as there
TEST(Interpreter, CheckMultisig12And23)
{
  const uint32_t flags = EScriptP2SH | EScriptStrictEnc;
  const CKey other{4};

  const COutput oneOfTwo = Bare(Script() << 1 << K1 << K2u << 2 << OP_CHECKMULTISIG);
  interpret(oneOfTwo, ScriptSig() << 0 << Sig().By(K1), flags, Ok);
  // The transaction changed after it is signed
  interpret(CTx{.Then = OutputValue(0), .Version = 1, .Sequence = 0xffffffff}, oneOfTwo, ScriptSig() << 0 << Sig().By(K1), Witness(), flags,
            EvalFalse);
  interpret(oneOfTwo, ScriptSig() << 0 << Sig().By(K2), flags, Ok);
  interpret(oneOfTwo, ScriptSig() << 0 << Sig().By(K6), flags, EvalFalse);

  const COutput twoOfThree = Bare(Script() << 2 << K1 << K2u << K6 << 3 << OP_CHECKMULTISIG);
  auto signedBy = [](const CKey &first, const CKey &second) { return ScriptSig() << 0 << Sig().By(first) << Sig().By(second); };
  interpret(twoOfThree, signedBy(K1, K2), flags, Ok);
  interpret(twoOfThree, signedBy(K1, K6), flags, Ok);
  interpret(twoOfThree, signedBy(K2, K6), flags, Ok);
  // Can't reuse sig
  interpret(twoOfThree, signedBy(K2, K2), flags, EvalFalse);
  // sigs must be in correct order
  interpret(twoOfThree, signedBy(K2, K1), flags, EvalFalse);
  interpret(twoOfThree, signedBy(K6, K2), flags, EvalFalse);
  // sigs must match pubkeys
  interpret(twoOfThree, signedBy(other, K2), flags, EvalFalse);
  interpret(twoOfThree, signedBy(K1, other), flags, EvalFalse);
  // Must have signatures
  interpret(twoOfThree, ScriptSig() << 0, flags, InvalidStackOperation);
}

// Core's script_standard_push: the pushes CScript's << writes, of numbers and of data of every size, are push only
// and minimal
TEST(Interpreter, StandardPush)
{
  const COutput one = Bare(Script("1"));
  for (int number = 0; number < 67000; number++)
    interpret(one, ScriptSig() << number, EScriptMinimalData | EScriptSigPushOnly, Ok);
  for (size_t size = 0; size <= 520; size++) {
    const Bytes data(size, 0x49);
    interpret(one, ScriptSig() << data, EScriptMinimalData | EScriptSigPushOnly, Ok);
  }
}

// Core's script_IsPushOnly_on_invalid_scripts: a push cut short is not push only
TEST(Interpreter, PushOnlyOfPushCutShort)
{
  interpret("0x01", "1", EScriptSigPushOnly, SigPushOnly);
}

// Core's multisig_verify, under P2SH and STRICTENC as there: a AND b, a OR b, an escrow of 2 of 3; the fourth key
// is in no script
TEST(Interpreter, MultisigVerify)
{
  const uint32_t flags = EScriptP2SH | EScriptStrictEnc;
  const CKey keys[4] = {K1, K2, K6, CKey{4}};
  const COutput both = Bare(Script() << 2 << K1 << K2 << 2 << OP_CHECKMULTISIG);
  const COutput either = Bare(Script() << 1 << K1 << K2 << 2 << OP_CHECKMULTISIG);
  const COutput escrow = Bare(Script() << 2 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG);

  interpret(both, ScriptSig() << 0 << Sig().By(K1) << Sig().By(K2), flags, Ok);
  for (const CKey &key: keys) {
    interpret(both, ScriptSig() << 0 << Sig().By(key), flags, InvalidStackOperation);
    interpret(both, ScriptSig() << 0 << Sig().By(K2) << Sig().By(key), flags, EvalFalse);
  }

  for (size_t i = 0; i < 4; i++)
    interpret(either, ScriptSig() << 0 << Sig().By(keys[i]), flags, i < 2 ? Ok : EvalFalse);
  interpret(either, ScriptSig() << 0 << 1, flags, SigDer);

  for (size_t i = 0; i < 4; i++) {
    for (size_t j = 0; j < 4; j++)
      interpret(escrow, ScriptSig() << 0 << Sig().By(keys[i]) << Sig().By(keys[j]), flags, i < j && j < 3 ? Ok : EvalFalse);
  }
}

// Core's script_p2sh_tests: norecurse, switchover, is
TEST(Interpreter, P2sh)
{
  // Only the outer pay-to-script-hash runs the script hashed: a bad opcode there, and P2SH of that P2SH, whose
  // HASH160 <> EQUAL only checks the hash
  const CScriptSig invalid = Script("0xff 0xff");
  interpret(P2SH(invalid), ScriptSig() << Redeem(), EScriptP2SH, BadOpcode);
  interpret(P2SH(P2SH(invalid)), ScriptSig() << Code(invalid) << Redeem(), EScriptP2SH, Ok);

  // A script that fails: valid under the old rules, the hash is right; not under the new
  const CScriptSig notValid = Script("11 12 EQUALVERIFY");
  const COutput fund = P2SH(notValid);
  interpret(fund, ScriptSig() << Redeem(), EScriptNone, Ok);
  interpret(fund, ScriptSig() << Redeem(), EScriptP2SH, EqualVerify);

  // Not pay-to-script-hash if the hash is pushed with one of the OP_PUSHDATA opcodes: the script hashed does not run
  const Bytes hash(fund.Script.begin() + 2, fund.Script.begin() + 22);
  interpret(Bare(Script() << OP_HASH160 << Pushdata1(hash) << OP_EQUAL), ScriptSig() << Code(notValid), EScriptP2SH, Ok);
  interpret(Bare(Script() << OP_HASH160 << Pushdata2(hash) << OP_EQUAL), ScriptSig() << Code(notValid), EScriptP2SH, Ok);
  interpret(Bare(Script() << OP_HASH160 << Pushdata4(hash) << OP_EQUAL), ScriptSig() << Code(notValid), EScriptP2SH, Ok);
}

// Core's script_p2sh_tests sign: pay to a key and to a key hash, of different keys, bare and under P2SH. An output
// takes its own script sig and none of the others'
TEST(Interpreter, ScriptSigOfAnotherOutput)
{
  const COutput scripts[4] = {P2PK(K1), P2PKH(K2), P2PK(K2), P2PKH(K6)};
  const CKey keys[4] = {K1, K2, K2, K6};
  COutput outputs[8];
  CScriptSig scriptSigs[8];
  for (size_t i = 0; i < 4; i++) {
    const CSig sig = Sig().By(keys[i]).Over(scripts[i].Script);
    outputs[i] = P2SH(scripts[i]);
    outputs[i + 4] = scripts[i];
    scriptSigs[i + 4] = i % 2 ? ScriptSig() << sig << keys[i] : ScriptSig() << sig;
    scriptSigs[i] = scriptSigs[i + 4] << scripts[i].Script;
  }

  for (size_t i = 0; i < 8; i++) {
    for (size_t j = 0; j < 8; j++) {
      // Another's: P2SH does not find its script on top, nor a key hash its key; a key gets the signature of
      // another key, or a key or a script where the signature goes, and that is not DER
      const BTC::EScriptError another = i < 4 ? EvalFalse : i % 2 ? EqualVerify : j == 4 || j == 6 ? EvalFalse : SigDer;
      interpret(outputs[i], scriptSigs[j], EScriptP2SH | EScriptStrictEnc, i == j ? Ok : another);
    }
  }
}

// Core's test_witness: a spend of every form of output under the flags of a block before P2SH, before segwit, with
// segwit, and under the standard ones; a spend made for another key's output; a multisig one of the two has signed
TEST(Interpreter, OutputForms)
{
  const uint32_t segwit = EScriptP2SH | EScriptWitness;
  const uint32_t sets[] = {EScriptNone, EScriptP2SH, segwit, StandardFlags};

  // Pay to a key and its P2SH, the key compressed and not. The other key's signature fails CHECKSIG; before P2SH
  // the redeem script only has to hash right
  for (const auto &[key, other]: {std::pair{K1, K2}, std::pair{K1u, K2u}}) {
    for (uint32_t flags: sets) {
      const BTC::EScriptError wrongKey = flags == StandardFlags ? SigNullFail : EvalFalse;
      interpret(P2PK(key), ScriptSig() << Sig(), flags, Ok);
      interpret(P2PK(key), ScriptSig() << Sig().By(other), flags, wrongKey);
      interpret(P2SH(P2PK(key)), ScriptSig() << Sig() << Redeem(), flags, Ok);
      interpret(P2SH(P2PK(key)), ScriptSig() << Sig().By(other) << Redeem(), flags, flags == EScriptNone ? Ok : wrongKey);
    }
  }

  // Witness pay to a key hash and its P2SH. The other key's witness: nothing looks at it before segwit, then its key
  // does not hash to the program
  for (uint32_t flags: sets) {
    const BTC::EScriptError otherKey = flags & EScriptWitness ? EqualVerify : Ok;
    interpret(P2WPKH(K1), ScriptSig(), Witness() << Sig() << K1, flags, Ok);
    interpret(P2WPKH(K1), ScriptSig(), Witness() << Sig().By(K2) << K2, flags, otherKey);
    interpret(P2SH(P2WPKH(K1)), ScriptSig() << Redeem(), Witness() << Sig() << K1, flags, Ok);
    interpret(P2SH(P2WPKH(K1)), ScriptSig() << Redeem(), Witness() << Sig().By(K2) << K2, flags, otherKey);
  }

  // 2 of 2, bare and in the three wrappings. One signer leaves nothing where the other signs: the script fails
  // where it runs, and it does not run before the soft fork of its wrapping
  const CScriptSig multi = Script() << 2 << K1 << K6 << 2 << OP_CHECKMULTISIG;
  for (const CKey &signer: {K1, K6}) {
    const CScriptSig partial = ScriptSig() << 0 << Sig().By(signer) << 0;
    const CWitness partialWitness = Witness() << Bytes{} << Sig().By(signer) << Bytes{} << WitnessScript();
    interpret(Bare(multi), partial, EScriptNone, EvalFalse);
    interpret(P2SH(multi), partial << Redeem(), EScriptNone, Ok);
    interpret(P2SH(multi), partial << Redeem(), EScriptP2SH, EvalFalse);
    interpret(P2WSH(multi), ScriptSig(), partialWitness, EScriptNone, Ok);
    interpret(P2WSH(multi), ScriptSig(), partialWitness, segwit, EvalFalse);
    interpret(P2SH(P2WSH(multi)), ScriptSig() << Redeem(), partialWitness, EScriptP2SH, Ok);
    interpret(P2SH(P2WSH(multi)), ScriptSig() << Redeem(), partialWitness, segwit, EvalFalse);
  }

  // The two signatures put together
  const CScriptSig both = ScriptSig() << 0 << Sig().By(K1) << Sig().By(K6);
  const CWitness bothWitness = Witness() << Bytes{} << Sig().By(K1) << Sig().By(K6) << WitnessScript();
  interpret(Bare(multi), both, StandardFlags, Ok);
  interpret(P2SH(multi), both << Redeem(), EScriptP2SH, Ok);
  interpret(P2SH(multi), both << Redeem(), StandardFlags, Ok);
  interpret(P2WSH(multi), ScriptSig(), bothWitness, segwit, Ok);
  interpret(P2WSH(multi), ScriptSig(), bothWitness, StandardFlags, Ok);
  interpret(P2SH(P2WSH(multi)), ScriptSig() << Redeem(), bothWitness, segwit, Ok);
  interpret(P2SH(P2WSH(multi)), ScriptSig() << Redeem(), bothWitness, StandardFlags, Ok);
}
