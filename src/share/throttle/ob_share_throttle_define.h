/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEABASE_SHARE_THROTTLE_OB_SHARE_THROTTLE_DEFINE_H
#define OCEABASE_SHARE_THROTTLE_OB_SHARE_THROTTLE_DEFINE_H

#include "lib/alloc/alloc_struct.h"

/**
 * @brief This File is used to avoid introducing circular dependencies. It declares some class but do not include their
 * .h file
 */

#define DEFINE_CUSTOM_FUNC_FOR_THROTTLE(throttle_name)                                                           \
  static const lib::ObLabel throttle_unit_name()                                                                 \
  {                                                                                                              \
    static lib::ObLabel label(#throttle_name);                                                                   \
    return label;                                                                                                \
  }                                                                                                              \
  static int64_t resource_unit_size();                                                                           \
  static void init_throttle_config(int64_t &resource_limit, int64_t &trigger_percentage, int64_t &max_duration); \
  static void adaptive_update_limit(const int64_t holding_size,                                                  \
                                    const int64_t config_specify_resource_limit,                                 \
                                    int64_t &resource_limit,                                                     \
                                    int64_t &last_update_limit_ts,                                               \
                                    bool &is_updated);

#define DEFINE_SHARE_THROTTLE(ThrottleName, ...)      \
                                                      \
  struct FakeAllocatorFor##ThrottleName {             \
    DEFINE_CUSTOM_FUNC_FOR_THROTTLE(ThrottleName);    \
  };                                                  \
                                                      \
  LST_DEFINE(__VA_ARGS__);                            \
                                                      \
  template <typename FakeAllocator, typename... Args> \
  class ObShareResourceThrottleTool;                  \
                                                      \
  using ThrottleName##ThrottleTool = ObShareResourceThrottleTool<FakeAllocatorFor##ThrottleName, __VA_ARGS__>;

namespace oceanbase {

namespace share {
// This Macor will expand as follows :
//
// struct FakeAllocatorForTxShare {
//   static const lib::ObLabel throttle_unit_name()
//   {
//     lib::ObLabel label("TxShare");
//     return label;
//   }
//   static int64_t resource_unit_size();
//   static void init_throttle_config(int64_t &resource_limit, int64_t &trigger_percentage, int64_t &max_duration);
// };
//
// class ObMemstoreAllocator;
// class ObTenantTxDataAllocator;
// class ObTenantMdsAllocator;
//
// template <typename FakeAllocator, typename... Args>
// class ObShareResourceThrottleTool;
//
// using TxShareThrottleTool = ObShareResourceThrottleTool<FakeAllocatorForTxShare,
//                                                         ObMemstoreAllocator,
//                                                         ObTenantTxDataAllocator,
//                                                         ObTenantMdsAllocator>;
DEFINE_SHARE_THROTTLE(TxShare, ObMemstoreAllocator, ObTenantTxDataAllocator, ObTenantMdsAllocator)

}  // namespace share

}  // namespace oceanbase

#endif