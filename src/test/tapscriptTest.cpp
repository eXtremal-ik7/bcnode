// Taproot's script path (BIP341) and tapscript (BIP342): a spend to a line with its verdict along the
// flag sets (spendBuilder.h). Signatures sign the builder's own BIP342 digest

#include "spendBuilder.h"

using namespace SpendTest;
using enum BTC::EScriptError;

namespace {

const Bytes X1 = K1.xOnly();
const Bytes X2 = K2.xOnly();

// Before TAPROOT a v1 program is a future one, the fast path takes the spend for valid; from TAPROOT on
// the interpreter's verdict
CVerdicts Tapscript(CVerdicts fromTaproot)
{
  CVerdicts verdicts(Ok);
  for (size_t i = static_cast<size_t>(EFlagSet::Taproot); i < verdicts.At.size(); i++)
    verdicts.At[i] = Interpreter(fromTaproot.At[i].Error);
  return verdicts;
}

// The stack the leaf's script starts with, then the script and the control block
CWitness ScriptPath(CWitness items = Witness())
{
  return items << LeafScript() << Control();
}

// The output of a single leaf, its internal key K1
COutput Leaf1(const CScriptSig &script)
{
  return P2TR(K1, Leaf(script));
}

CWitness Items(size_t count, const Bytes &item = {})
{
  CWitness witness;
  for (size_t i = 0; i < count; i++)
    witness = witness << item;
  return witness;
}

Bytes Prefix(const Bytes &data, size_t size)
{
  return Bytes(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(size));
}

}

// The control block: the leaf version and the parity of the output key, the internal key, the path
TEST(Tapscript, Commitment)
{
  const COutput checksig = Leaf1(Script() << X1 << OP_CHECKSIG);
  spend(checksig, ScriptPath(Witness() << Schnorr()),                   Tapscript(Ok));
  spend(checksig, ScriptPath(Witness() << Schnorr().FlipR()),           Tapscript(SchnorrSig));
  spend(checksig, ScriptPath(Witness() << Schnorr().By(K2)),            Tapscript(SchnorrSig));

  // Deeper in the tree: TapBranch takes the two hashes in byte order, the leaf's is on either side
  const Bytes sibling = LeafHash(Leaf(Script("1")));
  const COutput deep = P2TR(K1, Leaf(Script() << X1 << OP_CHECKSIG), {Zeros(32), Bytes(32, 0xff), sibling});
  spend(deep, ScriptPath(Witness() << Schnorr()),                       Tapscript(Ok));
  // 128 levels at most
  const COutput deepest = P2TR(K1, Leaf(Script("1")), std::vector<Bytes>(128, Bytes(32, 0x11)));
  spend(deepest, ScriptPath(),                                          Tapscript(Ok));

  // Of no size a control block has: short of the internal key, a level cut short, a level past 128
  spend(checksig, Witness() << Schnorr() << LeafScript() << Bytes{},                              Tapscript(TaprootWrongControlSize));
  spend(checksig, Witness() << Schnorr() << LeafScript() << Prefix(checksig.Control, 32),         Tapscript(TaprootWrongControlSize));
  spend(checksig, Witness() << Schnorr() << LeafScript() << Concat(checksig.Control, Zeros(31)), Tapscript(TaprootWrongControlSize));
  spend(deepest,  Witness() << LeafScript() << Concat(deepest.Control, Bytes(32, 0x11)),          Tapscript(TaprootWrongControlSize));

  // Not what the output commits to: the parity, the internal key, one off the curve, the leaf version,
  // a branch, the script
  Bytes parity = checksig.Control, internal = checksig.Control, offCurve = checksig.Control, version = checksig.Control;
  parity[0] ^= 1;
  std::copy(X2.begin(), X2.end(), internal.begin() + 1);
  std::fill(offCurve.begin() + 1, offCurve.end(), 0xff);
  version[0] ^= 0x02;
  Bytes branch = deep.Control;
  branch.back() ^= 1;
  spend(checksig, Witness() << Schnorr() << LeafScript() << parity,           Tapscript(WitnessProgramMismatch));
  spend(checksig, Witness() << Schnorr() << LeafScript() << internal,         Tapscript(WitnessProgramMismatch));
  spend(checksig, Witness() << Schnorr() << LeafScript() << offCurve,         Tapscript(WitnessProgramMismatch));
  spend(checksig, Witness() << Schnorr() << LeafScript() << version,          Tapscript(WitnessProgramMismatch));
  spend(deep,     Witness() << Schnorr() << LeafScript() << branch,           Tapscript(WitnessProgramMismatch));
  spend(checksig, Witness() << Schnorr() << Code(Script("1")) << Control(),   Tapscript(WitnessProgramMismatch));

  // A leaf version of the future: valid whatever its script
  spend(P2TR(K1, Leaf(Script("RETURN"), 0xc2)), ScriptPath(),  Tapscript(Ok | Standard(DiscourageUpgradableTaprootVersion)));
  spend(P2TR(K1, Leaf(Bytes{}, 0xfe)),          ScriptPath(),  Tapscript(Ok | Standard(DiscourageUpgradableTaprootVersion)));

  // P2SH of a v1 program is no taproot
  spend(P2SH(checksig), ScriptSig() << Redeem(), ScriptPath(Witness() << Schnorr()),
        Interpreter(Ok | Standard(DiscourageUpgradableWitnessProgram)));
}

// The annex: the last of two elements and more that starts with 0x50, out of the stack, under the signatures
TEST(Tapscript, Annex)
{
  const COutput checksig = Leaf1(Script() << X1 << OP_CHECKSIG);
  spend(checksig, ScriptPath(Witness() << Schnorr()) << Annex(),                        Tapscript(Ok));
  spend(checksig, ScriptPath(Witness() << Schnorr()) << Bytes{0x50},                    Tapscript(Ok));
  spend(CTx{.Then = OtherAnnex()}, checksig, ScriptPath(Witness() << Schnorr()) << Annex(), Tapscript(SchnorrSig));
  // The key path: the fast path leaves it to the interpreter
  spend(P2TR(K1), Witness() << Schnorr() << Annex(),                                    Tapscript(Ok));
  spend(CTx{.Then = OtherAnnex()}, P2TR(K1), Witness() << Schnorr() << Annex(),         Tapscript(SchnorrSig));
}

// OP_SUCCESSx anywhere in the script, executed or not, makes the spend valid; looked for ahead of the
// limits, after the leaf version
TEST(Tapscript, OpSuccess)
{
  const CVerdicts success = Tapscript(Ok | Standard(DiscourageOpSuccess));
  // The ranges' ends: 80, 98, 126-129, 131-134, 137-138, 141-142, 149-153, 187-254
  for (uint8_t op: {0x50, 0x62, 0x7e, 0x81, 0x83, 0x86, 0x89, 0x8a, 0x8d, 0x8e, 0x95, 0x99, 0xbb, 0xfe}) {
    SCOPED_TRACE(op);
    spend(Leaf1(Script() << Op(op)), ScriptPath(), success);
  }
  // Their neighbours execute
  for (uint8_t op: {0x82, 0x87, 0x8b, 0x8c, 0x8f, 0x9a, 0xba}) {
    SCOPED_TRACE(op);
    spend(Leaf1(Script() << Op(op)), ScriptPath(), Tapscript(InvalidStackOperation));
  }
  spend(Leaf1(Script() << Op(0xff)),                     ScriptPath(),  Tapscript(BadOpcode));
  // OP_VERIF stays bad unexecuted
  spend(Leaf1(Script("0 IF VERIF ENDIF 1")),             ScriptPath(),  Tapscript(BadOpcode));

  spend(Leaf1(Script("RETURN") << Op(0x50)),             ScriptPath(),  success);
  spend(Leaf1(Script("0 IF") << Op(0xbb) << Script("ENDIF 0")), ScriptPath(), success);
  // Found while the script parses: ahead of a push cut short, not after one
  spend(P2TR(K1, Leaf(Bytes{0x50, 0x4d, 0xff})),         ScriptPath(),  success);
  spend(P2TR(K1, Leaf(Bytes{0x4d, 0xff})),               ScriptPath(),  Tapscript(BadOpcode));
  // Ahead of the stack's limit
  spend(Leaf1(Script() << Op(0x50)),                     ScriptPath(Items(1001)), success);
  // After the leaf version
  spend(P2TR(K1, Leaf(Bytes{0x50}, 0xc2)),               ScriptPath(),  Tapscript(Ok | Standard(DiscourageUpgradableTaprootVersion)));
}

// No limit on the script's size or its opcodes; the stack's 1000 from the start; elements of 520 bytes
TEST(Tapscript, Limits)
{
  spend(Leaf1(Script("NOP*10001 1")),                    ScriptPath(),                       Tapscript(Ok));
  spend(Leaf1(Script("2DROP*500 1")),                    ScriptPath(Items(1000)),            Tapscript(Ok));
  spend(Leaf1(Script("2DROP*500 1")),                    ScriptPath(Items(1001)),            Tapscript(StackSize));
  spend(Leaf1(Script("1 DUP*999 DROP*999")),             ScriptPath(),                       Tapscript(Ok));
  spend(Leaf1(Script("1 DUP*1000 DROP*1000")),           ScriptPath(),                       Tapscript(StackSize));
  // The altstack counts in
  spend(Leaf1(Script("1 DUP*999 TOALTSTACK 1")),         ScriptPath(),                       Tapscript(StackSize));
  spend(Leaf1(Script("DROP 1")),                         ScriptPath(Items(1, Bytes(520, 1))), Tapscript(Ok));
  spend(Leaf1(Script("DROP 1")),                         ScriptPath(Items(1, Bytes(521, 1))), Tapscript(PushSize));
  // One element left, true
  spend(Leaf1(Script("1 1")),                            ScriptPath(),                       Tapscript(CleanStack));
  spend(Leaf1(Script("0")),                              ScriptPath(),                       Tapscript(EvalFalse));
}

// The argument of IF and NOTIF is empty or 0x01 whatever the flags
TEST(Tapscript, MinimalIf)
{
  const COutput branch = Leaf1(Script("IF 1 ELSE 0 ENDIF"));
  spend(branch, ScriptPath(Witness() << Bytes{0x01}),        Tapscript(Ok));
  spend(branch, ScriptPath(Witness() << Bytes{}),            Tapscript(EvalFalse));
  spend(branch, ScriptPath(Witness() << Bytes{0x02}),        Tapscript(TapscriptMinimalIf));
  spend(branch, ScriptPath(Witness() << Bytes{0x00}),        Tapscript(TapscriptMinimalIf));
  spend(branch, ScriptPath(Witness() << Bytes{0x01, 0x00}),  Tapscript(TapscriptMinimalIf));
  spend(Leaf1(Script("NOTIF 0 ELSE 1 ENDIF")), ScriptPath(Witness() << Bytes{0x02}), Tapscript(TapscriptMinimalIf));
}

// CHECKSIG: 64 bytes, or 65 with a hash type; an empty signature is false; a key of 32 bytes, an empty
// one refused, one of another size valid with any signature
TEST(Tapscript, CheckSig)
{
  const COutput checksig = Leaf1(Script() << X1 << OP_CHECKSIG);
  for (uint8_t type: {SIGHASH_ALL, SIGHASH_NONE, SIGHASH_SINGLE, uint8_t(SIGHASH_ALL | SIGHASH_ANYONECANPAY),
                      uint8_t(SIGHASH_NONE | SIGHASH_ANYONECANPAY), uint8_t(SIGHASH_SINGLE | SIGHASH_ANYONECANPAY)}) {
    SCOPED_TRACE(type);
    spend(checksig, ScriptPath(Witness() << Schnorr(type)),   Tapscript(Ok));
  }
  spend(checksig, ScriptPath(Witness() << Schnorr(SIGHASH_DEFAULT)), Tapscript(SchnorrSigHashtype));
  spend(checksig, ScriptPath(Witness() << Schnorr(0x04)),     Tapscript(SchnorrSigHashtype));
  spend(checksig, ScriptPath(Witness() << Schnorr(0x84)),     Tapscript(SchnorrSigHashtype));
  spend(CTx{.Inputs = 2, .Outputs = 1, .Checked = 1}, checksig, ScriptPath(Witness() << Schnorr(SIGHASH_SINGLE)),
        Tapscript(SchnorrSigHashtype));
  spend(checksig, ScriptPath(Witness() << Bytes(63, 1)),      Tapscript(SchnorrSigSize));
  spend(checksig, ScriptPath(Witness() << Bytes(66, 1)),      Tapscript(SchnorrSigSize));
  // A failed signature fails the spend: there is no NULLFAIL to ask
  spend(Leaf1(Script() << X1 << Script("CHECKSIG NOT")), ScriptPath(Witness() << Schnorr().FlipR()), Tapscript(SchnorrSig));
  spend(Leaf1(Script() << X1 << Script("CHECKSIG NOT")), ScriptPath(Witness() << Bytes{}),           Tapscript(Ok));
  spend(Leaf1(Script() << X1 << Script("CHECKSIGVERIFY 1")), ScriptPath(Witness() << Schnorr()),     Tapscript(Ok));
  spend(Leaf1(Script() << X1 << Script("CHECKSIGVERIFY 1")), ScriptPath(Witness() << Bytes{}),       Tapscript(CheckSigVerify));

  spend(Leaf1(Script("0 CHECKSIG")),                      ScriptPath(Witness() << Schnorr()),        Tapscript(PubKeyType));
  spend(Leaf1(Script("0 CHECKSIG NOT")),                  ScriptPath(Witness() << Bytes{}),          Tapscript(PubKeyType));
  const COutput upgradable = Leaf1(Script() << K1 << OP_CHECKSIG);
  spend(upgradable, ScriptPath(Witness() << Schnorr()),       Tapscript(Ok | Standard(DiscourageUpgradablePubKeyType)));
  spend(upgradable, ScriptPath(Witness() << Bytes(10, 1)),    Tapscript(Ok | Standard(DiscourageUpgradablePubKeyType)));
  spend(Leaf1(Script() << K1 << Script("CHECKSIG NOT")), ScriptPath(Witness() << Bytes{}),
        Tapscript(Ok | Standard(DiscourageUpgradablePubKeyType)));

  // CHECKMULTISIG is gone, executed
  spend(Leaf1(Script("0 0 0 CHECKMULTISIG")),             ScriptPath(),  Tapscript(TapscriptCheckMultisig));
  spend(Leaf1(Script("0 0 0 CHECKMULTISIGVERIFY 1")),     ScriptPath(),  Tapscript(TapscriptCheckMultisig));
  spend(Leaf1(Script("0 IF CHECKMULTISIG ENDIF 1")),      ScriptPath(),  Tapscript(Ok));
}

// <sig> <n> <key> CHECKSIGADD: n + 1 for a valid signature, n for an empty one
TEST(Tapscript, CheckSigAdd)
{
  const COutput twoOfTwo = Leaf1(Script() << X1 << OP_CHECKSIG << X2 << Script("CHECKSIGADD 2 NUMEQUAL"));
  spend(twoOfTwo, ScriptPath(Witness() << Schnorr().By(K2) << Schnorr()),       Tapscript(Ok));
  spend(twoOfTwo, ScriptPath(Witness() << Bytes{} << Schnorr()),                Tapscript(EvalFalse));
  spend(twoOfTwo, ScriptPath(Witness() << Schnorr().By(K2).FlipR() << Schnorr()), Tapscript(SchnorrSig));

  // n is a number of 4 bytes, minimal under MINIMALDATA
  const COutput add = Leaf1(Script() << X1 << Script("CHECKSIGADD 1 NUMEQUAL"));
  spend(add, ScriptPath(Witness() << Schnorr() << Bytes{}),                     Tapscript(Ok));
  spend(add, ScriptPath(Witness() << Schnorr() << Bytes{0x00}),                 Tapscript(Ok | Standard(UnknownError)));
  spend(add, ScriptPath(Witness() << Schnorr() << Bytes{0, 0, 0, 0, 0}),        Tapscript(UnknownError));
  spend(Leaf1(Script() << X1 << Script("CHECKSIGADD")), ScriptPath(Witness() << Bytes{}), Tapscript(InvalidStackOperation));

  // Not an opcode outside tapscript
  spend(P2WSH(Script("0 0 0 CHECKSIGADD")), Witness() << WitnessScript(), Interpreter(Ok | Segwit(BadOpcode)));
}

// The budget: the witness's serialized size and 50, a signature that is not empty takes 50
TEST(Tapscript, ValidationWeight)
{
  // 20 signatures fit in a witness of 950 bytes: the signature 1 + 64, the control block 1 + 33, the
  // count 1, the script 3 + 701 and the push of the padding - 146 bytes with 144 of padding
  const CScriptSig checks = Repeat(20, Script() << OP_DUP << X1 << OP_CHECKSIGVERIFY);
  spend(Leaf1(Script() << Bytes(144, 1) << OP_DROP << checks), ScriptPath(Witness() << Schnorr()), Tapscript(Ok));
  spend(Leaf1(Script() << Bytes(143, 1) << OP_DROP << checks), ScriptPath(Witness() << Schnorr()), Tapscript(TapscriptValidationWeight));
  // Empty signatures take nothing
  spend(Leaf1(Repeat(20, Script() << OP_DUP << X1 << Script("CHECKSIG DROP")) << Script("DROP 1")),
        ScriptPath(Witness() << Bytes{}), Tapscript(Ok));
}

// The signature commits to the position of the last OP_CODESEPARATOR executed, counted in opcodes
TEST(Tapscript, CodeSeparator)
{
  const COutput first = Leaf1(Script("CODESEPARATOR") << X1 << OP_CHECKSIG);
  spend(first, ScriptPath(Witness() << Schnorr().Separator(0)),   Tapscript(Ok));
  spend(first, ScriptPath(Witness() << Schnorr()),                Tapscript(SchnorrSig));
  spend(first, ScriptPath(Witness() << Schnorr().Separator(1)),   Tapscript(SchnorrSig));
  spend(Leaf1(Script() << X1 << Script("CODESEPARATOR CHECKSIG")), ScriptPath(Witness() << Schnorr().Separator(1)), Tapscript(Ok));
  spend(Leaf1(Script("CODESEPARATOR CODESEPARATOR") << X1 << OP_CHECKSIG), ScriptPath(Witness() << Schnorr().Separator(1)),
        Tapscript(Ok));
  // Not executed
  spend(Leaf1(Script("0 IF CODESEPARATOR ENDIF") << X1 << OP_CHECKSIG), ScriptPath(Witness() << Schnorr()), Tapscript(Ok));
}
