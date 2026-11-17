// Fast path: a spend to a line with its verdict along the flag
// sets (spendBuilder.h). The builder signs digests of its own, put together apart from BTC/sighash.h:
// a digest wrong in a byte there fails the spends here

#include "spendBuilder.h"

using namespace SpendTest;
using enum BTC::EScriptError;

namespace {

// x off the curve, as a compressed key; a prefix no key has
const Bytes OffCurve = Concat({0x02}, Bytes(32, 0xff));
const Bytes BadPrefix = Concat({0x05}, Bytes(32, 0x01));

}

// The keys and their hashes are what they are elsewhere: BIP173's example address is P2WPKH of key 1
TEST(FastPath, Keys)
{
  const Bytes program{0x75, 0x1e, 0x76, 0xe8, 0x19, 0x91, 0x96, 0xd4, 0x54, 0x94,
                      0x1c, 0x45, 0xd1, 0xb3, 0xa3, 0x23, 0xf1, 0x43, 0x3b, 0xd6};
  EXPECT_EQ(Hash160(K1), program);
  EXPECT_EQ(P2WPKH(K1).Script, Concat({0x00, 0x14}, program));
}

// P2PK: <sig> | <key> CHECKSIG
TEST(FastPath, PayToPubKey)
{
  spend(P2PK(K1),  ScriptSig() << Sig(),                          Ok);
  spend(P2PK(K1u), ScriptSig() << Sig(),                          Ok);
  spend(P2PK(K1h), ScriptSig() << Sig(),                          Ok | Standard(PubKeyType));
  spend(P2PK(K6),  ScriptSig() << Sig(),                          Ok);
  spend(P2PK(K6h), ScriptSig() << Sig(),                          Ok | Standard(PubKeyType));
  spend(P2PK(K1),  ScriptSig() << Sig(SIGHASH_NONE),              Ok);
  spend(P2PK(K1),  ScriptSig() << Sig().HighS(),                  Ok | Standard(SigHighS));
  spend(P2PK(K1),  ScriptSig() << Sig().PadR(),                   Ok | Bip66(SigDer));
  spend(P2PK(K1),  ScriptSig() << Sig().PadR().HighS(),           Ok | Bip66(SigDer));
  spend(P2PK(K1),  ScriptSig() << Sig(0x00),                      Ok | Standard(SigHashtype));
  spend(P2PK(K1),  ScriptSig() << Sig(0x04),                      Ok | Standard(SigHashtype));
  spend(P2PK(K1),  ScriptSig() << Sig(0x84),                      Ok | Standard(SigHashtype));
  spend(P2PK(K1),  ScriptSig() << Sig(0x41),                      Ok | Standard(SigHashtype));
  // The low five bits name NONE and SINGLE: 0x06 is neither, 0x23 is SINGLE
  spend(P2PK(K1),  ScriptSig() << Sig(0x06),                      Ok | Standard(SigHashtype));
  spend(P2PK(K1),  ScriptSig() << Sig(0x23),                      Ok | Standard(SigHashtype));
  spend(P2PK(K1),  ScriptSig() << Sig(0x04).HighS(),              Ok | Standard(SigHighS));
  spend(P2PK(K1),  ScriptSig() << Sig().AsType(SIGHASH_NONE),     EvalFalse | Standard(SigNullFail));
  spend(P2PK(K1),  ScriptSig() << Sig().FlipR(),                  EvalFalse | Standard(SigNullFail));
  spend(P2PK(K1),  ScriptSig() << Sig().By(K2),                   EvalFalse | Standard(SigNullFail));
  spend(P2PK(K1),  ScriptSig() << Bytes{},                        EvalFalse);
  // A push of 1 to 16 is OP_1 to OP_16's under MINIMALDATA
  spend(P2PK(K1),  ScriptSig() << Bytes{0x05},                    EvalFalse | Bip66(SigDer) | Standard(MinimalData));
  spend(P2PK(K1),  ScriptSig() << Bytes{0x10},                    EvalFalse | Bip66(SigDer) | Standard(MinimalData));
  spend(P2PK(K1),  ScriptSig() << Bytes{0x11},                    EvalFalse | Bip66(SigDer));
  spend(P2PK(K1),  ScriptSig() << Bytes{0x05} << Sig(),           Interpreter(Ok) | Standard(MinimalData));
  // FindAndDelete: the signature is the push of the key in the script code
  spend(P2PK(K1),  ScriptSig() << K1,                             EvalFalse | Bip66(SigDer) | Standard(SigFindAndDelete));
  // SIGHASH_SINGLE without its output signs 1, valid by consensus
  spend(CTx{.Inputs = 2, .Outputs = 1, .Checked = 1}, P2PK(K1), ScriptSig() << Sig(SIGHASH_SINGLE), Ok);
  // A witness: Core names it once the script succeeds, here it comes first
  spend(P2PK(K1),  ScriptSig() << Sig(), Witness() << Bytes{1},          Ok | Segwit(WitnessUnexpected));
  spend(P2PK(K1),  ScriptSig() << Sig().FlipR(), Witness() << Bytes{1},  EvalFalse | Segwit(WitnessUnexpected));
  // Off the template
  spend(P2PK(K1),  ScriptSig() << Pushdata1(Sig()),               Interpreter(Ok | Standard(MinimalData)));
  spend(P2PK(K1),  ScriptSig() << OP_0 << Sig(),                  Interpreter(Ok | Standard(CleanStack)));
  spend(P2PK(K1),  ScriptSig() << Sig() << OP_NOP,                Interpreter(Ok));
  spend(P2PK(K1),  ScriptSig(),                                   Interpreter(InvalidStackOperation));
}

// P2PKH: <sig> <key> | DUP HASH160 <hash> EQUALVERIFY CHECKSIG
TEST(FastPath, PayToPubKeyHash)
{
  spend(P2PKH(K1),  ScriptSig() << Sig() << K1,                   Ok);
  spend(P2PKH(K1u), ScriptSig() << Sig() << K1u,                  Ok);
  spend(P2PKH(K1h), ScriptSig() << Sig() << K1h,                  Ok | Standard(PubKeyType));
  spend(P2PKH(K6),  ScriptSig() << Sig() << K6,                   Ok);
  spend(P2PKH(K1),  ScriptSig() << Sig(SIGHASH_NONE | SIGHASH_ANYONECANPAY) << K1, Ok);
  spend(P2PKH(K1),  ScriptSig() << Sig().HighS() << K1,           Ok | Standard(SigHighS));
  spend(P2PKH(K1),  ScriptSig() << Sig().PadR() << K1,            Ok | Bip66(SigDer));
  spend(P2PKH(K1),  ScriptSig() << Sig(0x04) << K1,               Ok | Standard(SigHashtype));
  spend(P2PKH(K1),  ScriptSig() << Sig().FlipR() << K1,           EvalFalse | Standard(SigNullFail));
  spend(P2PKH(K1),  ScriptSig() << Bytes{} << K1,                 EvalFalse);
  spend(P2PKH(K1),  ScriptSig() << Sig() << K2,                   EqualVerify);
  spend(P2PKH(K1),  ScriptSig() << Sig() << K1u,                  EqualVerify);
  // MINIMALDATA at the push it fails, the first or the second
  spend(P2PKH(K1),  ScriptSig() << Bytes{0x81} << K1,             EvalFalse | Bip66(SigDer) | Standard(MinimalData));
  spend(P2PKH(K1),  ScriptSig() << Sig() << Bytes{0x01},          EqualVerify | Standard(MinimalData));
  spend(P2PKH(K1),  ScriptSig() << Bytes{0x00} << K1,             EvalFalse | Bip66(SigDer));
  // FindAndDelete: the signature is the push of the hash in the script code; one of its size is not
  spend(P2PKH(K1),  ScriptSig() << Hash160(K1) << K1,             EvalFalse | Bip66(SigDer) | Standard(SigFindAndDelete));
  spend(P2PKH(K1),  ScriptSig() << Zeros(20) << K1,               EvalFalse | Bip66(SigDer));
  // Keys libsecp256k1 does not take: 70 bytes, a prefix no key has, x off the curve
  spend(P2PKH(Bytes(70, 0x04)), ScriptSig() << Sig().By(K1) << Bytes(70, 0x04), EvalFalse | Standard(PubKeyType));
  spend(P2PKH(BadPrefix),       ScriptSig() << Sig().By(K1) << BadPrefix,       EvalFalse | Standard(PubKeyType));
  spend(P2PKH(OffCurve),        ScriptSig() << Sig().By(K1) << OffCurve,        EvalFalse | Standard(SigNullFail));
  spend(P2PKH(K1),  ScriptSig() << Sig() << K1, Witness() << Bytes{1}, Ok | Segwit(WitnessUnexpected));
  // Off the template
  spend(P2PKH(K1),  ScriptSig() << Pushdata1(Sig()) << K1,        Interpreter(Ok | Standard(MinimalData)));
  spend(P2PKH(K1),  ScriptSig() << Sig() << Pushdata1(K1),        Interpreter(Ok | Standard(MinimalData)));
  spend(P2PKH(K1),  ScriptSig() << OP_0 << Sig() << K1,           Interpreter(Ok | Standard(CleanStack)));
  spend(P2PKH(K1),  ScriptSig() << K1,                            Interpreter(InvalidStackOperation));
  spend(P2PKH(K1),  ScriptSig() << OP_1 << K1,                    Interpreter(EvalFalse | Bip66(SigDer)));
  spend(P2PKH(K1),  ScriptSig() << Raw({0x05, 0x01}),             Interpreter(BadOpcode));
}

// The DER of the signature: BIP66 refuses what the lax parser takes, the parser fails the rest
TEST(FastPath, SignatureEncoding)
{
  spend(P2PK(K1), ScriptSig() << Sig().HighR(),                   Ok);
  spend(P2PK(K1), ScriptSig() << Sig().PadR(),                    Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().PadS(),                    Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().HighR().UnpadR(),          Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().HighS().UnpadS(),          Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().LongTotal(),               Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().LongR(),                   Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().LongR(2),                  Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().WrongTotal(),              Ok | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().Trailing(),                Ok | Bip66(SigDer));
  // Over 73 bytes: too long for a direct push, a witness has room
  spend(P2WPKH(K1), Witness() << Sig().PadR(10) << K1,            Ok | Bip66(SigDer));
  // Strict DER, a zero signature to libsecp256k1
  spend(P2PK(K1), ScriptSig() << Sig().OverflowR(),               EvalFalse | Standard(SigNullFail));
  spend(P2PK(K1), ScriptSig() << Sig().OverflowS(),               EvalFalse | Standard(SigNullFail));
  // R = 1, S = 1: strict DER, no such signature
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01, 0x01}, EvalFalse | Standard(SigNullFail));
  spend(P2PK(K1), ScriptSig() << Sig().EmptyR(),                  EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().EmptyS(),                  EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().Tag(0x31),                 EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().TagR(0x03),                EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Sig().TagS(0x03),                EvalFalse | Bip66(SigDer));
  // Cut short or too long for the lax parser: R longer than the signature, the sequence or R ending
  // at its tag or its length, four length bytes of R, the long length of R zeros alone
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x06, 0x02, 0x7f, 0x01, 0x02, 0x01, 0x01, 0x01}, EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x01},                                      EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x00, 0x01},                                EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x84, 0x01},                                EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x01, 0x02, 0x01},                          EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x03, 0x02, 0x84, 0x00, 0x01},              EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x06, 0x02, 0x84, 0x01, 0x00, 0x00, 0x00, 0x01}, EvalFalse | Bip66(SigDer));
  spend(P2PK(K1), ScriptSig() << Bytes{0x30, 0x06, 0x02, 0x81, 0x00, 0x02, 0x01, 0x01, 0x01}, EvalFalse | Bip66(SigDer));
}

// P2WPKH: OP_0 <hash>, the witness <sig> <key>; anyone can spend it before WITNESS
TEST(FastPath, PayToWitnessPubKeyHash)
{
  spend(P2WPKH(K1),  Witness() << Sig() << K1,                    Ok);
  spend(P2WPKH(K1u), Witness() << Sig() << K1u,                   Ok | Standard(WitnessPubKeyType));
  spend(P2WPKH(K1h), Witness() << Sig() << K1h,                   Ok | Standard(PubKeyType));
  spend(P2WPKH(K6),  Witness() << Sig() << K6,                    Ok);
  spend(P2WPKH(K1),  Witness() << Sig(SIGHASH_SINGLE) << K1,      Ok);
  // SIGHASH_SINGLE without its output signs no outputs
  spend(CTx{.Inputs = 2, .Outputs = 1, .Checked = 1}, P2WPKH(K1), Witness() << Sig(SIGHASH_SINGLE) << K1, Ok);
  spend(P2WPKH(K1),  Witness() << Sig().HighS() << K1,            Ok | Standard(SigHighS));
  spend(P2WPKH(K1),  Witness() << Sig().PadR() << K1,             Ok | Bip66(SigDer));
  spend(P2WPKH(K1),  Witness() << Sig(0x84) << K1,                Ok | Standard(SigHashtype));
  spend(P2WPKH(K1),  Witness() << Sig(0x06) << K1,                Ok | Standard(SigHashtype));
  spend(P2WPKH(K1),  Witness() << Sig(0x23) << K1,                Ok | Standard(SigHashtype));
  spend(P2WPKH(K1),  Witness() << Sig().FlipR() << K1,            Ok | Segwit(EvalFalse) | Standard(SigNullFail));
  spend(P2WPKH(K1),  Witness() << Bytes{} << K1,                  Ok | Segwit(EvalFalse));
  spend(P2WPKH(K1),  Witness() << Sig() << K2,                    Ok | Segwit(EqualVerify));
  // No MINIMALDATA and no FindAndDelete in a witness
  spend(P2WPKH(K1),  Witness() << Bytes{0x05} << K1,              Ok | Segwit(EvalFalse) | Bip66(SigDer));
  spend(P2WPKH(K1),  Witness() << Hash160(K1) << K1,              Ok | Segwit(EvalFalse) | Bip66(SigDer));
  // Two elements exactly, 520 bytes at most
  spend(P2WPKH(K1),  Witness() << Sig(),                          Ok | Segwit(WitnessProgramMismatch));
  spend(P2WPKH(K1),  Witness() << Bytes{} << Sig() << K1,         Ok | Segwit(WitnessProgramMismatch));
  spend(P2WPKH(K1),  Witness(),                                   Ok | Segwit(WitnessProgramMismatch));
  spend(P2WPKH(K1),  Witness() << Sig() << Bytes(520, 0x02),      Ok | Segwit(EqualVerify));
  spend(P2WPKH(K1),  Witness() << Sig() << Bytes(521, 0x02),      Ok | Segwit(PushSize));
  spend(P2WPKH(K1),  Witness() << Bytes(521, 0x30) << K1,         Ok | Segwit(PushSize));
  // A program CastToBool takes for false, and one it takes for true by its last byte alone
  spend(P2WPKH(Zeros(20)),        Witness() << Sig().By(K1) << K1, EvalFalse);
  spend(P2WPKH(NegativeZero(20)), Witness() << Sig().By(K1) << K1, EvalFalse);
  spend(P2WPKH(Concat(Zeros(19), {0x01})), Witness() << Sig().By(K1) << K1, Ok | Segwit(EqualVerify));
  // Core runs a script sig ahead of the witness
  spend(P2WPKH(K1),  ScriptSig() << OP_0, Witness() << Sig() << K1, Interpreter(Ok | Segwit(WitnessMalleated)));
}

// P2SH-P2WPKH: <redeem> | HASH160 <hash> EQUAL, the redeem script OP_0 <hash> runs with the witness
TEST(FastPath, PayToScriptHashWitnessPubKeyHash)
{
  const COutput key1 = P2WPKH(K1);
  spend(P2SH(key1),  ScriptSig() << Redeem(key1),  Witness() << Sig() << K1,          Ok);
  spend(P2SH(P2WPKH(K1u)), ScriptSig() << Redeem(P2WPKH(K1u)), Witness() << Sig() << K1u, Ok | Standard(WitnessPubKeyType));
  spend(P2SH(key1),  ScriptSig() << Redeem(key1),  Witness() << Sig().FlipR() << K1,  Ok | Segwit(EvalFalse) | Standard(SigNullFail));
  spend(P2SH(key1),  ScriptSig() << Redeem(key1),  Witness() << Sig() << K2,          Ok | Segwit(EqualVerify));
  spend(P2SH(key1),  ScriptSig() << Redeem(key1),  Witness(),                         Ok | Segwit(WitnessProgramMismatch));
  // The redeem script of another output: its hash fails before P2SH too
  spend(P2SH(key1),  ScriptSig() << Redeem(P2WPKH(K2)), Witness() << Sig() << K1,     EvalFalse);
  // A program CastToBool takes for false: P2SH runs the redeem script
  const COutput zeros = P2WPKH(Zeros(20));
  spend(P2SH(zeros), ScriptSig() << Redeem(zeros), Witness() << Sig().By(K1) << K1,   Ok | P2sh(EvalFalse));
  spend(P2SH(key1),  ScriptSig() << Bytes{0x05} << Redeem(key1), Witness() << Sig() << K1, Interpreter(Ok | Segwit(WitnessMalleatedP2SH)) | Standard(MinimalData));
  // Off the template: a redeem script not of P2WPKH, not pushed directly, not alone, none
  const COutput p2wsh = P2WSH(Bytes(32, 0x22));
  const COutput v1 = WitnessProgram(1, Bytes(20, 0x01));
  const COutput push21 = Output(Concat({0x00, 0x15}, Bytes(20, 0x01)));
  spend(P2SH(p2wsh),  ScriptSig() << Redeem(p2wsh),  Witness() << Bytes{0x51},       Interpreter(Ok | Segwit(WitnessProgramMismatch)));
  spend(P2SH(v1),     ScriptSig() << Redeem(v1),     Witness(),                       Interpreter(Ok | Standard(DiscourageUpgradableWitnessProgram)));
  spend(P2SH(push21), ScriptSig() << Redeem(push21), Witness(),                       Interpreter(Ok | P2sh(BadOpcode)));
  spend(P2SH(key1),   ScriptSig() << Pushdata1(Redeem(key1)), Witness() << Sig() << K1, Interpreter(Ok | Segwit(WitnessMalleatedP2SH) | Standard(MinimalData)));
  spend(P2SH(key1),   ScriptSig() << OP_0 << Redeem(key1),   Witness() << Sig() << K1, Interpreter(Ok | Segwit(WitnessMalleatedP2SH)));
  spend(P2SH(key1),   Witness() << Sig() << K1,                                       Interpreter(InvalidStackOperation));
}

// P2TR key path: OP_1 <key>, the witness <sig>; anyone can spend it before TAPROOT
TEST(FastPath, TaprootKeyPath)
{
  const CTx noOutputOfItsOwn{.Inputs = 2, .Outputs = 1, .Checked = 1};
  spend(P2TR(K1), Witness() << Schnorr(),                         Ok);
  spend(P2TR(K1), Witness() << Schnorr(SIGHASH_ALL),              Ok);
  spend(P2TR(K6), Witness() << Schnorr(),                         Ok);
  spend(P2TR(K1), Witness() << Schnorr(SIGHASH_SINGLE | SIGHASH_ANYONECANPAY), Ok);
  spend(P2TR(K1), Witness() << Schnorr().FlipR(),                 Ok | Taproot(SchnorrSig));
  spend(P2TR(K1), Witness() << Schnorr().By(K2),                  Ok | Taproot(SchnorrSig));
  // The hash type is in the digest
  spend(P2TR(K1), Witness() << Schnorr().AsType(SIGHASH_ALL),     Ok | Taproot(SchnorrSig));
  // SIGHASH_DEFAULT never written, the hash type defined, SIGHASH_SINGLE with its output
  spend(P2TR(K1), Witness() << Schnorr(SIGHASH_DEFAULT),          Ok | Taproot(SchnorrSigHashtype));
  spend(P2TR(K1), Witness() << Schnorr(0x04),                     Ok | Taproot(SchnorrSigHashtype));
  spend(P2TR(K1), Witness() << Schnorr(0x80),                     Ok | Taproot(SchnorrSigHashtype));
  spend(P2TR(K1), Witness() << Schnorr(0x84),                     Ok | Taproot(SchnorrSigHashtype));
  spend(noOutputOfItsOwn, P2TR(K1), Witness() << Schnorr(SIGHASH_SINGLE), Ok | Taproot(SchnorrSigHashtype));
  spend(noOutputOfItsOwn, P2TR(K1), Witness() << Schnorr(SIGHASH_SINGLE | SIGHASH_ANYONECANPAY), Ok | Taproot(SchnorrSigHashtype));
  // 64 or 65 bytes
  spend(P2TR(K1), Witness() << Bytes{},                           Ok | Taproot(SchnorrSigSize));
  spend(P2TR(K1), Witness() << Bytes(63, 0x01),                   Ok | Taproot(SchnorrSigSize));
  spend(P2TR(K1), Witness() << Bytes(66, 0x01),                   Ok | Taproot(SchnorrSigSize));
  spend(P2TR(K1), Witness(),                                      Ok | Taproot(WitnessProgramWitnessEmpty));
  // A program CastToBool takes for false; one no key has
  spend(P2TR(Zeros(32)),       Witness() << Schnorr().By(K1),     EvalFalse);
  spend(P2TR(Bytes(32, 0xff)), Witness() << Schnorr().By(K1),     Ok | Taproot(SchnorrSig));
  // The script path, an annex, a script sig: the interpreter's
  spend(P2TR(K1), Witness() << Schnorr() << Bytes{0x01},          Ok | Taproot(Interpreter(TaprootWrongControlSize)));
  spend(P2TR(K1), Witness() << Schnorr() << Annex(),              Ok | Taproot(Interpreter(Ok)));
  spend(P2TR(K1), ScriptSig() << OP_0, Witness() << Schnorr(),    Interpreter(Ok | Segwit(WitnessMalleated)));
}

// Records off the five templates
TEST(FastPath, OtherOutputs)
{
  // P2PK of a key prefix no key has
  const Bytes noKey = Concat(Concat({0x21, 0x05}, Bytes(32, 0x01)), {0xac});
  spend(P2WSH(Bytes(32, 0x22)),             Witness() << Bytes{0x51},      Interpreter(Ok | Segwit(WitnessProgramMismatch)));
  spend(WitnessProgram(2, Bytes(32, 0x01)), Witness() << Bytes{0x01},      Interpreter(Ok | Standard(DiscourageUpgradableWitnessProgram)));
  spend(Anchor(),                           Witness(),                     Interpreter(Ok));
  // P2A runs nothing, a witness on it included; in P2SH, or a program beside it, a program of the future
  spend(Anchor(),                           Witness() << Bytes{0x01},      Interpreter(Ok));
  spend(P2SH(Anchor()), ScriptSig() << Redeem(), Witness(),                Interpreter(Ok | Standard(DiscourageUpgradableWitnessProgram)));
  spend(WitnessProgram(1, {0x4e, 0x74}),    Witness(),                     Interpreter(Ok | Standard(DiscourageUpgradableWitnessProgram)));
  spend(WitnessProgram(1, {0x4f, 0x73}),    Witness(),                     Interpreter(Ok | Standard(DiscourageUpgradableWitnessProgram)));
  spend(Output({0x51}),                     ScriptSig(),                   Interpreter(Ok));
  spend(Output(noKey),                      ScriptSig() << Sig().By(K1),   Interpreter(EvalFalse | Standard(PubKeyType)));
}

// What a signature commits to: a change after it is made, 1 where the change breaks it. Two inputs
// and two outputs, the input checked is 1

namespace {

struct CCommits {
  CChange Change;
  std::vector<int> Breaks;
};

const uint8_t EcdsaHashTypes[] = {
  SIGHASH_ALL, SIGHASH_NONE, SIGHASH_SINGLE,
  SIGHASH_ALL | SIGHASH_ANYONECANPAY, SIGHASH_NONE | SIGHASH_ANYONECANPAY, SIGHASH_SINGLE | SIGHASH_ANYONECANPAY
};

// Core's SignatureHash: the other inputs with no scripts, their sequences zero for NONE and SINGLE
const CCommits LegacyCommits[] = {
  //                  ALL NONE SINGLE  ACP|ALL ACP|NONE ACP|SINGLE
  {OutputValue(0),   {1,  0,   0,      1,      0,       0}},
  {OutputValue(1),   {1,  0,   1,      1,      0,       1}},
  {OutputScript(1),  {1,  0,   1,      1,      0,       1}},
  {AddOutput(),      {1,  0,   0,      1,      0,       0}},
  {SpentAmount(),    {0,  0,   0,      0,      0,       0}},
  {OtherAmount(),    {0,  0,   0,      0,      0,       0}},
  {OtherScript(),    {0,  0,   0,      0,      0,       0}},
  {Sequence(),       {1,  1,   1,      1,      1,       1}},
  {OtherSequence(),  {1,  0,   0,      0,      0,       0}},
  {Outpoint(),       {1,  1,   1,      1,      1,       1}},
  {OtherOutpoint(),  {1,  1,   1,      0,      0,       0}},
  {OtherScriptSig(), {0,  0,   0,      0,      0,       0}},
  {AddInput(),       {1,  1,   1,      0,      0,       0}},
  {LockTime(),       {1,  1,   1,      1,      1,       1}},
  {Version(),        {1,  1,   1,      1,      1,       1}}
};

// BIP143
const CCommits Bip143Commits[] = {
  //                  ALL NONE SINGLE  ACP|ALL ACP|NONE ACP|SINGLE
  {OutputValue(0),   {1,  0,   0,      1,      0,       0}},
  {OutputValue(1),   {1,  0,   1,      1,      0,       1}},
  {OutputScript(1),  {1,  0,   1,      1,      0,       1}},
  {AddOutput(),      {1,  0,   0,      1,      0,       0}},
  {SpentAmount(),    {1,  1,   1,      1,      1,       1}},
  {OtherAmount(),    {0,  0,   0,      0,      0,       0}},
  {OtherScript(),    {0,  0,   0,      0,      0,       0}},
  {Sequence(),       {1,  1,   1,      1,      1,       1}},
  {OtherSequence(),  {1,  0,   0,      0,      0,       0}},
  {Outpoint(),       {1,  1,   1,      1,      1,       1}},
  {OtherOutpoint(),  {1,  1,   1,      0,      0,       0}},
  {OtherScriptSig(), {0,  0,   0,      0,      0,       0}},
  {AddInput(),       {1,  1,   1,      0,      0,       0}},
  {LockTime(),       {1,  1,   1,      1,      1,       1}},
  {Version(),        {1,  1,   1,      1,      1,       1}}
};

// BIP341: SIGHASH_DEFAULT first, 64 bytes
const CCommits Bip341Commits[] = {
  //                  DEFAULT ALL NONE SINGLE  ACP|ALL ACP|NONE ACP|SINGLE
  {OutputValue(0),   {1,      1,  0,   0,      1,      0,       0}},
  {OutputValue(1),   {1,      1,  0,   1,      1,      0,       1}},
  {OutputScript(1),  {1,      1,  0,   1,      1,      0,       1}},
  {AddOutput(),      {1,      1,  0,   0,      1,      0,       0}},
  {SpentAmount(),    {1,      1,  1,   1,      1,      1,       1}},
  {OtherAmount(),    {1,      1,  1,   1,      0,      0,       0}},
  {OtherScript(),    {1,      1,  1,   1,      0,      0,       0}},
  {Sequence(),       {1,      1,  1,   1,      1,      1,       1}},
  {OtherSequence(),  {1,      1,  1,   1,      0,      0,       0}},
  {Outpoint(),       {1,      1,  1,   1,      1,      1,       1}},
  {OtherOutpoint(),  {1,      1,  1,   1,      0,      0,       0}},
  {OtherScriptSig(), {0,      0,  0,   0,      0,      0,       0}},
  {AddInput(),       {1,      1,  1,   1,      0,      0,       0}},
  {LockTime(),       {1,      1,  1,   1,      1,      1,       1}},
  {Version(),        {1,      1,  1,   1,      1,      1,       1}}
};

}

TEST(FastPath, LegacyDigest)
{
  for (const CCommits &row: LegacyCommits) {
    for (size_t i = 0; i < std::size(EcdsaHashTypes); i++) {
      const CTx tx{.Inputs = 2, .Outputs = 2, .Checked = 1, .Then = row.Change};
      const CVerdicts verdicts = row.Breaks.at(i) ? EvalFalse | Standard(SigNullFail) : Ok;
      spend(tx, P2PK(K1),  ScriptSig() << Sig(EcdsaHashTypes[i]),        verdicts);
      spend(tx, P2PKH(K1), ScriptSig() << Sig(EcdsaHashTypes[i]) << K1,  verdicts);
    }
  }
}

TEST(FastPath, Bip143Digest)
{
  const COutput key1 = P2WPKH(K1);
  for (const CCommits &row: Bip143Commits) {
    for (size_t i = 0; i < std::size(EcdsaHashTypes); i++) {
      const CTx tx{.Inputs = 2, .Outputs = 2, .Checked = 1, .Then = row.Change};
      const CVerdicts verdicts = row.Breaks.at(i) ? Ok | Segwit(EvalFalse) | Standard(SigNullFail) : Ok;
      spend(tx, key1,       Witness() << Sig(EcdsaHashTypes[i]) << K1,                              verdicts);
      spend(tx, P2SH(key1), ScriptSig() << Redeem(key1), Witness() << Sig(EcdsaHashTypes[i]) << K1, verdicts);
    }
  }
}

TEST(FastPath, Bip341Digest)
{
  for (const CCommits &row: Bip341Commits) {
    for (size_t i = 0; i <= std::size(EcdsaHashTypes); i++) {
      const CTx tx{.Inputs = 2, .Outputs = 2, .Checked = 1, .Then = row.Change};
      const CVerdicts verdicts = row.Breaks.at(i) ? Ok | Taproot(SchnorrSig) : Ok;
      spend(tx, P2TR(K1), Witness() << (i == 0 ? Schnorr() : Schnorr(EcdsaHashTypes[i - 1])), verdicts);
    }
  }
}

// Sync's batch: the jobs of many transactions, the ones that fail by their positions
TEST(FastPath, Batch)
{
  CBatch batch;
  batch.add(P2PK(K1),   ScriptSig() << Sig());
  batch.add(P2WPKH(K1), ScriptSig(), Witness() << Sig().FlipR() << K1);
  batch.add(P2TR(K1),   ScriptSig(), Witness() << Schnorr(), CTx{.Inputs = 2, .Checked = 1});
  batch.add(P2TR(K1),   ScriptSig(), Witness() << Schnorr().FlipR());
  batch.add(P2PKH(K1),  ScriptSig() << Sig() << K1);
  EXPECT_EQ(batch.failed(), (std::vector<uint32_t>{1, 3}));
  EXPECT_EQ(CVerdict(batch.jobs()[1].Error), CVerdict(EvalFalse));
  EXPECT_EQ(CVerdict(batch.jobs()[3].Error), CVerdict(SchnorrSig));
}
