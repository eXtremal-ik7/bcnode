#include "proto.h"

size_t BTC::Io<DOGE::Proto::BlockHeader>::getSerializedSize(const DOGE::Proto::BlockHeader &data)
{
  size_t result = BTC::Io<DOGE::Proto::PureBlockHeader>::getSerializedSize(data);
  if (data.nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW) {
    result += BTC::Io<decltype (data.ParentBlockCoinbaseTx)>::getSerializedSize(data.ParentBlockCoinbaseTx);
    result += BTC::Io<decltype (data.HashBlock)>::getSerializedSize(data.HashBlock);
    result += BTC::Io<decltype (data.MerkleBranch)>::getSerializedSize(data.MerkleBranch);
    result += BTC::Io<decltype (data.Index)>::getSerializedSize(data.Index);
    result += BTC::Io<decltype (data.ChainMerkleBranch)>::getSerializedSize(data.ChainMerkleBranch);
    result += BTC::Io<decltype (data.ChainIndex)>::getSerializedSize(data.ChainIndex);
    result += BTC::Io<decltype (data.ParentBlock)>::getSerializedSize(data.ParentBlock);
  }

  return result;
}

size_t BTC::Io<DOGE::Proto::BlockHeader>::getUnpackedExtraSize(xmstream &src)
{
  size_t result = 0;
  int32_t nVersion;
  BTC::unserialize(src, nVersion);
  src.seek(sizeof(DOGE::Proto::PureBlockHeader) - sizeof(nVersion));

  if (nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW) {
    result += BTC::Io<decltype (DOGE::Proto::BlockHeader::ParentBlockCoinbaseTx)>::getUnpackedExtraSize(src);
    src.seek(sizeof(uint256));
    result += BTC::Io<decltype (DOGE::Proto::BlockHeader::MerkleBranch)>::getUnpackedExtraSize(src);
    src.seek(sizeof(int));
    result += BTC::Io<decltype (DOGE::Proto::BlockHeader::ChainMerkleBranch)>::getUnpackedExtraSize(src);
    src.seek(sizeof(int));
    result += BTC::Io<decltype (DOGE::Proto::BlockHeader::ParentBlock)>::getUnpackedExtraSize(src);
  }

  return result;
}

void BTC::Io<DOGE::Proto::BlockHeader>::serialize(xmstream &dst, const DOGE::Proto::BlockHeader &data)
{
  BTC::serialize(dst, *(DOGE::Proto::PureBlockHeader*)&data);
  if (data.nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW) {
    BTC::serialize(dst, data.ParentBlockCoinbaseTx);

    BTC::serialize(dst, data.HashBlock);
    BTC::serialize(dst, data.MerkleBranch);
    BTC::serialize(dst, data.Index);

    BTC::serialize(dst, data.ChainMerkleBranch);
    BTC::serialize(dst, data.ChainIndex);
    BTC::serialize(dst, data.ParentBlock);
  }
}

void BTC::Io<DOGE::Proto::BlockHeader>::unserialize(xmstream &src, DOGE::Proto::BlockHeader &data)
{
  BTC::unserialize(src, *(DOGE::Proto::PureBlockHeader*)&data);
  if (data.nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW) {
    BTC::unserialize(src, data.ParentBlockCoinbaseTx);
    BTC::unserialize(src, data.HashBlock);
    BTC::unserialize(src, data.MerkleBranch);
    BTC::unserialize(src, data.Index);
    BTC::unserialize(src, data.ChainMerkleBranch);
    BTC::unserialize(src, data.ChainIndex);
    BTC::unserialize(src, data.ParentBlock);
  }
}

void BTC::Io<DOGE::Proto::BlockHeader>::unpack2(xmstream &src, DOGE::Proto::BlockHeader *data, uint8_t **extraData)
{
  new (data) DOGE::Proto::BlockHeader;
  BTC::unserialize(src, *(DOGE::Proto::PureBlockHeader*)data);
  if (data->nVersion & DOGE::Proto::BlockHeader::VERSION_AUXPOW) {
    BTC::Io<decltype (data->ParentBlockCoinbaseTx)>::unpack2(src, &data->ParentBlockCoinbaseTx, extraData);
    BTC::Io<decltype (data->HashBlock)>::unpack2(src, &data->HashBlock, extraData);
    BTC::Io<decltype (data->MerkleBranch)>::unpack2(src, &data->MerkleBranch, extraData);
    BTC::unserialize(src, data->Index);
    BTC::Io<decltype (data->ChainMerkleBranch)>::unpack2(src, &data->ChainMerkleBranch, extraData);
    BTC::unserialize(src, data->ChainIndex);
    BTC::Io<decltype (data->ParentBlock)>::unpack2(src, &data->ParentBlock, extraData);
  }
}
