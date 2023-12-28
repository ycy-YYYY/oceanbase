//Copyright (c) 2021 OceanBase
// OceanBase is licensed under Mulan PubL v2.
// You can use this software according to the terms and conditions of the Mulan PubL v2.
// You may obtain a copy of Mulan PubL v2 at:
//          http://license.coscl.org.cn/MulanPubL-2.0
// THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
// EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
// MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
// See the Mulan PubL v2 for more details.
#ifndef OB_SHARE_COMPACTION_COMPACTION_TIME_GUARD_H_
#define OB_SHARE_COMPACTION_COMPACTION_TIME_GUARD_H_
#include "share/ob_occam_time_guard.h"
namespace oceanbase
{
namespace compaction
{

class ObCompactionTimeGuard : public common::occam::ObOccamTimeGuard
{
public:
  const static uint64_t WARN_THRESHOLD = 30L * 1000 * 1000; // 30s
  ObCompactionTimeGuard(const uint64_t warn_threshold = WARN_THRESHOLD, const char *mod = "")
    : ObOccamTimeGuard(warn_threshold, nullptr, nullptr, mod),
      add_time_(0)
  {}
  virtual ~ObCompactionTimeGuard() {}
  virtual int64_t to_string(char *buf, const int64_t buf_len) const
  {
    UNUSEDx(buf, buf_len);
    return 0;
  }
  void add_time_guard(const ObCompactionTimeGuard &other);
  ObCompactionTimeGuard & operator=(const ObCompactionTimeGuard &other);
  OB_INLINE bool is_empty() const { return 0 == idx_; }
  // set the dag add_time as the first click time
  OB_INLINE void set_last_click_ts(const int64_t time)
  {
    last_click_ts_ = time;
    add_time_ = time;
  }
  OB_INLINE uint32_t get_specified_cost_time(const int64_t line) const {
    uint32_t ret_val = 0;
    for (int64_t idx = 0; idx < idx_; ++idx) {
      if (line_array_[idx] == line) {
        ret_val = click_poinsts_[idx];
        break;
      }
    }
    return ret_val;
  }

  int64_t add_time_;
};

} // namespace compaction
} // namespace oceanbase

#endif // OB_SHARE_COMPACTION_COMPACTION_TIME_GUARD_H_
