#include "validation.h"
#include "common/merkleTree.h"

static const unsigned char pchMergedMiningHeader[] = { 0xfa, 0xbe, 'm', 'm' };

static uint32_t getExpectedIndex(uint32_t nNonce, int nChainId, unsigned h)
{
  uint32_t rand = nNonce;
  rand = rand * 1103515245 + 12345;
  rand += nChainId;
  rand = rand * 1103515245 + 12345;

  return rand % (1 << h);
}

bool validateAuxPow(const DOGE::Proto::Block &block, const DOGE::Common::ChainParams &chainParams, std::string &error)
{
  if (!(block.header.nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW))
    return true;

  if (block.header.Index != 0) {
    error = "AuxPow is not a generate";
    return false;
  }

  uint32_t chainId = block.header.nVersion >> 16;
  uint32_t parentChainId = block.header.ParentBlock.nVersion >> 16;

  if (chainParams.StrictChainId && parentChainId == chainId) {
    error = "Aux POW parent has our chain ID";
    return false;
  }

  if (block.header.ChainMerkleBranch.size() > 30) {
    error = "Aux POW chain merkle branch too long";
    return false;
  }

  // Check parent block merkle tree
  {
    uint256 parentBlockCoinbaseTxHash = block.header.ParentBlockCoinbaseTx.getTxId();
    if (calculateMerkleRoot(parentBlockCoinbaseTxHash, &block.header.MerkleBranch[0], block.header.MerkleBranch.size(), 0) != block.header.ParentBlock.hashMerkleRoot) {
      error = "Aux POW merkle root incorrect";
      return false;
    }
  }

  // Check parent block's coinbase txin format
  uint256 chainMerkleRoot =
    calculateMerkleRoot(block.header.GetHash(), &block.header.ChainMerkleBranch[0], block.header.ChainMerkleBranch.size(), block.header.ChainIndex);
  std::reverse(chainMerkleRoot.begin(), chainMerkleRoot.end());

  auto &parentCoinbaseScript = block.header.ParentBlockCoinbaseTx.txIn[0].scriptSig;
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

  auto chainMerkleTreeSizePos = chainMerkleRootPos + sizeof(uint256);
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

  if (chainMerkleTreeSize != (1u << block.header.ChainMerkleBranch.size())) {
    error = "Aux POW merkle branch size does not match parent coinbase";
    return false;
  }

  if (block.header.ChainIndex != getExpectedIndex(extraNoncePart, chainId, block.header.ChainMerkleBranch.size())) {
    error = "Aux POW wrong index";
    return false;
  }

  return true;
}
