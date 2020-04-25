#include "script.h"
#include "openssl/ripemd.h"
#include "openssl/sha.h"

namespace BC {
namespace Script {

bool decodeStandardOutput(const BC::Proto::TxOut &out, BC::Proto::AddressTy &address)
{
  if (out.pkScript.size() == 35 || out.pkScript.size() == 67) {
    // Pay to public key
    uint8_t sha256[32];
    {
      SHA256_CTX ctx;
      SHA256_Init(&ctx);
      SHA256_Update(&ctx, out.pkScript.data() + 1, out.pkScript.size() - 2);
      SHA256_Final(sha256, &ctx);
    }

    {
      RIPEMD160_CTX ctx;
      RIPEMD160_Init(&ctx);
      RIPEMD160_Update(&ctx, sha256, sizeof(sha256));
      RIPEMD160_Final(address.begin(), &ctx);
    }

    return true;
  }


  if (out.pkScript.size() == sizeof(BC::Proto::AddressTy) + 5) {
    // Pay to public key hash
    if (out.pkScript[0] == OP_DUP &&
        out.pkScript[1] == OP_HASH160 &&
        out.pkScript[2] == sizeof(BC::Proto::AddressTy) &&
        out.pkScript[sizeof(BC::Proto::AddressTy) + 3] == OP_EQUALVERIFY &&
        out.pkScript[sizeof(BC::Proto::AddressTy) + 4] == OP_CHECKSIG) {
      memcpy(address.begin(), &out.pkScript[3], sizeof(BC::Proto::AddressTy));
      return true;
    }
  }

  return false;
}

}
}
