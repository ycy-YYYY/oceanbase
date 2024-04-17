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

#ifndef DEV_SRC_SQL_DAS_OB_DAS_CONTEXT_H_
#define DEV_SRC_SQL_DAS_OB_DAS_CONTEXT_H_
#include "sql/das/ob_das_define.h"
#include "sql/das/ob_das_location_router.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "sql/das/ob_das_factory.h"
#include "storage/tx/ob_trans_define.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"
namespace oceanbase
{
namespace sql
{
class ObDASTabletMapper;

struct DmlRowkeyDistCtx
{
public:
  DmlRowkeyDistCtx()
    : deleted_rows_(nullptr),
    table_id_(common::OB_INVALID_ID)
  {}
  SeRowkeyDistCtx *deleted_rows_;
  uint64_t table_id_;
};
typedef common::ObList<DmlRowkeyDistCtx, common::ObIAllocator> DASDelCtxList;

class ObDASCtx
{
  OB_UNIS_VERSION(1);
public:
  ObDASCtx(common::ObIAllocator &allocator)
    : table_locs_(allocator),
      external_table_locs_(allocator),
      sql_ctx_(nullptr),
      location_router_(allocator),
      das_factory_(allocator),
      related_tablet_map_(allocator),
      allocator_(allocator),
      snapshot_(),
      savepoint_(),
      write_branch_id_(0),
      del_ctx_list_(allocator),
      jump_read_group_id_(-1),
      flags_(0)
  {
    is_fk_cascading_ = 0;
    need_check_server_ = 1;
    same_server_ = 1;
    iter_uncommitted_row_ = 0;
  }
  ~ObDASCtx()
  {
    // Destroy the hash set list used for checking duplicate rowkey for foreign key cascade delete
    if (!del_ctx_list_.empty()) {
      DASDelCtxList::iterator iter = del_ctx_list_.begin();
      for (; iter != del_ctx_list_.end(); iter++) {
        DmlRowkeyDistCtx& del_ctx = *iter;
        if (del_ctx.deleted_rows_ != nullptr) {
          del_ctx.deleted_rows_->destroy();
          del_ctx.deleted_rows_ = nullptr;
        }
      }
    }
    del_ctx_list_.destroy();
  }

  int init(const ObPhysicalPlan &plan, ObExecContext &ctx);
  ObDASTableLoc *get_table_loc_by_id(uint64_t table_loc_id, uint64_t ref_table_id);
  ObDASTableLoc *get_external_table_loc_by_id(uint64_t table_loc_id, uint64_t ref_table_id);
  DASTableLocList &get_table_loc_list() { return table_locs_; }
  const DASTableLocList &get_table_loc_list() const { return table_locs_; }
  DASDelCtxList& get_das_del_ctx_list() {return  del_ctx_list_;}
  DASTableLocList &get_external_table_loc_list() { return external_table_locs_; }
  int extended_tablet_loc(ObDASTableLoc &table_loc,
                          const common::ObTabletID &tablet_id,
                          ObDASTabletLoc *&tablet_loc,
                          const common::ObObjectID &partition_id = OB_INVALID_ID,
                          const common::ObObjectID &first_level_part_id = OB_INVALID_ID);
  int extended_tablet_loc(ObDASTableLoc &table_loc,
                          const ObCandiTabletLoc &candi_tablet_loc,
                          ObDASTabletLoc *&talet_loc);
  int extended_table_loc(const ObDASTableLocMeta &loc_meta, ObDASTableLoc *&table_loc);
  int add_candi_table_loc(const ObDASTableLocMeta &loc_meta, const ObCandiTableLoc &candi_table_loc);
  int get_das_tablet_mapper(const uint64_t ref_table_id,
                            ObDASTabletMapper &tablet_mapper,
                            const DASTableIDArrayWrap *related_table_ids = nullptr);
  int get_all_lsid(share::ObLSArray &ls_ids);
  int64_t get_related_tablet_cnt() const;
  void set_snapshot(const transaction::ObTxReadSnapshot &snapshot) { snapshot_ = snapshot; }
  transaction::ObTxReadSnapshot &get_snapshot() { return snapshot_; }
  transaction::ObTxSEQ get_savepoint() const { return savepoint_; }
  void set_savepoint(const transaction::ObTxSEQ savepoint) { savepoint_ = savepoint; }
  void set_write_branch_id(const int16_t branch_id) { write_branch_id_ = branch_id; }
  int16_t get_write_branch_id() const { return write_branch_id_; }
  ObDASLocationRouter &get_location_router() { return location_router_; }
  int build_related_tablet_loc(ObDASTabletLoc &tablet_loc);
  int build_related_table_loc(ObDASTableLoc &table_loc);
  int rebuild_tablet_loc_reference();
  void clear_all_location_info()
  {
    table_locs_.clear();
    related_tablet_map_.clear();
    external_table_locs_.clear();
    same_server_ = 1;
  }
  ObDASTaskFactory &get_das_factory() { return das_factory_; }
  void set_sql_ctx(ObSqlCtx *sql_ctx) { sql_ctx_ = sql_ctx; }
  DASRelatedTabletMap &get_related_tablet_map() { return related_tablet_map_; }
  bool is_partition_hit();
  void unmark_need_check_server();

  int build_external_table_location(
      uint64_t table_loc_id, uint64_t ref_table_id, common::ObIArray<ObAddr> &locations);
  int build_related_tablet_map(const ObDASTableLocMeta &loc_meta);

  TO_STRING_KV(K_(table_locs),
               K_(external_table_locs),
               K_(is_fk_cascading),
               K_(snapshot),
               K_(savepoint),
               K_(write_branch_id));
private:
  int check_same_server(const ObDASTabletLoc *tablet_loc);
private:
  DASTableLocList table_locs_;
  /*  The external table locations stored in table_locs_ are fake local locations generated by optimizer.
   *  The real locations of external table are determined at runtime when building dfos by QC.
   *  external_cached_table_locs_ are "cached values" which only used by QC and do not need to serialized to SQC.
   */
  DASTableLocList external_table_locs_;
  ObSqlCtx *sql_ctx_;
  ObDASLocationRouter location_router_;
  ObDASTaskFactory das_factory_;
  DASRelatedTabletMap related_tablet_map_;
  common::ObIAllocator &allocator_;
  transaction::ObTxReadSnapshot snapshot_;           // Mvcc snapshot
  transaction::ObTxSEQ savepoint_;                   // DML savepoint
  // for DML like `insert update` and `replace`, which use savepoint to
  // resolve conflicts and when these DML executed under partition-wise
  // style, they need rollback their own writes but not all, we assign
  // id to data writes by different writer thread (named branch)
  int16_t write_branch_id_;
  //@todo: save snapshot version
  DASDelCtxList del_ctx_list_;
public:
  int64_t jump_read_group_id_;
  union {
    uint64_t flags_;
    struct {
      uint64_t is_fk_cascading_                 : 1; //fk starts to trigger nested sql
      uint64_t need_check_server_               : 1; //need to check if partitions hit the same server
      uint64_t same_server_                     : 1; //if partitions hit the same server, could be local or remote
      uint64_t iter_uncommitted_row_            : 1; //iter uncommitted row in fk_checker
      uint64_t reserved_                        : 60;
    };
  };
};
}  // namespace sql
}  // namespace oceanbase
#endif /* DEV_SRC_SQL_DAS_OB_DAS_CONTEXT_H_ */
