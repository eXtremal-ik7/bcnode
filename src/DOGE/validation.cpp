#include "validation.h"
#include "BTC/merkleTree.h"

static const unsigned char pchMergedMiningHeader[] = { 0xfa, 0xbe, 'm', 'm' };

static uint32_t getExpectedIndex(uint32_t nNonce, int nChainId, unsigned h)
{
  uint32_t rand = nNonce;
  rand = rand * 1103515245 + 12345;
  rand += nChainId;
  rand = rand * 1103515245 + 12345;

  return rand % (1 << h);
}

bool validateAuxPow(const DOGE::Proto::CBlock &block, const DOGE::Common::ChainParams &chainParams, std::string &error)
{
  if (!(block.Header.Version & DOGE::Proto::CBlockHeader::VERSION_AUXPOW))
    return true;

  if (block.Header.Index != 0) {
    error = "AuxPow is not a generate";
    return false;
  }

  uint32_t chainId = block.Header.Version >> 16;
  uint32_t parentChainId = block.Header.ParentBlock.Version >> 16;

  if (chainParams.StrictChainId && parentChainId == chainId) {
    error = "Aux POW parent has our chain ID";
    return false;
  }

  if (block.Header.ChainMerkleBranch.size() > 30) {
    error = "Aux POW chain merkle branch too long";
    return false;
  }

  // Check parent block merkle tree
  {
    BaseBlob<256> parentBlockCoinbaseTxHash = block.Header.ParentBlockCoinbaseTx.getTxId();
    if (BTC::calculateMerkleRoot(parentBlockCoinbaseTxHash, &block.Header.MerkleBranch[0], block.Header.MerkleBranch.size(), 0) != block.Header.ParentBlock.HashMerkleRoot) {
      error = "Aux POW merkle root incorrect";
      return false;
    }
  }

  // Check parent block's coinbase txin format
  BaseBlob<256> chainMerkleRoot =
    BTC::calculateMerkleRoot(block.Header.GetHash(), &block.Header.ChainMerkleBranch[0], block.Header.ChainMerkleBranch.size(), block.Header.ChainIndex);
  std::reverse(chainMerkleRoot.begin(), chainMerkleRoot.end());

  auto &parentCoinbaseScript = block.Header.ParentBlockCoinbaseTx.TxIn[0].ScriptSig;
  auto chainMerkleRootPos = std::search(parentCoinbaseScript.begin(), parentCoinbaseScript.end(), chainMerkleRoot.begin(), chainMerkleRoot.end());
  auto mergedMiningHeaderPos = std::search(parentCoinbaseScript.begin(), parentCoinbaseScript.end(), pchMergedMiningHeader, pchMergedMiningHeader+sizeof(pchMergedMiningHeader));

  if (chainMerkleRootPos == parentCoinbaseScript.end()) {
    error = "Aux POW missing chain merkle root in parent coinbase";
    return false;
  }

  if (mergedMiningHeaderPos != parentCoinbaseScript.end()) {
    if (std::search(mergedMiningHeaderPos+1, parentCoinbaseScript.end(), pchMergedMiningHeader, pchMergedMiningHeader+sizeof(pchMergedMiningHeader)) != parentCoinbaseScript.end()) {
      error = "Multiple merged mining headers in coinbase";
      return false;
    }

    if (mergedMiningHeaderPos + sizeof(pchMergedMiningHeader) != chainMerkleRootPos) {
      error = "Merged mining header is not just before chain merkle root";
      return false;
    }
  } else {
    if (chainMerkleRootPos - parentCoinbaseScript.begin() > 20) {
      error = "Aux POW chain merkle root must start in the first 20 bytes of the parent coinbase";
      return false;
    }
  }

  auto chainMerkleTreeSizePos = chainMerkleRootPos + chainMerkleRoot.size();
  if (parentCoinbaseScript.end() - chainMerkleTreeSizePos < 8) {
    error = "Aux POW missing chain merkle tree size and nonce in parent coinbase";
    return false;
  }

  uint32_t chainMerkleTreeSize = 0;
  uint32_t extraNoncePart = 0;
  memcpy(&chainMerkleTreeSize, chainMerkleTreeSizePos, 4);
  memcpy(&extraNoncePart, chainMerkleTreeSizePos+4, 4);
  chainMerkleTreeSize = xletoh(chainMerkleTreeSize);
  extraNoncePart = xletoh(extraNoncePart);

  if (chainMerkleTreeSize != (1u << block.Header.ChainMerkleBranch.size())) {
    error = "Aux POW merkle branch size does not match parent coinbase";
    return false;
  }

  if (static_cast<uint32_t>(block.Header.ChainIndex) != getExpectedIndex(extraNoncePart, chainId, block.Header.ChainMerkleBranch.size())) {
    error = "Aux POW wrong index";
    return false;
  }

  return true;
}
