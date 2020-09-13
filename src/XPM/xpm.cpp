// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "xpm.h"
#include "common/uint256.h"


bool XPM::Common::setupChainParams(ChainParams *params, const char *network)
{
  if (strcmp(network, "main") == 0) {
    // Setup for mainnet
    params->networkId = NetworkIdMain;
    params->magic = 0xE7E5E7E4;
    params->DefaultPort = 9911;
    params->DefaultRPCPort = 9912;

    params->PublicKeyPrefix = 23;

    // TODO: do correct BIP34 checking
    params->BIP34Height = 17;

    {
      // Genesis block
      // TODO: build it correct way
      params->GenesisBlock.header.nVersion = 2;
      params->GenesisBlock.header.hashPrevBlock.SetNull();
      params->GenesisBlock.header.nTime = 1373064429;
      params->GenesisBlock.header.nBits = 0x06000000;
      params->GenesisBlock.header.nNonce = 383;
      params->GenesisBlock.header.bnPrimeChainMultiplier = ((uint64_t) 532541) * (uint64_t)(2 * 3 * 5 * 7 * 11 * 13 * 17 * 19 * 23);

      XPM::Proto::Transaction tx;
      tx.version = 1;
      tx.lockTime = 0;
      tx.txIn.resize(1);
      tx.txOut.resize(1);
      tx.txIn[0].sequence = -1;
      tx.txIn[0].previousOutputHash.SetNull();
      tx.txIn[0].previousOutputIndex = -1;
      xmstream scriptSig;
      serialize(scriptSig, static_cast<uint8_t>(0));
      XPM::serialize(scriptSig, mpz_class(999));
      serialize(scriptSig, static_cast<uint8_t>(0x4C)); // OP_PUSHDATA1
      serialize(scriptSig, std::string("Sunny King - dedicated to Satoshi Nakamoto and all who have fought for the freedom of mankind"));
      xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);
      tx.txOut[0].value = 100000000;
      params->GenesisBlock.vtx.emplace_back(std::move(tx));
      params->GenesisBlock.header.hashMerkleRoot = calculateBlockMerkleRoot(params->GenesisBlock);
      genesis_block_hash_assert_eq<XPM::X>(params->GenesisBlock.header, "963d17ba4dc753138078a2f56afb3af9674e2546822badff26837db9a0152106");
    }

    params->minimalChainLength = 6;

    // DNS seeds
    params->DNSSeeds.assign({
      "seed.primecoin.info",
      "primeseed.muuttuja.org",
      "seed.primecoin.org",
      "xpm.dnsseed.coinsforall.io"
    });

  } else if (strcmp(network, "testnet") == 0) {
    // Setup for testnet
    params->networkId = NetworkIdTestnet;
    params->magic = 0xC3CBFEFB;
    params->DefaultPort = 9913;
    params->DefaultRPCPort = 9914;

    params->PublicKeyPrefix = 111;

    // TODO: do correct BIP34 checking
    params->BIP34Height = 17;

    {
      // Genesis block
      // TODO: build it correct way
      params->GenesisBlock.header.nVersion = 2;
      params->GenesisBlock.header.hashPrevBlock.SetNull();
      params->GenesisBlock.header.nTime = 1373063882;
      params->GenesisBlock.header.nBits = 0x06000000;
      params->GenesisBlock.header.nNonce = 1513;
      params->GenesisBlock.header.bnPrimeChainMultiplier = ((uint64_t) 585641) * (uint64_t)(2 * 3 * 5 * 7 * 11 * 13 * 17 * 19 * 23);

      XPM::Proto::Transaction tx;
      tx.version = 1;
      tx.lockTime = 0;
      tx.txIn.resize(1);
      tx.txOut.resize(1);
      xmstream scriptSig;
      tx.txIn[0].sequence = -1;
      tx.txIn[0].previousOutputHash.SetNull();
      tx.txIn[0].previousOutputIndex = -1;
      serialize(scriptSig, static_cast<uint8_t>(0));
      XPM::serialize(scriptSig, mpz_class(999));
      serialize(scriptSig, static_cast<uint8_t>(0x4C)); // OP_PUSHDATA1
      serialize(scriptSig, std::string("Sunny King - dedicated to Satoshi Nakamoto and all who have fought for the freedom of mankind"));
      xvectorFromStream(std::move(scriptSig), tx.txIn[0].scriptSig);
      tx.txOut[0].value = 100000000;
      params->GenesisBlock.vtx.emplace_back(std::move(tx));
      params->GenesisBlock.header.hashMerkleRoot = calculateBlockMerkleRoot(params->GenesisBlock);
      genesis_block_hash_assert_eq<XPM::X>(params->GenesisBlock.header, "221156cf301bc3585e72de34fe1efdb6fbd703bc27cfc468faa1cdd889d0efa0");
    }

    params->minimalChainLength = 2;

    // DNS seeds
    params->DNSSeeds.assign({
      "testseed.primecoin.info",
      "primeseedtn.muuttuja.org",
      "seed.testnet.primecoin.org",
      "xpmtestnet.dnsseed.coinsforall.io"
    });
  } else {
    return false;
  }

  return true;
}

// header.nBits is a fixed point number xxxxxxxx.yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy where is
//   y: fractional part (low 24 bit), minimal unit is 0.000000059604644775390625
//   x: rational part (high 8 bit)

// Initialization
void XPM::Common::initialize()
{
}

// XPM Proof of work
static constexpr int nFractionalBits = 24;
static constexpr unsigned TARGET_FRACTIONAL_MASK = (1u<<nFractionalBits) - 1;
static constexpr unsigned TARGET_LENGTH_MASK = ~TARGET_FRACTIONAL_MASK;
static constexpr uint64_t nFractionalDifficultyMax = (1llu << (nFractionalBits + 32));
static constexpr uint64_t nFractionalDifficultyMin = (1llu << 32);
static constexpr uint64_t nFractionalDifficultyThreshold = (1llu << (8 + 32));
static constexpr unsigned nWorkTransitionRatio = 32;

static inline unsigned int TargetGetLength(unsigned int nBits)
{
  return ((nBits & TARGET_LENGTH_MASK) >> nFractionalBits);
}

static inline unsigned int TargetGetFractional(unsigned int nBits)
{
    return (nBits & TARGET_FRACTIONAL_MASK);
}

/// Returns value of hyperbolic function of difficulty fractional part
/// range:
///   f(0.000000000000000000000000) = 2^32 = 4294967296
///   f(0.999999940395355224609375) = 2^56 = 72057594037927936
uint64_t TargetGetFractionalDifficulty(unsigned int nBits)
{
  return nFractionalDifficultyMax / static_cast<uint64_t>((1llu << nFractionalBits) - TargetGetFractional(nBits));
}

/// 6.000 256
/// 6.100 283
/// 6.500 496
/// 6.990 6253
/// 6.999 7945
/// 7.000 8192
/// 7.500 15887
/// 7.999 254261
/// 8.000 262144
arith_uint256 XPM::Common::GetBlockProof(const XPM::Proto::BlockHeader &header, const XPM::Common::ChainParams &chainParams)
{
  uint256 result;
  uint64_t nFractionalDifficulty = TargetGetFractionalDifficulty(header.nBits);
  mpz_class bnWork = 256;

  for (unsigned int nCount = chainParams.minimalChainLength; nCount < TargetGetLength(header.nBits); nCount++)
      bnWork *= nWorkTransitionRatio;

  // TODO: don't use bignum here
  // arith_uint256 can support multiplication and division too
  bnWork *= ((uint64_t) nWorkTransitionRatio) * nFractionalDifficulty;
  bnWork /= (((uint64_t) nWorkTransitionRatio - 1) * nFractionalDifficultyMin + nFractionalDifficulty);
  uint256FromBN(result, bnWork.get_mpz_t());
  return UintToArith256(result);
}


static inline unsigned int TargetFromInt(unsigned int nLength)
{
    return (nLength << nFractionalBits);
}

static uint32_t fractionalPart( XPM::Common::CheckConsensusCtx &ctx)
{
  // Calculate fractional length
  // ctx.exp = ((ctx.bn - ctx.result) << nFractionalBits) / ctx.bn
  mpz_sub(ctx.exp, ctx.bn, ctx.FermatResult);
  mpz_mul_2exp(ctx.exp, ctx.exp, nFractionalBits);
  mpz_div(ctx.exp, ctx.exp, ctx.bn);
  return static_cast<uint32_t>(mpz_get_ui(ctx.exp)) & ((1 << nFractionalBits) - 1);
}

static inline double getPrimeDifficulty(unsigned int nBits)
{
    return ((double) nBits / (double) (1 << nFractionalBits));
}

static bool FermatProbablePrimalityTest(XPM::Common::CheckConsensusCtx &ctx)
{
  // FermatResult = 2^(bn - 1) mod bn
  mpz_sub_ui(ctx.exp, ctx.bn, 1);
  mpz_powm(ctx.FermatResult, ctx.two, ctx.exp, ctx.bn);
  return mpz_cmp_ui(ctx.FermatResult, 1) == 0;
}

static bool EulerLagrangeLifchitzPrimalityTest(XPM::Common::CheckConsensusCtx &ctx, bool isSophieGermain)
{
  // EulerResult = 2^((bn - 1)/2) mod bn
  mpz_sub_ui(ctx.exp, ctx.bn, 1);
  mpz_fdiv_q_2exp(ctx.exp, ctx.exp, 1);
  mpz_powm(ctx.EulerResult, ctx.two, ctx.exp, ctx.bn);

  // FermatResult = (EulerResult ^ 2) mod bn = 2^(bn - 1) mod bn
  mpz_mul(ctx.FermatResult, ctx.EulerResult, ctx.EulerResult);
  mpz_mod(ctx.FermatResult, ctx.FermatResult, ctx.bn);

  auto mod8 = mpz_get_ui(ctx.bn) % 8;
  bool passedTest = false;
  if (isSophieGermain && (mod8 == 7)) {
    passedTest = mpz_cmp_ui(ctx.EulerResult, 1) == 0;
  } else if (isSophieGermain && (mod8 == 3)) {
    mpz_add_ui(ctx.exp, ctx.EulerResult, 1);
    passedTest = mpz_cmp(ctx.exp, ctx.bn) == 0;
  } else if ((!isSophieGermain) && (mod8 == 5)) {
    mpz_add_ui(ctx.exp, ctx.EulerResult, 1);
    passedTest = mpz_cmp(ctx.exp, ctx.bn) == 0;
  } else if ((!isSophieGermain) && (mod8 == 1)) {
    passedTest = mpz_cmp_ui(ctx.EulerResult, 1) == 0;
  }

  return passedTest;
}

static unsigned primeChainLength(XPM::Common::CheckConsensusCtx &ctx, bool isSophieGermain)
{
  if (!FermatProbablePrimalityTest(ctx))
    return fractionalPart(ctx);

  uint32_t rationalPart = 0;
  while (true) {
    rationalPart++;

    // Calculate next number in Cunningham chain
    // 1CC (Sophie Germain) bn = bn*2 + 1
    // 2CC bn = bn*2 - 1
    mpz_mul_2exp(ctx.bn, ctx.bn, 1);
    if (isSophieGermain)
      mpz_add_ui(ctx.bn, ctx.bn, 1);
    else
      mpz_sub_ui(ctx.bn, ctx.bn, 1);

    bool EulerTestPassed = EulerLagrangeLifchitzPrimalityTest(ctx, isSophieGermain);
    bool FermatTestPassed = mpz_cmp_ui(ctx.FermatResult, 1) == 0;
    if (EulerTestPassed != FermatTestPassed) {
      // Euler-Lagrange-Lifchitz and Fermat tests gives different results!
      return 0;
    }

    if (!EulerTestPassed)
      return (rationalPart << nFractionalBits) | fractionalPart(ctx);
  }
}

/// Get length of chain type 1 / Sophie Germain (n, 2n+1, ...)
static inline uint32_t c1Length(XPM::Common::CheckConsensusCtx &ctx)
{
  mpz_sub_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
  return primeChainLength(ctx, true);
}

/// Get length of chain type 2 (n, 2n-1, ...)
static inline uint32_t c2Length(XPM::Common::CheckConsensusCtx &ctx)
{
  mpz_add_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
  return primeChainLength(ctx, false);
}

/// Get length of bitwin chain
static inline uint32_t bitwinLength(uint32_t l1, uint32_t l2)
{
  return TargetGetLength(l1) > TargetGetLength(l2) ?
    (l2 + TargetFromInt(TargetGetLength(l2)+1)) :
    (l1 + TargetFromInt(TargetGetLength(l1)));
}

void XPM::Common::checkConsensusInitialize(CheckConsensusCtx &ctx)
{
  mpz_init(ctx.bnPrimeChainOrigin);
  mpz_init(ctx.bn);
  mpz_init(ctx.exp);
  mpz_init(ctx.EulerResult);
  mpz_init(ctx.FermatResult);
  mpz_init(ctx.two);
  mpz_set_ui(ctx.two, 2);
}

bool XPM::Common::checkConsensus(const Proto::BlockHeader &header, XPM::Common::CheckConsensusCtx &ctx, BC::Common::ChainParams &chainParams)
{
  // Check target
  if (TargetGetLength(header.nBits) < chainParams.minimalChainLength || TargetGetLength(header.nBits) > 99)
    return false;

  XPM::Proto::BlockHashTy hash = header.GetOriginalHeaderHash();

  {
    // Check header hash limit (most significant bit of hash must be 1)
    uint8_t hiByte = *(hash.begin() + 31);
    if (!(hiByte & 0x80))
      return false;
  }

  // Check target for prime proof-of-work
  uint256ToBN(ctx.bnPrimeChainOrigin, hash);
  mpz_mul(ctx.bnPrimeChainOrigin, ctx.bnPrimeChainOrigin, header.bnPrimeChainMultiplier.get_mpz_t());

  auto bnPrimeChainOriginBitSize = mpz_sizeinbase(ctx.bnPrimeChainOrigin, 2);
  if (bnPrimeChainOriginBitSize < 255)
    return false;
  if (bnPrimeChainOriginBitSize > 2000)
    return false;

  uint32_t l1 = c1Length(ctx);
  uint32_t l2 = c2Length(ctx);
  uint32_t lbitwin = bitwinLength(l1, l2);
  if (!(l1 >= header.nBits || l2 >= header.nBits || lbitwin >= header.nBits))
    return false;

  uint32_t chainLength;
  chainLength = std::max(l1, l2);
  chainLength = std::max(chainLength, lbitwin);

  // Check that the certificate (bnPrimeChainMultiplier) is normalized
  if (mpz_get_ui(header.bnPrimeChainMultiplier.get_mpz_t()) % 2 == 0 && mpz_get_ui(ctx.bnPrimeChainOrigin) % 4 == 0) {
     mpz_fdiv_q_2exp(ctx.bnPrimeChainOrigin, ctx.bnPrimeChainOrigin, 1);

     // Calculate extended C1, C2 & bitwin chain lengths
     mpz_sub_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
     uint32_t l1Extended = FermatProbablePrimalityTest(ctx) ? l1 + (1 << nFractionalBits) : fractionalPart(ctx);
     mpz_add_ui(ctx.bn, ctx.bnPrimeChainOrigin, 1);
     uint32_t l2Extended = FermatProbablePrimalityTest(ctx) ? l2 + (1 << nFractionalBits) : fractionalPart(ctx);
     uint32_t lbitwinExtended = bitwinLength(l1Extended, l2Extended);
     if (l1Extended > chainLength || l2Extended > chainLength || lbitwinExtended > chainLength)
       return false;
  }

  // TODO: store chain length and type (for what?)
  return true;
}

unsigned XPM::Common::checkBlockStandalone(const Proto::Block &block, const ChainParams &chainParams, std::string &error)
{
  bool isValid = true;
  applyValidation(validateBlockSize<XPM::X>, block, chainParams, error, &isValid);
  applyValidation(validateMerkleRoot<XPM::X>, block, chainParams, error, &isValid);
  return isValid;
}

bool XPM::Common::checkBlockContextual(const BlockIndex &index, const Proto::Block &block, const ChainParams &chainParams, std::string &error)
{
  bool isValid = true;
  applyContextualValidation(validateBIP34<XPM::X>, index, block, chainParams, error, &isValid);
  return isValid;
}
