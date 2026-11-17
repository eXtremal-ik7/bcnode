#include "sighash.h"
#include <string.h>

namespace BTC {

void keyHashScript(const uint8_t *hash, uint8_t (&script)[25])
{
  script[0] = Script::OP_DUP;
  script[1] = Script::OP_HASH160;
  script[2] = Script::OP_PUSH20;
  memcpy(script + 3, hash, 20);
  script[23] = Script::OP_EQUALVERIFY;
  script[24] = Script::OP_CHECKSIG;
}

void spentDigests(std::span<const Script::CUnspentOutputInfo* const> spent, CTxDigests &digests)
{
  CSha256Writer amounts;
  CSha256Writer scripts;
  xvector<uint8_t> script;
  for (const Script::CUnspentOutputInfo *info: spent) {
    amounts.le64(static_cast<uint64_t>(info->Value));
    // An OP_RETURN record keeps no script, but no input can spend one
    if (!Script::restoreScript(*info, script))
      script.resize(0);
    scripts.script(script.data(), script.size());
  }
  digests.SpentAmounts = amounts.sha256();
  digests.SpentScripts = scripts.sha256();
}

BaseBlob<256> tagHash(const char *tag)
{
  BaseBlob<256> hash;
  ::sha256(tag, strlen(tag), hash.begin());
  return hash;
}

const BaseBlob<256> &tapSighashTag()
{
  static const BaseBlob<256> tag = tagHash("TapSighash");
  return tag;
}

}
