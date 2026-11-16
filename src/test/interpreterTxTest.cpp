// The interpreter against Core's tx_valid and tx_invalid: the scripts and the transaction around them
// are Core's, the keys and signatures the test keys'. A vector of tx_valid is valid under every flag
// but those it names, and fails once any of them is back; a vector of tx_invalid fails under the flags
// it names and is valid once any of them is away (spendBuilder.h). The error of an invalid one is
// what Core's VerifyScript gives. A vector of several inputs is a line to each input that signs.
// After the vectors, the spends Core's transaction_tests and txvalidationcache_tests make themselves

#include "spendBuilder.h"

using namespace SpendTest;
using enum BTC::EScriptError;
using enum BTC::EScriptFlag;

namespace {

const CKey K3{3}, K4{4}, K5{5};
constexpr COutput::EDigest Legacy = COutput::EDigest::Legacy;
constexpr COutput::EDigest Bip143 = COutput::EDigest::Bip143;
constexpr uint8_t ACP = SIGHASH_ANYONECANPAY;

// Core's vectors are of version 1 with final sequences, unless they test either
CTx tx1(size_t inputs = 1, size_t outputs = 1, size_t checked = 0)
{
  return CTx{.Inputs = inputs, .Outputs = outputs, .Checked = checked, .Version = 1, .Sequence = 0xffffffff};
}

// Changed after it is signed
CTx tx1(size_t inputs, size_t outputs, size_t checked, const CChange &then)
{
  return CTx{.Inputs = inputs, .Outputs = outputs, .Checked = checked, .Then = then, .Version = 1, .Sequence = 0xffffffff};
}

}

// Transactions of the chain and their kin
TEST(InterpreterTx, History)
{
  // 23b397edccd3740a74adb603c9756370fafcde9bcc4483eb271ecad09a94dd63, the first OP_CHECKMULTISIG in standard form.
  // Its signature is not DER, S is negative, and OpenSSL took it
  const COutput oneOfTwo = Bare(Script() << 1 << K1u << K2u << 2 << OP_CHECKMULTISIG);
  const CSig negativeS = Sig().By(K1).HighS().UnpadS();
  const uint32_t encoding = EScriptDerSig | EScriptLowS | EScriptStrictEnc;
  txValid(tx1(), oneOfTwo, ScriptSig() << 0 << negativeS, encoding);
  // A byte more ahead of the hash type
  txValid(tx1(), oneOfTwo, ScriptSig() << 0 << negativeS.DoubleType(), encoding);
  // The dummy other than an empty string: a byte, OP_1, OP_1NEGATE
  txValid(tx1(), oneOfTwo, ScriptSig() << "ff"_hex << negativeS, encoding | EScriptNullDummy);
  txValid(tx1(), oneOfTwo, ScriptSig() << 1 << negativeS, encoding | EScriptNullDummy);
  txValid(tx1(), oneOfTwo, ScriptSig() << -1 << negativeS, encoding | EScriptNullDummy);
  txInvalid(tx1(), oneOfTwo, ScriptSig() << "00"_hex << negativeS, EScriptNullDummy, SigNullDummy);
  txInvalid(tx1(), oneOfTwo, ScriptSig() << 1 << negativeS, EScriptNullDummy, SigNullDummy);
  txInvalid(tx1(), oneOfTwo, ScriptSig() << -1 << negativeS, EScriptNullDummy, SigNullDummy);
  // The dummy missing
  txInvalid(tx1(), oneOfTwo, ScriptSig() << negativeS, EScriptNone, InvalidStackOperation);

  // c99c49da4c38af669dea436d3e73780dfdb6c1ecf9958baa52960e8baee30e73: a 0-sequence and a signature of the hash type 0
  txValid(CTx{.Version = 1, .Sequence = 0}, P2PKH(K1u), ScriptSig() << Sig(0x00).HighS() << K1u, EScriptLowS | EScriptStrictEnc);

  // A nearly-standard transaction with CHECKSIGVERIFY 1 instead of CHECKSIG
  txValid(tx1(), Bare(Script() << OP_DUP << OP_HASH160 << Hash160(K1) << OP_EQUALVERIFY << OP_CHECKSIGVERIFY << 1),
          ScriptSig() << Sig().By(K1) << K1, EScriptNone);
  // 0e1b5688cf179cd9f7cbda1fac0090f6e684bbf8cd946660120197c3f3681809 with junk after the script the signature is of
  txInvalid(tx1(), Bare(Script() << K1u << OP_CHECKSIG << OP_VERIFY << 1),
            ScriptSig() << Sig().By(K1).HighS().Over(Code(Script() << K1u << OP_CHECKSIG)), EScriptNone, Verify);

  // f7fdd091fa6d8f5e7a8c2458f5c38faffff2d3f1406b6e4fe2c99dcc0d2d1cbb: an integer of a signature negative, valid
  // before BIP66. R of its first input, S of the second
  txValid(tx1(2, 1, 0), P2PKH(K1u), ScriptSig() << Sig().HighR().UnpadR() << K1u, encoding);
  txValid(tx1(2, 1, 1), P2PKH(K1u), ScriptSig() << Sig().HighS().UnpadS() << K1u, encoding);
  // A signature that is not DER: a byte more ahead of the hash type
  txValid(tx1(), P2PKH(K1), ScriptSig() << Sig().DoubleType() << K1, encoding);
  txInvalid(tx1(), P2PKH(K1), ScriptSig() << Sig().DoubleType() << K1, EScriptDerSig, SigDer);

  // A valid P2SH transaction of the standard type BIP 16 puts forth, and one that is not
  txValid(tx1(), P2SH(P2PK(K1)), ScriptSig() << Sig().HighS() << Redeem(), EScriptLowS);
  // The script hashed is the text "What is ", these opcodes when it runs
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, P2SH(Script("7 ENDIF NOP DEPTH 0x20697320")), ScriptSig() << Redeem(),
            EScriptP2SH, UnbalancedConditional);
  // The size of a coinbase's script is not for the scripts to check
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("1")), ScriptSig("1 1"), EScriptCleanStack);
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("1")), ScriptSig("1*100"), EScriptCleanStack);

  // ddc454a1c0c35c188c98976b17670f69e586d9c0f3593ea879928332f0a069e7 spends an output that pushes with a PUSHDATA1
  // whose length is negative when read as signed
  txValid(tx1(), Bare(Script() << Bytes(174, 0x60) << OP_DROP << OP_DUP << OP_HASH160 << Hash160(K1u) << OP_EQUALVERIFY << OP_CHECKSIG),
          ScriptSig() << Sig().By(K1) << K1u, EScriptNone);

  // The order of signatures: correct, and not
  const COutput twoOfThree = P2SH(Script() << 2 << K1 << K2 << K6 << 3 << OP_CHECKMULTISIG);
  txValid(tx1(), twoOfThree, ScriptSig() << 0 << Sig().By(K1) << Sig().By(K2).HighS() << Redeem(), EScriptLowS);
  txInvalid(tx1(), twoOfThree, ScriptSig() << 0 << Sig().By(K2).HighS() << Sig().By(K1) << Redeem(), EScriptP2SH, EvalFalse);

  // cc60b1f899ec0a69b7c3f25ddf32c4524096a9c5b01cbd84c6d0312a0c478984 relies on OP_CHECKSIG leaving 0 for a
  // signature of no bytes
  txValid(tx1(), Bare(Script() << K1 << OP_CHECKSIG << OP_SWAP << K2 << OP_CHECKSIG << OP_SWAP << 3 << OP_PICK << OP_SHA256
            << "d68df9e32a147cffa36193c6f7c43a1c8c69cda530e1c6db354bfabdcfefaf3c"_hex << OP_EQUAL << 3 << OP_PICK << OP_SHA256
            << "f531f3041d3136701ea09067c53e7159c8f9b2746a56c3d82966c54bbc553226"_hex << OP_EQUAL << OP_BOOLAND << 4 << OP_PICK << OP_SIZE
            << OP_NIP << "20"_hex << "22"_hex << OP_WITHIN << OP_BOOLAND << 3 << OP_PICK << OP_SIZE << OP_NIP << "20"_hex << "22"_hex
            << OP_WITHIN << OP_BOOLAND << OP_IF << 3 << OP_PICK << OP_SIZE << OP_NIP << 3 << OP_PICK << OP_SIZE << OP_NIP << OP_EQUAL
            << OP_PICK << OP_ELSE << OP_BOOLAND << OP_ENDIF),
          ScriptSig() << "ca42095840735e89283fec298e62ac2ddea9b5f34a8cbb7097ad965b87568100"_hex
            << "1b1b01dc829177da4a14551d2fc96a9db00c6501edfa12f22cd9cefd335c227f"_hex << Sig().By(K2) << 0, EScriptCleanStack);
}

// Keys and signatures of no bytes
TEST(InterpreterTx, EmptyKeysAndSignatures)
{
  // Empty pubkey
  txValid(tx1(), Bare(Script() << Bytes() << OP_CHECKSIG << OP_NOT), ScriptSig() << Sig().By(K1), EScriptStrictEnc | EScriptNullFail);
  // Empty signature
  txValid(tx1(), Bare(Script() << K1 << OP_CHECKSIG << OP_NOT), ScriptSig() << 0, EScriptNone);
  // Empty stack when we try to run CHECKSIG
  txInvalid(tx1(), Bare(Script() << K1 << OP_CHECKSIG << OP_NOT), ScriptSig(), EScriptNone, InvalidStackOperation);
  // An empty key CHECKMULTISIG does not get to, and one it does
  txValid(tx1(), Bare(Script() << 1 << Bytes() << K1 << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K1), EScriptNone);
  txValid(tx1(), Bare(Script() << 1 << K1 << Bytes() << 2 << OP_CHECKMULTISIG), ScriptSig() << 0 << Sig().By(K1), EScriptStrictEnc);
  // A signature of a CHECKMULTISIG that fails, the other empty
  const COutput notTwoOfTwo = Bare(Script() << 2 << K1 << K2 << 2 << OP_CHECKMULTISIG << OP_NOT);
  txValid(tx1(), notTwoOfTwo, ScriptSig() << 0 << Sig().By(K1) << 0, EScriptNullFail);
  txValid(tx1(), notTwoOfTwo, ScriptSig() << 0 << 0 << Sig().By(K2), EScriptNullFail);
}

// The hash types of a signature outside segwit
TEST(InterpreterTx, SighashTypes)
{
  // SIGHASH_SINGLE of an input with no output of its index signs the constant 1, a bug the chain keeps
  txValid(tx1(2, 1, 1), P2PKH(K1), ScriptSig() << Sig(SIGHASH_SINGLE) << K1, EScriptNone);
  txValid(CTx{.Inputs = 2, .Version = 1, .Sequence = 0xffffffff}, Bare(Script("1")), ScriptSig("1"), EScriptCleanStack);
  // SIGHASH_SINGLE | SIGHASH_ANYONECANPAY inputs, the last with no output of its index
  for (size_t i = 0; i < 3; i++)
    txValid(tx1(3, 2, i), P2PKH(K1), ScriptSig() << Sig(SIGHASH_SINGLE | ACP) << K1, EScriptNone);
  // afd9c17f8913577ec3509520bd6e5d63e9c0fd2a5f70c787993b097ba6ca9fae has several SIGHASH_SINGLE signatures
  txValid(tx1(3, 3, 0), P2PKH(K1), ScriptSig() << Sig(SIGHASH_SINGLE) << K1, EScriptNone);
  txValid(tx1(3, 3, 1), P2PKH(K1), ScriptSig() << Sig(SIGHASH_SINGLE) << K1, EScriptNone);
  txValid(tx1(3, 3, 2), P2PKH(K1), ScriptSig() << Sig(SIGHASH_SINGLE).HighS() << K1, EScriptLowS);

  // The first input signed with SIGHASH_ALL, the second with SIGHASH_ANYONECANPAY
  txValid(tx1(2, 1, 0), P2PK(K1), ScriptSig() << Sig(), EScriptNone);
  txValid(tx1(2, 1, 1), P2PK(K1), ScriptSig() << Sig(SIGHASH_ALL | ACP), EScriptNone);
  // The sequence of the first input changed: the second's signature holds, SIGHASH_ANYONECANPAY is followed
  txValid(CTx{.Inputs = 2, .Version = 1, .Sequence = 1, .OtherSequence = 0xffffffff}, P2PK(K1), ScriptSig() << Sig().HighS(), EScriptLowS);
  txValid(tx1(2, 1, 1, OtherSequence()), P2PK(K1), ScriptSig() << Sig(SIGHASH_ALL | ACP), EScriptNone);
  // The sequence of the SIGHASH_ANYONECANPAY input changed: the SIGHASH_ALL signature does not hold
  txInvalid(CTx{.Inputs = 2, .Then = OtherSequence(), .Version = 1, .Sequence = 1}, P2PK(K1), ScriptSig() << Sig().HighS(), EScriptNone,
            EvalFalse);

  // Outside segwit SIGHASH_SINGLE | SIGHASH_ANYONECANPAY hashes the position of the input: the inputs and their
  // outputs permuted, the signatures do not hold
  for (size_t i = 0; i < 2; i++) {
    txValid(tx1(2, 2, i), P2PK(K1), ScriptSig() << Sig(SIGHASH_SINGLE | ACP), EScriptNone);
    txInvalid(tx1(2, 2, i, Permute()), P2PK(K1), ScriptSig() << Sig(SIGHASH_SINGLE | ACP), EScriptNone, EvalFalse);
  }
}

// OP_CODESEPARATOR outside segwit, and CONST_SCRIPTCODE that forbids it
TEST(InterpreterTx, CodeSeparator)
{
  const uint32_t excluded = EScriptConstScriptCode | EScriptLowS;
  // SignatureHash() removes OP_CODESEPARATOR
  for (const CScriptSig &script: {Script() << OP_CODESEPARATOR << K1 << OP_CHECKSIG,
                                  Script() << OP_CODESEPARATOR << OP_CODESEPARATOR << K1 << OP_CHECKSIG}) {
    txValid(tx1(), Bare(script), ScriptSig() << Sig().By(K1).HighS(), excluded);
    txInvalid(tx1(), Bare(script), ScriptSig() << Sig().By(K1).HighS(), EScriptConstScriptCode, OpCodeSeparator);
  }

  // Hashed data starts at the CODESEPARATOR
  const COutput after = Bare(Script() << K1 << OP_CODESEPARATOR << OP_CHECKSIG);
  const CSig ofCheckSig = Sig().By(K1).HighS().Over(Code(Script() << OP_CHECKSIG));
  txValid(tx1(), after, ScriptSig() << ofCheckSig, excluded);
  txInvalid(tx1(), after, ScriptSig() << ofCheckSig, EScriptConstScriptCode, OpCodeSeparator);

  // But only if execution has reached it: the first CHECKSIGVERIFY takes the whole script
  const COutput twice = Bare(Script() << K1 << OP_CHECKSIGVERIFY << OP_CODESEPARATOR << K1 << OP_CHECKSIGVERIFY << OP_CODESEPARATOR << 1);
  const CSig ofSecond = Sig().By(K1).Over(Code(Script() << K1 << OP_CHECKSIGVERIFY << OP_CODESEPARATOR << 1));
  txValid(tx1(), twice, ScriptSig() << ofSecond << Sig().By(K1).HighS(), excluded);
  txInvalid(tx1(), twice, ScriptSig() << ofSecond << Sig().By(K1).HighS(), EScriptConstScriptCode, OpCodeSeparator);

  // CODESEPARATOR in an IF block: not run, it does not change what is hashed; run, it does. Each signature with the
  // other branch fails
  const COutput inIf = Bare(Script() << OP_IF << OP_CODESEPARATOR << OP_ENDIF << K1 << OP_CHECKSIGVERIFY << OP_CODESEPARATOR << 1);
  const CSig skipped = Sig().By(K1).HighS();
  const CSig run = Sig().By(K1).Over(Code(Script() << OP_ENDIF << K1 << OP_CHECKSIGVERIFY << OP_CODESEPARATOR << 1));
  txValid(tx1(), inIf, ScriptSig() << skipped << 0, excluded);
  txValid(tx1(), inIf, ScriptSig() << run << 1, EScriptConstScriptCode);
  txInvalid(tx1(), inIf, ScriptSig() << skipped << 1, EScriptNone, CheckSigVerify);
  txInvalid(tx1(), inIf, ScriptSig() << run << 0, EScriptNone, CheckSigVerify);
  // Under CONST_SCRIPTCODE it is invalid in the block not run too
  txInvalid(tx1(), inIf, ScriptSig() << skipped << 0, EScriptConstScriptCode, OpCodeSeparator);
  txInvalid(tx1(), inIf, ScriptSig() << run << 1, EScriptConstScriptCode, OpCodeSeparator);
}

// CHECKSIG in a script sig: the script code is the script sig, the signature checked taken out of it
TEST(InterpreterTx, CheckSigInScriptSig)
{
  const COutput output = P2PKH(K1);
  // The signatures the output checks
  const Bytes outer = Signature(Sig().By(K1), tx1(), Legacy, output.Script);
  const Bytes outerHigh = Signature(Sig().By(K1).HighS(), tx1(), Legacy, output.Script);
  const uint32_t excluded = EScriptSigPushOnly | EScriptConstScriptCode | EScriptCleanStack;

  // CHECKSIG is legal in scriptSigs
  {
    const CSig inner = Sig().HighS().Over(Code(Script() << OP_NOP << K1 << OP_CHECKSIG << outer << K1));
    const CScriptSig scriptSig = ScriptSig() << OP_NOP << inner << K1 << OP_CHECKSIG << outer << K1;
    txValid(tx1(), output, scriptSig, excluded | EScriptLowS);
    txInvalid(tx1(), output, scriptSig, EScriptConstScriptCode, SigFindAndDelete);
  }
  // Same semantics for OP_CODESEPARATOR
  {
    const CSig inner = Sig().HighS().Over(Code(Script() << OP_NOP << K1 << OP_CHECKSIG << outerHigh << OP_CODESEPARATOR << K1));
    const CScriptSig scriptSig = ScriptSig("'deadbeef' DROP CODESEPARATOR NOP") << inner << K1 << OP_CHECKSIG << outerHigh
                                                                                << OP_CODESEPARATOR << K1;
    txValid(tx1(), output, scriptSig, excluded | EScriptLowS);
    txInvalid(tx1(), output, scriptSig, EScriptConstScriptCode, OpCodeSeparator);
  }
  // Signatures are removed from the script they are in by FindAndDelete() in the CHECKSIG code; even multiple
  // instances of one signature can be removed
  {
    const CSig inner = Sig().Over(Code(Script() << K1 << OP_CHECKSIG << outer << K1));
    const CScriptSig scriptSig = ScriptSig() << inner << inner << K1 << OP_CHECKSIG << outer << K1;
    txValid(tx1(), output, scriptSig, excluded);
    txInvalid(tx1(), output, scriptSig, EScriptConstScriptCode, SigFindAndDelete);
  }
  // That also includes ahead of the opcode being executed
  {
    const CSig inner = Sig().HighS().Over(Code(Script() << K1 << OP_DROP << OP_CHECKSIG << outerHigh << OP_DROP << K1));
    const CScriptSig scriptSig = ScriptSig() << inner << inner << K1 << inner << OP_DROP << OP_CHECKSIG << outerHigh << inner << OP_DROP << K1;
    txValid(tx1(), output, scriptSig, excluded | EScriptLowS);
    txInvalid(tx1(), output, scriptSig, EScriptConstScriptCode, SigFindAndDelete);
  }
}

// FindAndDelete: the push of a signature checked is taken out of the script code, outside segwit only
TEST(InterpreterTx, FindAndDelete)
{
  // The nearly-standard transaction of History with the signature duplicated in the scriptPubKey
  const CScriptSig verifyOne = Script() << OP_DUP << OP_HASH160 << Hash160(K1) << OP_EQUALVERIFY << OP_CHECKSIGVERIFY << 1;
  const Bytes sig = Signature(Sig().By(K1), tx1(), Legacy, Code(verifyOne));
  // With the proper pushdata prefix
  txValid(tx1(), Bare(Script() << verifyOne << sig), ScriptSig() << sig << K1, EScriptCleanStack | EScriptConstScriptCode);
  // With a non-standard one: FindAndDelete removes only the push the standard way
  txInvalid(tx1(), Bare(Script() << verifyOne << Pushdata1(sig)), ScriptSig() << sig << K1, EScriptNone, CheckSigVerify);
  // The same, the signature in the script sig pushed with the same OP_PUSHDATA
  txInvalid(tx1(), Bare(Script() << verifyOne << Pushdata1(sig)), ScriptSig() << Pushdata1(sig) << K1, EScriptNone, CheckSigVerify);
  // With another hash type after it: FindAndDelete removes only what matches whole
  Bytes otherType = sig;
  otherType.back() = SIGHASH_ALL | ACP;
  txInvalid(tx1(), Bare(Script() << verifyOne << otherType), ScriptSig() << sig << K1, EScriptNone, CheckSigVerify);

  // CHECKMULTISIG removes all its signatures ahead of hashing. A signature of the digest 1 (SIGHASH_SINGLE with no
  // output of the index) is known ahead and stands in the script as a key; were it not removed, the SIGHASH_ALL
  // signature beside it would fail. In a redeem script, as on mainnet, and bare
  const CTx second = tx1(2, 1, 1);
  const Bytes single = Signature(Sig(SIGHASH_SINGLE).By(K2).HighS(), second, Legacy, {});
  const CScriptSig multisig = Script() << 2 << single << K2 << K2 << 3 << OP_CHECKMULTISIG;
  const CSig all = Sig().By(K2).HighS().Over(Code(Script() << 2 << K2 << K2 << 3 << OP_CHECKMULTISIG));
  txValid(second, P2SH(multisig), ScriptSig() << 0 << single << all << Redeem(), EScriptConstScriptCode | EScriptLowS);
  txInvalid(second, P2SH(multisig), ScriptSig() << 0 << single << all << Redeem(), EScriptP2SH | EScriptConstScriptCode, SigFindAndDelete);
  txValid(second, Bare(multisig), ScriptSig() << 0 << single << single, EScriptConstScriptCode | EScriptLowS);
  txInvalid(second, Bare(multisig), ScriptSig() << 0 << single << single, EScriptConstScriptCode, SigFindAndDelete);
  // The first input of those transactions
  txValid(tx1(2, 1, 0), P2PKH(K1), ScriptSig() << Sig() << K1, EScriptNone);

  // The script is CHECKSIGVERIFY <signature>, the key comes from the stack. In P2SH the signature is of the script
  // with itself removed
  {
    const Bytes own = Signature(Sig().By(K1).HighS(), tx1(), Legacy, Code(Script() << OP_CHECKSIGVERIFY));
    txValid(tx1(), P2SH(Script() << OP_CHECKSIGVERIFY << own), ScriptSig() << own << K1 << Redeem(), EScriptConstScriptCode | EScriptLowS);
  }
  // In P2WSH nothing is removed and the digest covers the signature: the signature is picked first, the key is the
  // one it signs that digest under
  {
    const CChosenSig chosen = ChosenSig(1, SIGHASH_ALL, true);
    const CScriptSig script = Script() << OP_CHECKSIGVERIFY << chosen.Signature;
    txValid(tx1(), P2WSH(script), ScriptSig(), Witness() << chosen.Signature << chosen.KeyFor(tx1(), Bip143, Code(script)) << WitnessScript(),
            EScriptLowS);
    // The wrong digests. P2SH: the key signs the script with the signature left in
    txInvalid(tx1(), P2SH(script), ScriptSig() << chosen.Signature << chosen.KeyFor(tx1(), Legacy, Code(script)) << Redeem(), EScriptP2SH,
              CheckSigVerify);
    // P2WSH: a signature of the script with itself removed
    const Bytes removed = Signature(Sig().By(K1).HighS(), tx1(), Bip143, Code(Script() << OP_CHECKSIGVERIFY));
    txInvalid(tx1(), P2WSH(Script() << OP_CHECKSIGVERIFY << removed), ScriptSig(), Witness() << removed << K1 << WitnessScript(),
              EScriptP2SH | EScriptWitness, CheckSigVerify);
  }

  // The multisig version: the script is 2 CHECKMULTISIGVERIFY <sig1> <sig2> DROP, the keys come from the stack
  {
    const Bytes removed = Code(Script() << 2 << OP_CHECKMULTISIGVERIFY << OP_DROP);
    const Bytes first = Signature(Sig().By(K1).HighS(), tx1(), Legacy, removed), second = Signature(Sig().By(K2).HighS(), tx1(), Legacy, removed);
    txValid(tx1(), P2SH(Script() << 2 << OP_CHECKMULTISIGVERIFY << first << second << OP_DROP),
            ScriptSig() << 0 << first << second << 2 << K1 << K2 << Redeem(), EScriptConstScriptCode | EScriptLowS);

    const CChosenSig a = ChosenSig(2, SIGHASH_ALL, true), b = ChosenSig(3, SIGHASH_ALL, true);
    const CScriptSig script = Script() << 2 << OP_CHECKMULTISIGVERIFY << a.Signature << b.Signature << OP_DROP;
    txValid(tx1(), P2WSH(script), ScriptSig(), Witness() << Bytes() << a.Signature << b.Signature << "02"_hex
              << a.KeyFor(tx1(), Bip143, Code(script)) << b.KeyFor(tx1(), Bip143, Code(script)) << WitnessScript(), EScriptLowS);
    // The wrong digests, as above
    txInvalid(tx1(), P2SH(script), ScriptSig() << 0 << a.Signature << b.Signature << 2 << a.KeyFor(tx1(), Legacy, Code(script))
                << b.KeyFor(tx1(), Legacy, Code(script)) << Redeem(), EScriptP2SH, CheckMultisigVerify);
    const Bytes firstSegwit = Signature(Sig().By(K1).HighS(), tx1(), Bip143, removed);
    const Bytes secondSegwit = Signature(Sig().By(K2).HighS(), tx1(), Bip143, removed);
    txInvalid(tx1(), P2WSH(Script() << 2 << OP_CHECKMULTISIGVERIFY << firstSegwit << secondSegwit << OP_DROP), ScriptSig(),
              Witness() << Bytes() << firstSegwit << secondSegwit << "02"_hex << K1 << K2 << WitnessScript(), EScriptP2SH | EScriptWitness,
              CheckMultisigVerify);
  }
}

// CHECKLOCKTIMEVERIFY against the lock time of the transaction and the sequence of the input
TEST(InterpreterTx, CheckLockTimeVerify)
{
  // By-height locks, with argument == 0 and == tx nLockTime
  txValid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("0 CHECKLOCKTIMEVERIFY 1")), ScriptSig(), EScriptCleanStack);
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 499999999}, Bare(Script("499999999 CHECKLOCKTIMEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 499999999}, Bare(Script("0 CHECKLOCKTIMEVERIFY 1")), ScriptSig(), EScriptCleanStack);
  // By-time locks, with argument just beyond tx nLockTime (but within numerical boundaries)
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 500000000}, Bare(Script("500000000 CHECKLOCKTIMEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 4294967295}, Bare(Script("4294967295 CHECKLOCKTIMEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 4294967295}, Bare(Script("500000000 CHECKLOCKTIMEVERIFY")), ScriptSig(), EScriptNone);
  // Any non-maxint nSequence is fine
  txValid(CTx{.Version = 1, .Sequence = 0xfffffffe}, Bare(Script("0 CHECKLOCKTIMEVERIFY 1")), ScriptSig(), EScriptCleanStack);
  // The argument can be calculated rather than created directly by a PUSHDATA
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 500000000}, Bare(Script("499999999 1ADD CHECKLOCKTIMEVERIFY")), ScriptSig(), EScriptNone);
  // Perhaps even by an ADD producing a 5-byte result that is out of bounds for other opcodes
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 4294967294}, Bare(Script("2147483647 2147483647 ADD CHECKLOCKTIMEVERIFY")),
            ScriptSig(), EScriptNone);
  // 5 byte non-minimally-encoded arguments are valid
  txValid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("<0000000000> CHECKLOCKTIMEVERIFY 1")), ScriptSig(), EScriptMinimalData | EScriptCleanStack);
  // Valid CHECKLOCKTIMEVERIFY in scriptSig
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 1}, Bare(Script("1")),
            ScriptSig("1 CHECKLOCKTIMEVERIFY"), EScriptSigPushOnly | EScriptCleanStack);
  // Valid CHECKLOCKTIMEVERIFY in redeemScript
  txValid(CTx{.Version = 1, .Sequence = 0, .LockTime = 1}, P2SH(Script("1 CHECKLOCKTIMEVERIFY")), ScriptSig() << Redeem(), EScriptNone);
  // By-height locks, with argument just beyond tx nLockTime
  txInvalid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("1 CHECKLOCKTIMEVERIFY")), ScriptSig(), EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 499999998}, Bare(Script("499999999 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  // By-time locks, with argument just beyond tx nLockTime (but within numerical boundaries)
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 500000000}, Bare(Script("500000001 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 4294967294}, Bare(Script("4294967295 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  // Argument missing
  txInvalid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("CHECKLOCKTIMEVERIFY 1")), ScriptSig(), EScriptCheckLockTimeVerify, InvalidStackOperation);
  txInvalid(CTx{.Version = 1, .Sequence = 1}, Bare(Script("1")), ScriptSig("CHECKLOCKTIMEVERIFY"), EScriptCheckLockTimeVerify, InvalidStackOperation);
  // Argument negative with by-blockheight nLockTime=0
  txInvalid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("-1 CHECKLOCKTIMEVERIFY")), ScriptSig(), EScriptCheckLockTimeVerify, NegativeLocktime);
  // Argument negative with by-blocktime nLockTime=500,000,000
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 500000000}, Bare(Script("-1 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, NegativeLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 1, .LockTime = 2}, Bare(Script("1")), ScriptSig("0 1 SUB CHECKLOCKTIMEVERIFY"),
            EScriptCheckLockTimeVerify, NegativeLocktime);
  // Input locked
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("0 CHECKLOCKTIMEVERIFY 1")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff, .LockTime = 2}, Bare(Script("0")), ScriptSig("1 CHECKLOCKTIMEVERIFY"), EScriptNone, EvalFalse);
  // Another input being unlocked isn't sufficient; the CHECKLOCKTIMEVERIFY-using input must be unlocked
  txInvalid(CTx{.Inputs = 2, .Version = 1, .Sequence = 0xffffffff, .OtherSequence = 0}, Bare(Script("0 CHECKLOCKTIMEVERIFY 1")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  // Argument/tx height/time mismatch, both versions
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 500000000}, Bare(Script("0 CHECKLOCKTIMEVERIFY 1")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 500000000}, Bare(Script("0")), ScriptSig("1 CHECKLOCKTIMEVERIFY"), EScriptNone, EvalFalse);
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 500000000}, Bare(Script("499999999 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("500000000 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 499999999}, Bare(Script("500000000 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  // Argument 2^32 with nLockTime=2^32-1
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 4294967295}, Bare(Script("<0000000001> CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  // Same, but with nLockTime=2^31-1
  txInvalid(CTx{.Version = 1, .Sequence = 0, .LockTime = 2147483647}, Bare(Script("2147483648 CHECKLOCKTIMEVERIFY")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  // 6 byte non-minimally-encoded arguments are invalid even if their contents are valid
  txInvalid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("<000000000000> CHECKLOCKTIMEVERIFY 1")), ScriptSig(),
            EScriptCheckLockTimeVerify, UnknownError);
  // Failure due to failing CHECKLOCKTIMEVERIFY in scriptSig
  txInvalid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("1")), ScriptSig("1 CHECKLOCKTIMEVERIFY"), EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
  // Failure due to failing CHECKLOCKTIMEVERIFY in redeemScript
  txInvalid(CTx{.Version = 1, .Sequence = 0}, P2SH(Script("1 CHECKLOCKTIMEVERIFY")), ScriptSig() << Redeem(),
            EScriptP2SH | EScriptCheckLockTimeVerify, UnsatisfiedLocktime);
}

// CHECKSEQUENCEVERIFY against the sequence of the input and the version of the transaction
TEST(InterpreterTx, CheckSequenceVerify)
{
  // By-height locks, with argument == 0 and == txin.nSequence
  txValid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("0 CHECKSEQUENCEVERIFY 1")), ScriptSig(), EScriptCleanStack);
  txValid(CTx{.Version = 2, .Sequence = 0xffff}, Bare(Script("65535 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x7fbfffff}, Bare(Script("65535 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x7fbfffff}, Bare(Script("0 CHECKSEQUENCEVERIFY 1")), ScriptSig(), EScriptCleanStack);
  // By-time locks, with argument == 0 and == txin.nSequence
  txValid(CTx{.Version = 2, .Sequence = 0x400000}, Bare(Script("4194304 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x40ffff}, Bare(Script("4259839 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x7fffffff}, Bare(Script("4259839 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x7fffffff}, Bare(Script("4194304 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  // Upper sequence with upper sequence is fine
  txValid(CTx{.Version = 2, .Sequence = 0x80000000}, Bare(Script("2147483648 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x80000000}, Bare(Script("4294967295 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xfffffffe}, Bare(Script("2147483648 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xfffffffe}, Bare(Script("4294967295 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xffffffff}, Bare(Script("2147483648 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xffffffff}, Bare(Script("4294967295 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  // Argument 2^31 with various nSequence
  txValid(CTx{.Version = 2, .Sequence = 0x7fbfffff}, Bare(Script("2147483648 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x7fffffff}, Bare(Script("2147483648 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xffffffff}, Bare(Script("2147483648 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  // Argument 2^32-1 with various nSequence
  txValid(CTx{.Version = 2, .Sequence = 0x7fbfffff}, Bare(Script("4294967295 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x7fffffff}, Bare(Script("4294967295 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xffffffff}, Bare(Script("4294967295 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  // Argument 3<<31 with various nSequence
  txValid(CTx{.Version = 2, .Sequence = 0x7fbfffff}, Bare(Script("<0000008001> CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x7fffffff}, Bare(Script("<0000008001> CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xffffffff}, Bare(Script("<0000008001> CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  // 5 byte non-minimally-encoded operandss are valid
  txValid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("<0000000000> CHECKSEQUENCEVERIFY 1")), ScriptSig(), EScriptMinimalData | EScriptCleanStack);
  // The argument can be calculated rather than created directly by a PUSHDATA
  txValid(CTx{.Version = 2, .Sequence = 0x400000}, Bare(Script("4194303 1ADD CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0xffff}, Bare(Script("4194304 1SUB CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  // An ADD producing a 5-byte result that sets CTxIn::SEQUENCE_LOCKTIME_DISABLE_FLAG
  txValid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("2147483647 65536 ADD CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  txValid(CTx{.Version = 2, .Sequence = 0x400000}, Bare(Script("2147483647 4259840 ADD CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptNone);
  // Valid CHECKSEQUENCEVERIFY in scriptSig
  txValid(CTx{.Version = 2, .Sequence = 1}, Bare(Script("1")), ScriptSig("1 CHECKSEQUENCEVERIFY"), EScriptSigPushOnly | EScriptCleanStack);
  // Valid CHECKSEQUENCEVERIFY in redeemScript
  txValid(CTx{.Version = 2, .Sequence = 1}, P2SH(Script("1 CHECKSEQUENCEVERIFY")), ScriptSig() << Redeem(), EScriptNone);
  // Valid CHECKSEQUENCEVERIFY even with negative tx version number
  txValid(CTx{.Version = -1, .Sequence = 1}, P2SH(Script("1 CHECKSEQUENCEVERIFY")), ScriptSig() << Redeem(), EScriptNone);
  // By-height locks, with argument just beyond txin.nSequence
  txInvalid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("1 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 2, .Sequence = 0x40fffe}, Bare(Script("4259839 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  // By-time locks, with argument just beyond txin.nSequence (but within numerical boundaries)
  txInvalid(CTx{.Version = 2, .Sequence = 0x400000}, Bare(Script("4194305 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 2, .Sequence = 0x40fffe}, Bare(Script("4259839 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  // Argument missing
  txInvalid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("CHECKSEQUENCEVERIFY 1")), ScriptSig(), EScriptCheckSequenceVerify, InvalidStackOperation);
  // Argument negative with by-blockheight txin.nSequence=0
  txInvalid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("-1 CHECKSEQUENCEVERIFY")), ScriptSig(), EScriptCheckSequenceVerify, NegativeLocktime);
  // Argument negative with by-blocktime txin.nSequence=CTxIn::SEQUENCE_LOCKTIME_TYPE_FLAG
  txInvalid(CTx{.Version = 2, .Sequence = 0x400000}, Bare(Script("-1 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, NegativeLocktime);
  // Argument/tx height/time mismatch, both versions
  txInvalid(CTx{.Version = 2, .Sequence = 0x400000}, Bare(Script("0 CHECKSEQUENCEVERIFY 1")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 2, .Sequence = 0x400000}, Bare(Script("65535 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("4194304 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("4259839 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  // 6 byte non-minimally-encoded arguments are invalid even if their contents are valid
  txInvalid(CTx{.Version = 2, .Sequence = 0xffff}, Bare(Script("<000000000000> CHECKSEQUENCEVERIFY 1")), ScriptSig(),
            EScriptCheckSequenceVerify, UnknownError);
  // Failure due to failing CHECKSEQUENCEVERIFY in scriptSig
  txInvalid(CTx{.Version = 2, .Sequence = 0}, Bare(Script("1")), ScriptSig("1 CHECKSEQUENCEVERIFY"), EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  // Failure due to failing CHECKSEQUENCEVERIFY in redeemScript
  txInvalid(CTx{.Version = 2, .Sequence = 0}, P2SH(Script("1 CHECKSEQUENCEVERIFY")), ScriptSig() << Redeem(),
            EScriptP2SH | EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  // Failure due to insufficient tx.version (<2)
  txInvalid(CTx{.Version = 1, .Sequence = 0}, Bare(Script("0 CHECKSEQUENCEVERIFY 1")), ScriptSig(), EScriptCheckSequenceVerify, UnsatisfiedLocktime);
  txInvalid(CTx{.Version = 1, .Sequence = 0x400000}, Bare(Script("4194304 CHECKSEQUENCEVERIFY")), ScriptSig(),
            EScriptCheckSequenceVerify, UnsatisfiedLocktime);
}

// Witness programs
TEST(InterpreterTx, Witness)
{
  // Valid P2WPKH, P2WSH, P2SH(P2WPKH), P2SH(P2WSH)
  txValid(tx1(), P2WPKH(K1), ScriptSig(), Witness() << Sig() << K1, EScriptNone);
  txValid(tx1(), P2WSH(Script() << K1 << OP_CHECKSIG), ScriptSig(), Witness() << Sig().By(K1) << WitnessScript(), EScriptNone);
  txValid(tx1(), P2SH(P2WPKH(K1)), ScriptSig() << Redeem(), Witness() << Sig() << K1, EScriptNone);
  txValid(tx1(), P2SH(P2WSH(Script() << K1 << OP_CHECKSIG)), ScriptSig() << Redeem(), Witness() << Sig().By(K1) << WitnessScript(), EScriptNone);

  // Unknown witness program version, without DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM and with it
  const COutput future = WitnessProgram(16, Hash160(K1));
  txValid(tx1(3, 3, 1), future, ScriptSig(), Witness() << Sig(SIGHASH_ALL | ACP).By(K1) << K1, EScriptDiscourageUpgradableWitnessProgram);
  txInvalid(tx1(3, 3, 1), future, ScriptSig(), Witness() << Sig(SIGHASH_ALL | ACP).By(K1) << K1,
            EScriptP2SH | EScriptWitness | EScriptDiscourageUpgradableWitnessProgram, DiscourageUpgradableWitnessProgram);
  // Unknown length for witness program v0
  txInvalid(tx1(3, 4, 1), WitnessProgram(0, Concat(Hash160(K1), {0xff})), ScriptSig(), Witness() << Sig(SIGHASH_NONE).By(K1) << K1,
            EScriptP2SH | EScriptWitness, WitnessProgramWrongLength);
  // Witness with a push of 520 bytes
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff}, P2WSH(Script("DROP 1")), ScriptSig(), Witness() << Bytes(520, 0x00)
              << WitnessScript(), EScriptNone);
  // Unknown version witness program with empty witness
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("16 <4c9c3dfac4207d5d8cb89df5722cb3d712385e3f>")), ScriptSig(),
            Witness(), EScriptDiscourageUpgradableWitnessProgram);
  // 1 byte push should not be considered a witness scriptPubKey
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("16 <01>")), ScriptSig(), EScriptMinimalData | EScriptCleanStack);
  // 41 bytes push should not be considered a witness scriptPubKey
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff},
            Bare(Script("16 <ff25429251b5a84f452230a3c75fd886b7fc5a7865ce4a7bb7a9d7c5be6da3dbff0000000000000000>")), ScriptSig(), EScriptCleanStack);
  // The witness version must use OP_1 to OP_16 only
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("<10> <0001>")), ScriptSig(), EScriptMinimalData | EScriptCleanStack);
  // The witness program push must be canonical
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("16 PUSHDATA1<0001>")), ScriptSig(), EScriptMinimalData | EScriptCleanStack);
  // Witness with a push of 521 bytes
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, P2WSH(Script("DROP 1")), ScriptSig(), Witness() << Bytes(521, 0x00) << WitnessScript(),
            EScriptP2SH | EScriptWitness, PushSize);
  // Witness with unknown version which push false on the stack should be invalid (even without DISCOURAGE_UPGRADABLE_WITNESS_PROGRAM)
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("16 <0000>")), ScriptSig(), Witness() << "01"_hex, EScriptNone, EvalFalse);
  // Witness program should leave clean stack
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, P2WSH(Script("1 1")), ScriptSig(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  // Witness v0 with a push of 2 bytes
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("0 <0001>")), ScriptSig(), Witness() << "00020001"_hex,
            EScriptP2SH | EScriptWitness, WitnessProgramWrongLength);
  // Unknown witness version with non empty scriptSig
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("16 <0001>")), ScriptSig("1"), Witness(),
            EScriptP2SH | EScriptWitness, WitnessMalleated);
  // P2WSH with a redeem representing a witness scriptPubKey should fail due to too many stack items
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, P2WSH(Script("1 <0101>")), ScriptSig(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  // P2WSH with an empty redeem should fail due to empty stack
  txInvalid(CTx{.Version = 2, .Sequence = 0xffffffff}, P2WSH(Script()), ScriptSig(), Witness() << WitnessScript(),
            EScriptP2SH | EScriptWitness, CleanStack);
  // 33 bytes push should be considered a witness scriptPubKey
  txInvalid(CTx{.Version = 1, .Sequence = 0xffffffff}, Bare(Script("16 <ff25429251b5a84f452230a3c75fd886b7fc5a7865ce4a7bb7a9d7c5be6da3dbff>")),
            ScriptSig(), Witness(), EScriptP2SH | EScriptWitness | EScriptDiscourageUpgradableWitnessProgram, DiscourageUpgradableWitnessProgram);

  // Test long outputs, which are streamed using length-prefixed bitcoin strings. This might be surprising
  txValid(CTx{.Version = 1, .Sequence = 0xffffffff, .OutputScript = Code(Script() << Bytes(220, 0x11) << OP_DROP << K1 << OP_CHECKSIG)},
          P2WPKH(K1), ScriptSig(), Witness() << Sig() << K1, EScriptNone);
}

// The hash types of a segwit signature: the signature of one transaction in another that differs in what the type
// does not sign, and in what it does
TEST(InterpreterTx, WitnessSighash)
{
  const COutput output = P2WPKH(K1);
  auto witness = [](uint8_t type) { return Witness() << Sig(type) << K1; };
  const uint32_t segwit = EScriptP2SH | EScriptWitness;

  // Witness with SigHash Single|AnyoneCanPay: the other inputs and outputs are not signed, the output of its index is
  txValid(tx1(4, 5, 1), output, ScriptSig(), witness(SIGHASH_SINGLE | ACP), EScriptNone);
  for (const CChange &change: {OtherOutpoint(), OtherSequence(), AddInput(), OutputValue(0), OutputValue(2), AddOutput()})
    txValid(tx1(3, 3, 1, change), output, ScriptSig(), witness(SIGHASH_SINGLE | ACP), EScriptNone);
  txInvalid(tx1(3, 3, 1, OutputValue(1)), output, ScriptSig(), witness(SIGHASH_SINGLE | ACP), segwit, EvalFalse);

  // Witness with SigHash Single: the other outputs and the other sequences are not signed
  txValid(tx1(3, 4, 1), output, ScriptSig(), witness(SIGHASH_SINGLE), EScriptNone);
  for (const CChange &change: {OtherSequence(), OutputValue(0), OutputValue(2), AddOutput()})
    txValid(tx1(3, 3, 1, change), output, ScriptSig(), witness(SIGHASH_SINGLE), EScriptNone);

  // Witness with SigHash None|AnyoneCanPay: its own input alone is signed
  txValid(tx1(4, 4, 2), output, ScriptSig(), witness(SIGHASH_NONE | ACP), EScriptNone);
  for (const CChange &change: {OtherOutpoint(), OtherSequence(), AddInput(), OutputValue(1), AddOutput()})
    txValid(tx1(3, 3, 1, change), output, ScriptSig(), witness(SIGHASH_NONE | ACP), EScriptNone);
  txInvalid(tx1(3, 3, 1, Sequence()), output, ScriptSig(), witness(SIGHASH_NONE | ACP), segwit, EvalFalse);

  // Witness with SigHash None: no output is signed, nor the other sequences
  txValid(tx1(3, 4, 1), output, ScriptSig(), witness(SIGHASH_NONE), EScriptNone);
  for (const CChange &change: {OtherSequence(), OutputValue(1), AddOutput()})
    txValid(tx1(3, 3, 1, change), output, ScriptSig(), witness(SIGHASH_NONE), EScriptNone);

  // Witness with SigHash All|AnyoneCanPay: the other inputs are not signed, every output is
  txValid(tx1(4, 3, 2), output, ScriptSig(), witness(SIGHASH_ALL | ACP), EScriptNone);
  for (const CChange &change: {OtherOutpoint(), OtherSequence(), AddInput()})
    txValid(tx1(3, 3, 1, change), output, ScriptSig(), witness(SIGHASH_ALL | ACP), EScriptNone);
  txInvalid(tx1(3, 3, 1, OutputValue(2)), output, ScriptSig(), witness(SIGHASH_ALL | ACP), segwit, EvalFalse);

  // Transaction mixing all SigHash, segwit and normal inputs
  for (size_t i = 0; i < 12; i++) {
    if (i < 2 || (i >= 7 && i <= 10))
      txValid(tx1(12, 12, i), output, ScriptSig(), witness(SIGHASH_ALL), EScriptNone);
    else
      txValid(tx1(12, 12, i), P2PKH(K1), ScriptSig() << Sig() << K1, EScriptNone);
  }

  // Witness SIGHASH_SINGLE with output out of bound
  txValid(tx1(2, 1, 1), P2WSH(Script() << OP_DUP << OP_HASH160 << Hash160(K1) << OP_EQUALVERIFY << OP_CHECKSIG), ScriptSig(),
          Witness() << Sig(SIGHASH_SINGLE).By(K1) << K1 << WitnessScript(), EScriptNone);

  // Witness Single|AnyoneCanPay does not hash input's position: valid with the inputs and their outputs permuted
  for (size_t i = 0; i < 2; i++) {
    txValid(tx1(2, 2, i), output, ScriptSig(), witness(SIGHASH_SINGLE | ACP), EScriptNone);
    txValid(tx1(2, 2, i, Permute()), output, ScriptSig(), witness(SIGHASH_SINGLE | ACP), EScriptNone);
  }
}

// The examples of BIP143
TEST(InterpreterTx, Bip143)
{
  // P2WSH with OP_CODESEPARATOR and out-of-range SIGHASH_SINGLE: the second signature is of what follows the separator
  txValid(tx1(2, 1, 0), P2PK(K1), ScriptSig() << Sig(), EScriptNone);
  txValid(tx1(2, 1, 1), P2WSH(Script() << K2 << OP_CHECKSIGVERIFY << OP_CODESEPARATOR << K6 << OP_CHECKSIG), ScriptSig(),
          Witness() << Sig(SIGHASH_SINGLE).By(K6).Over(Code(Script() << K6 << OP_CHECKSIG)) << Sig(SIGHASH_SINGLE).By(K2) << WitnessScript(),
          EScriptNone);

  // P2WSH with unexecuted OP_CODESEPARATOR and SINGLE|ANYONECANPAY, and with the separator run
  const CScriptSig skipped = Script() << 0 << OP_IF << OP_CODESEPARATOR << OP_ENDIF << K1 << OP_CHECKSIG;
  const CScriptSig run = Script() << 1 << OP_IF << OP_CODESEPARATOR << OP_ENDIF << K1 << OP_CHECKSIG;
  const CWitness ofWhole = Witness() << Sig(SIGHASH_SINGLE | ACP).By(K1) << WitnessScript();
  const CWitness ofRest = Witness() << Sig(SIGHASH_SINGLE | ACP).By(K1).Over(Code(Script() << OP_ENDIF << K1 << OP_CHECKSIG)) << WitnessScript();
  txValid(tx1(2, 2, 0), P2WSH(skipped), ScriptSig(), ofWhole, EScriptNone);
  txValid(tx1(2, 2, 1), P2WSH(run), ScriptSig(), ofRest, EScriptNone);
  // Same as the previous example with input-output pairs swapped
  txValid(tx1(2, 2, 0, Permute()), P2WSH(skipped), ScriptSig(), ofWhole, EScriptNone);
  txValid(tx1(2, 2, 1, Permute()), P2WSH(run), ScriptSig(), ofRest, EScriptNone);

  // P2SH-P2WSH 6-of-6 multisig signed with 6 different SIGHASH types
  txValid(tx1(1, 2, 0), P2SH(P2WSH(Script() << 6 << K1 << K2 << K3 << K4 << K5 << K6 << 6 << OP_CHECKMULTISIG)), ScriptSig() << Redeem(),
          Witness() << Bytes() << Sig(SIGHASH_ALL).By(K1) << Sig(SIGHASH_NONE).By(K2) << Sig(SIGHASH_SINGLE).By(K3) << Sig(SIGHASH_ALL | ACP).By(K4)
            << Sig(SIGHASH_NONE | ACP).By(K5) << Sig(SIGHASH_SINGLE | ACP).By(K6) << WitnessScript(), EScriptNone);
}

// Core's test_big_witness_transaction: every input a witness pay to a key hash, signed with the six hash types in
// turn. 4500 inputs there, for its digest cache and check queue
TEST(InterpreterTx, WitnessHashTypesInTurn)
{
  const uint8_t types[] = {SIGHASH_NONE | ACP, SIGHASH_SINGLE | ACP, SIGHASH_ALL | ACP, SIGHASH_NONE, SIGHASH_SINGLE, SIGHASH_ALL};
  for (size_t i = 0; i < 12; i++)
    interpret(tx1(12, 12, i), P2WPKH(K1), ScriptSig(), Witness() << Sig(types[i % 6]) << K1, EScriptP2SH | EScriptWitness, Ok);
}

// Core's checkinputs_test: under any flags a spend is valid when none of the flags that fail it is among them
TEST(InterpreterTx, CheckInputs)
{
  // A byte between the signature and its hash type: not DER
  const CScriptSig padded = ScriptSig() << Sig().DoubleType();
  interpret(P2PK(K1), padded, EScriptP2SH | EScriptDerSig, SigDer);
  validUnless(tx1(1, 4), P2PK(K1), padded, EScriptDerSig | EScriptLowS | EScriptStrictEnc);

  // The redeem script and no signature for it: fails once it runs
  validUnless(tx1(), P2SH(P2PK(K1)), ScriptSig() << Redeem(), EScriptP2SH);

  // A lock time of 100, the script asks for 101, then for 100; the same with a sequence of 100
  const COutput lockTime = Bare(Script() << OP_CHECKLOCKTIMEVERIFY << OP_DROP << K1 << OP_CHECKSIG);
  const CTx locked{.Version = 1, .Sequence = 0, .LockTime = 100};
  validUnless(locked, lockTime, ScriptSig() << Sig().By(K1) << 101, EScriptCheckLockTimeVerify);
  validUnless(locked, lockTime, ScriptSig() << Sig().By(K1) << 100, EScriptNone);
  const COutput sequence = Bare(Script() << OP_CHECKSEQUENCEVERIFY << OP_DROP << K1 << OP_CHECKSIG);
  const CTx aged{.Version = 2, .Sequence = 100};
  validUnless(aged, sequence, ScriptSig() << Sig().By(K1) << 101, EScriptCheckSequenceVerify);
  validUnless(aged, sequence, ScriptSig() << Sig().By(K1) << 100, EScriptNone);

  // A witness spend, and its witness taken away
  validUnless(tx1(), P2WPKH(K1), ScriptSig(), Witness() << Sig() << K1, EScriptNone);
  validUnless(tx1(), P2WPKH(K1), ScriptSig(), Witness(), EScriptWitness);

  // The two inputs of one transaction, and the witness of the second taken away
  validUnless(tx1(2, 1, 0), P2SH(P2PK(K1)), ScriptSig() << Sig() << Redeem(), EScriptNone);
  validUnless(tx1(2, 1, 1), P2WPKH(K1), ScriptSig(), Witness() << Sig() << K1, EScriptNone);
  validUnless(tx1(2, 1, 1), P2WPKH(K1), ScriptSig(), Witness(), EScriptWitness);
}
