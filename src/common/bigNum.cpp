// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bigNum.h"

// void baseBlob256ToBN(mpz_ptr bignum, const BaseBlob<256> &N)
// {
//   mpz_import(bignum, 32 / sizeof(unsigned long), -1, sizeof(unsigned long), -1, 0, N.begin());
// }


// void baseBlob256ToBN(mpz_class &bigNum, const BaseBlob<256> &N)
// {
//   baseBlob256ToBN(bigNum.get_mpz_t(), N);
// }

// void baseBlob256FromBN(BaseBlob<256> &N, mpz_srcptr bigNum)
// {
//   N.setNull();
//   int limbsNum = std::min(static_cast<int>(256/8/sizeof(mp_limb_t)), bigNum->_mp_size);
//   mp_limb_t *out = reinterpret_cast<mp_limb_t*>(N.begin());
//   for (int i = 0; i < limbsNum; i++)
//     out[i] = bigNum->_mp_d[i];
// }
