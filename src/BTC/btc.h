#pragma once

#include "proto.h"
#include "scriptFlags.h"
#include "validation.h"
#include <openssl/sha.h>
#include <string.h>
#include <stdint.h>
#include <asyncio/asyncioTypes.h>

#include "blockIndex.h"
#include "../loguru.hpp"

namespace BTC {

namespace DB {
class UTXODb;
}

class Configuration {
public:
  static constexpr size_t MaxBlockSize = 1000000;
  // Consensus: MAX_BLOCK_SIGOPS_COST / WITNESS_SCALE_FACTOR, in legacy sigops
  static constexpr unsigned MaxBlockSigOps = 20000;
  static constexpr uint32_t BlocksFileLimit = 128*1048576;
  static constexpr size_t DefaultBlockCacheSize = 512*1048576;
  static constexpr uint64_t RationalPartSize = 100000000ULL;
  // Consensus: Core's MAX_MONEY
  static constexpr int64_t MaxMoney = 21000000LL * 100000000LL;
  // Mempool policy, Bitcoin Core 31's defaults; fees per 1000 bytes
  static constexpr int64_t MinRelayTxFee = 100;
  static constexpr int64_t DustRelayTxFee = 3000;
  // Bitcoin Core rounds a fee up
  static constexpr bool FeeRoundsUp = true;
  static constexpr int32_t MaxStandardTxVersion = 3;
  static constexpr uint32_t MinStandardTxNonWitnessSize = 65;
  static constexpr size_t MaxStandardScriptSigSize = 1650;
  // Every data output together: the standard weight in vbytes (-datacarriersize)
  static constexpr size_t MaxDataCarrierBytes = 100000;
  // BIP54's limit of legacy sigops, a policy until it activates
  static constexpr unsigned MaxTxLegacySigOps = 2500;
  // Clusters are bounded instead of ancestors and descendants
  static constexpr size_t MempoolClusterLimit = 64;
  static constexpr uint64_t MempoolClusterSizeLimit = 101000;
  // The mempool's script flags: Core's MANDATORY_SCRIPT_VERIFY_FLAGS and STANDARD_SCRIPT_VERIFY_FLAGS
  static constexpr uint32_t MandatoryScriptFlags =
    EScriptP2SH | EScriptDerSig | EScriptNullDummy | EScriptCheckLockTimeVerify | EScriptCheckSequenceVerify |
    EScriptWitness | EScriptTaproot;
  static constexpr uint32_t StandardScriptFlags =
    MandatoryScriptFlags | EScriptStrictEnc | EScriptMinimalData | EScriptDiscourageUpgradableNops | EScriptCleanStack |
    EScriptMinimalIf | EScriptNullFail | EScriptLowS | EScriptDiscourageUpgradableWitnessProgram |
    EScriptWitnessPubKeyType | EScriptConstScriptCode | EScriptDiscourageUpgradableTaprootVersion |
    EScriptDiscourageOpSuccess | EScriptDiscourageUpgradablePubKeyType;

  static constexpr bool HasWitness = true;
  static constexpr uint32_t ProtocolVersion = 70015;
  static constexpr uint64_t ServicesEnabled =
    static_cast<uint64_t>(BTC::Proto::EServices::Network) |
    static_cast<uint64_t>(BTC::Proto::EServices::Witness);

  static constexpr const char *ProjectName = "Bitcoin";
  static constexpr const char *TickerName = "BTC";
  static constexpr const char *DefaultDataDir = "bcnodebtc";
  static constexpr const char *UserAgent = "/bcnode/btc-0.1/";
};

namespace Common {
  enum NetwordIdTy {
    NetworkIdMain = 0,
    NetworkIdTestnet,
    NetworkIdRegtest
  };


  template<typename T>
  struct ChainParamsTy {
    int networkId;
    uint32_t magic;
    // The coin's own block type: LTC's is not the generic BlockTy the others alias
    typename T::CBlock GenesisBlock;
    // Whether the databases have to see the genesis block. No feed carries it - the engine
    // puts it into the index ready-made - so its outputs reach nothing unless they are
    // connected here. Bitcoin's genesis coinbase is unspendable by consensus and stays out;
    // a chain that premines into a spendable output cannot
    bool ConnectGenesis = false;

    // Soft&hard forks
    uint32_t BIP34Height;
    uint32_t SegwitHeight;
    // Script rules by height, for the coin's blockScriptFlags: P2SH, DERSIG, CHECKLOCKTIMEVERIFY,
    // CHECKSEQUENCEVERIFY, taproot. A coin sets the ones its function reads
    uint32_t BIP16Height = 0;
    uint32_t BIP66Height = NeverHeight;
    uint32_t BIP65Height = NeverHeight;
    uint32_t CSVHeight = NeverHeight;
    uint32_t TaprootHeight = NeverHeight;
    std::vector<CScriptFlagException> ScriptFlagExceptions;

    // Blocks that repeat an earlier coinbase transaction (see CBIP30Repeat);
    // empty on every chain but Bitcoin
    std::vector<CBIP30Repeat> BIP30Repeats;

    // Prefixes
    std::vector<uint8_t> PublicKeyPrefix;
    std::vector<uint8_t> ScriptPrefix;
    std::vector<uint8_t> SecretKeyPrefix;
    // Segwit address HRP (BIP173); empty on chains without segwit
    std::string Bech32Prefix;

    // Network
    uint16_t DefaultPort;
    uint16_t DefaultRPCPort;
    std::vector<const char*> DNSSeeds;

    // ...
    UInt<256> powLimit;
  };

  struct CheckConsensusCtx {};

  using BlockIndex = BlockIndexTy<BTC::Proto>;
  using ChainParams = ChainParamsTy<BTC::Proto>;

  bool setupChainParams(ChainParams *params, const char *network);
  static inline bool hasWitness() { return true; }
  // Blocks a coinbase output waits before it may be spent (Core's COINBASE_MATURITY)
  static inline uint32_t coinbaseMaturity(const ChainParams&, uint32_t) { return 100; }

  UInt<256> GetBlockProof(const BTC::Proto::CBlockHeader &header, const ChainParams &chainParams);

  // Check functions
  static inline void checkConsensusInitialize(CheckConsensusCtx&) {}
  bool checkConsensus(const Proto::CBlockHeader &header, CheckConsensusCtx &ctx, ChainParams &chainParams);
  // Group check: the pipeline hands over a whole chunk so a chain with a multi-way hash kernel
  // can use it. Here the group is just walked
  static inline void checkConsensusMulti(const Proto::CBlockHeader *const *headers,
                                         size_t count,
                                         CheckConsensusCtx &ctx,
                                         ChainParams &chainParams,
                                         bool *results) {
    for (size_t i = 0; i < count; i++)
      results[i] = checkConsensus(*headers[i], ctx, chainParams);
  }

  static inline void initializeValidationContext(const Proto::CBlock &block, Proto::CBlockValidationData &ctx) { BTC::validationDataInitialize(block, ctx); }

  bool checkBlockStandalone(const Proto::CBlock &block,
                            Proto::CBlockValidationData &validation,
                            const ChainParams &chainParams,
                            std::string &error);
  // Validation data is non-const: this is where a block learns the consensus
  // exemptions its place in the chain grants it
  bool checkBlockContextual(const BlockIndex &index,
                            const Proto::CBlock &block,
                            Proto::CBlockValidationData &validation,
                            const Proto::CBlockLinkedOutputs &linkedOutputs,
                            const ChainParams &chainParams,
                            std::string &error);

  // A transaction alone: Core's CheckTransaction
  bool checkTransactionStandalone(const Proto::CTransaction &tx, const ChainParams &chainParams, std::string &error);
  // With the outputs it spends, for the block it would enter: CheckTxInputs, IsFinalTx and
  // SequenceLocks. Sets the fee once the input values pass
  bool checkTransactionContextual(const Proto::CTransaction &tx,
                                  const CPrevout *prevouts,
                                  const CTxContext &context,
                                  const ChainParams &chainParams,
                                  int64_t &fee,
                                  std::string &error);
  // Mempool policy on the transaction alone and with the outputs it spends: what this coin's node
  // asks beyond consensus (BTC/policy.h)
  bool checkPolicyStandalone(const Proto::CTransaction &tx, const CTxCost &cost, std::string &error);
  bool checkPolicyContextual(const Proto::CTransaction &tx,
                             const CPrevout *prevouts,
                             const CTxContext &context,
                             const CTxCost &cost,
                             int64_t fee,
                             std::string &error);
  // The longest transaction checkPolicyStandalone can pass: the network drops longer ones unparsed
  size_t maxStandardTxSize();

  // Scripts (BTC/scriptVerify.h). The flags a block's scripts are checked under: Core's
  // GetBlockScriptFlags
  uint32_t blockScriptFlags(const BlockIndex &index, const ChainParams &chainParams);
  // What the engine asks of a block: transactions [firstTx, lastTx) against the linked outputs of
  // their inputs - a wave hands a block out in parts. signatures: verified, or taken on trust with
  // the other rules checked; skip: transactions checked already (scriptTxSkipped), or nullptr.
  // False at an invalid input, named in the result
  bool checkBlockScripts(const BlockIndex &index,
                         const Proto::CBlock &block,
                         const Proto::CBlockLinkedOutputs &linkedOutputs,
                         const ChainParams &chainParams,
                         size_t firstTx,
                         size_t lastTx,
                         bool signatures,
                         const uint64_t *skip,
                         CScriptCheckResult &result);
  // The mempool's: under this coin's standard flags, and its mandatory ones for the reason
  EScriptRefusal checkPolicyScripts(const Proto::CTransaction &tx,
                                    std::span<const Script::CUnspentOutputInfo* const> spent,
                                    std::string &error,
                                    CScriptCount &count);
}

class X {
public:
  using BlockIndex = BTC::Common::BlockIndex;
  using ChainParams = BTC::Common::ChainParams;
  using Configuration = BTC::Configuration;
  using Proto = BTC::Proto;
  using UTXODb = BTC::DB::UTXODb;
  template<typename T> using Io = BTC::Io<T>;
};
}
