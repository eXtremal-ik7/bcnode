// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "serializedDataCache.h"

SerializedDataObject::~SerializedDataObject()
{
  operator delete(Data_);
  operator delete(UnpackedData_);
  Parent_->remove(this);
}
