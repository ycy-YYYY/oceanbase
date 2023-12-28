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

#define USING_LOG_PREFIX STORAGE_COMPACTION

#include "storage/ddl/ob_ddl_merge_task.h"
#include "share/scn.h"
#include "share/ob_ddl_checksum.h"
#include "share/ob_get_compat_mode.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/scheduler/ob_dag_warning_history_mgr.h"
#include "storage/blocksstable/index_block/ob_index_block_builder.h"
#include "storage/blocksstable/index_block/ob_sstable_sec_meta_iterator.h"
#include "storage/ddl/ob_tablet_ddl_kv_mgr.h"
#include "storage/ddl/ob_direct_load_struct.h"
#include "storage/ls/ob_ls.h"
#include "storage/meta_mem/ob_tablet_handle.h"
#include "storage/tablet/ob_tablet_create_delete_helper.h"
#include "storage/tablet/ob_tablet_create_sstable_param.h"
#include "storage/tx_storage/ob_ls_map.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/tx_storage/ob_ls_handle.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/ob_ddl_sim_point.h"
#include "observer/ob_server_event_history_table_operator.h"
#include "storage/column_store/ob_column_oriented_sstable.h"
#include "storage/ddl/ob_direct_insert_sstable_ctx_new.h"
#include "storage/column_store/ob_column_oriented_sstable.h"

using namespace oceanbase::observer;
using namespace oceanbase::share::schema;
using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::blocksstable;

namespace oceanbase
{
namespace storage
{

/******************             ObDDLTableMergeDag             *****************/
ObDDLTableMergeDag::ObDDLTableMergeDag()
  : ObIDag(ObDagType::DAG_TYPE_DDL_KV_MERGE),
    is_inited_(false),
    ddl_param_()
{
}

ObDDLTableMergeDag::~ObDDLTableMergeDag()
{
}

int ObDDLTableMergeDag::init_by_param(const share::ObIDagInitParam *param)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObTablet *tablet = nullptr;
  ObTabletHandle tablet_handle;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(ddl_param_));
  } else if (OB_ISNULL(param) || !param->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(param));
  } else {
    ddl_param_ = *static_cast<const ObDDLTableMergeDagParam *>(param);
    is_inited_ = true;
  }
  return ret;
}

int ObDDLTableMergeDag::create_first_task()
{
  int ret = OB_SUCCESS;
  ObLSService *ls_service = MTL(ObLSService *);
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObDDLKvMgrHandle ddl_kv_mgr_handle;
  ObArray<ObDDLKVHandle> ddl_kvs_handle;
  ObDDLTableMergeTask *merge_task = nullptr;
  if (OB_FAIL(ls_service->get_ls(ddl_param_.ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ddl_param_));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                               ddl_param_.tablet_id_,
                                               tablet_handle,
                                               ObMDSGetTabletMode::READ_ALL_COMMITED))) {
    LOG_WARN("get tablet failed", K(ret), K(ddl_param_));
  } else if (OB_UNLIKELY(!tablet_handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), K(ddl_param_));
  } else if (OB_FAIL(tablet_handle.get_obj()->get_ddl_kv_mgr(ddl_kv_mgr_handle))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_TASK_EXPIRED;
      LOG_INFO("ddl kv mgr not exist", K(ret), K(ddl_param_));
    } else {
      LOG_WARN("get ddl kv mgr failed", K(ret), K(ddl_param_));
    }
  } else if (is_full_direct_load(ddl_param_.direct_load_type_)
      && ddl_param_.start_scn_ < tablet_handle.get_obj()->get_tablet_meta().ddl_start_scn_) {
    ret = OB_TASK_EXPIRED;
    LOG_WARN("ddl task expired, skip it", K(ret), K(ddl_param_), "new_start_scn", tablet_handle.get_obj()->get_tablet_meta().ddl_start_scn_);
  } else if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->freeze_ddl_kv(
      ddl_param_.start_scn_, ddl_param_.snapshot_version_, ddl_param_.data_format_version_))) {
    LOG_WARN("ddl kv manager try freeze failed", K(ret), K(ddl_param_));
  } else if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->get_ddl_kvs(true/*frozen_only*/, ddl_kvs_handle))) {
    LOG_WARN("get freezed ddl kv failed", K(ret), K(ddl_param_));
  } else if (OB_FAIL(alloc_task(merge_task))) {
    LOG_WARN("Fail to alloc task", K(ret), K(ddl_param_));
  } else if (OB_FAIL(merge_task->init(ddl_param_, ddl_kvs_handle))) {
    LOG_WARN("failed to init ddl table merge task", K(ret), K(*this));
  } else if (OB_FAIL(add_task(*merge_task))) {
    LOG_WARN("Fail to add task", K(ret), K(ddl_param_));
  }
  return ret;
}

bool ObDDLTableMergeDag::operator == (const ObIDag &other) const
{
  bool is_same = true;
  if (this == &other) {
  } else if (get_type() != other.get_type()) {
    is_same = false;
  } else {
    const ObDDLTableMergeDag &other_dag = static_cast<const ObDDLTableMergeDag&> (other);
    // each tablet has max 1 dag in running, so that the compaction task is unique and no need to consider concurrency
    is_same = ddl_param_.tablet_id_ == other_dag.ddl_param_.tablet_id_
      && ddl_param_.ls_id_ == other_dag.ddl_param_.ls_id_;
  }
  return is_same;
}

int64_t ObDDLTableMergeDag::hash() const
{
  return ddl_param_.tablet_id_.hash();
}

int ObDDLTableMergeDag::fill_info_param(compaction::ObIBasicInfoParam *&out_param, ObIAllocator &allocator) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLTableMergeDag has not been initialized", K(ret));
  } else if (OB_FAIL(ADD_DAG_WARN_INFO_PARAM(out_param, allocator, get_type(),
                                  ddl_param_.ls_id_.id(),
                                  static_cast<int64_t>(ddl_param_.tablet_id_.id()),
                                  static_cast<int64_t>(ddl_param_.rec_scn_.get_val_for_inner_table_field()),
                                  "is_commit", to_cstring(ddl_param_.is_commit_)))) {
    LOG_WARN("failed to fill info param", K(ret));
  }
  return ret;
}

int ObDDLTableMergeDag::fill_dag_key(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(databuff_printf(buf, buf_len, "ddl table merge task: ls_id=%ld, tablet_id=%ld, rec_scn=%lu",
                              ddl_param_.ls_id_.id(), ddl_param_.tablet_id_.id(), ddl_param_.rec_scn_.get_val_for_inner_table_field()))) {
    LOG_WARN("fill dag key for ddl table merge dag failed", K(ret), K(ddl_param_));
  }
  return ret;
}

bool ObDDLTableMergeDag::ignore_warning()
{
  return OB_LS_NOT_EXIST == dag_ret_
    || OB_TABLET_NOT_EXIST == dag_ret_
    || OB_TASK_EXPIRED == dag_ret_
    || OB_EAGAIN == dag_ret_
    || OB_NEED_RETRY == dag_ret_;
}

ObDDLTableMergeTask::ObDDLTableMergeTask()
  : ObITask(ObITaskType::TASK_TYPE_DDL_KV_MERGE),
    is_inited_(false), merge_param_()
{

}

ObDDLTableMergeTask::~ObDDLTableMergeTask()
{
}

int ObDDLTableMergeTask::init(const ObDDLTableMergeDagParam &ddl_dag_param, const ObIArray<ObDDLKVHandle> &frozen_ddl_kvs)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret), K(merge_param_));
  } else if (OB_UNLIKELY(!ddl_dag_param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_dag_param));
  } else if (OB_FAIL(frozen_ddl_kvs_.assign(frozen_ddl_kvs))) {
    LOG_WARN("assign ddl kv handle array failed", K(ret), K(frozen_ddl_kvs.count()));
  } else {
    merge_param_ = ddl_dag_param;
    is_inited_ = true;
  }
  return ret;
}

int ObDDLTableMergeTask::process()
{
  int ret = OB_SUCCESS;
  int64_t MAX_DDL_SSTABLE = ObTabletDDLKvMgr::MAX_DDL_KV_CNT_IN_STORAGE * 0.5;
#ifdef ERRSIM
  if (0 != GCONF.errsim_max_ddl_sstable_count) {
    MAX_DDL_SSTABLE = GCONF.errsim_max_ddl_sstable_count;
  } else {
    MAX_DDL_SSTABLE = 2;
  }
  LOG_INFO("set max ddl sstable in errsim mode", K(MAX_DDL_SSTABLE));
#endif
  LOG_INFO("ddl merge task start process", K(*this), "ddl_event_info", ObDDLEventInfo());
  ObTabletHandle tablet_handle;
  ObDDLKvMgrHandle ddl_kv_mgr_handle;
  ObLSHandle ls_handle;
  ObTableStoreIterator ddl_table_iter;
  ObTabletMemberWrapper<ObTabletTableStore> table_store_wrapper;
  const uint64_t tenant_id = MTL_ID();
  common::ObArenaAllocator allocator("DDLMergeTask", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObTableHandleV2 old_sstable_handle;
  ObTableHandleV2 compacted_sstable_handle;
  ObSSTable *sstable = nullptr;
  bool is_major_exist = false;
  ObTenantDirectLoadMgr *tenant_direct_load_mgr = MTL(ObTenantDirectLoadMgr *);
  ObTabletDirectLoadMgrHandle tablet_mgr_hdl;
  ObTabletFullDirectLoadMgr *tablet_direct_load_mgr = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_ISNULL(tenant_direct_load_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), K(MTL_ID()));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(merge_param_.ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(merge_param_));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                               merge_param_.tablet_id_,
                                               tablet_handle,
                                               ObMDSGetTabletMode::READ_ALL_COMMITED))) {
    LOG_WARN("failed to get tablet", K(ret), K(merge_param_));
  } else if (OB_FAIL(tablet_handle.get_obj()->get_ddl_kv_mgr(ddl_kv_mgr_handle))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_TASK_EXPIRED;
      LOG_INFO("ddl kv mgr not exist", K(ret), K(merge_param_));
    } else {
      LOG_WARN("get ddl kv mgr failed", K(ret), K(merge_param_));
    }
  } else if (OB_FAIL(tablet_handle.get_obj()->get_ddl_sstables(ddl_table_iter))) {
    LOG_WARN("get ddl sstable handles failed", K(ret));
  } else {
    DEBUG_SYNC(BEFORE_DDL_TABLE_MERGE_TASK);
#ifdef ERRSIM
    static int64_t counter = 0;
    counter++;
    if (counter >= 2) {
      DEBUG_SYNC(BEFORE_MIG_DDL_TABLE_MERGE_TASK);
    }
#endif

    ObTabletDDLParam ddl_param;
    bool is_data_complete = false;
    const ObSSTable *first_major_sstable = nullptr;
    SCN compact_start_scn, compact_end_scn;
    if (OB_FAIL(ObTabletDDLUtil::check_and_get_major_sstable(
        merge_param_.ls_id_, merge_param_.tablet_id_, first_major_sstable, table_store_wrapper))) {
      LOG_WARN("check if major sstable exist failed", K(ret));
    } else if (nullptr != first_major_sstable) {
      is_major_exist = true;
      LOG_INFO("major sstable has been created before", K(merge_param_));
    } else if (tablet_handle.get_obj()->get_tablet_meta().table_store_flag_.with_major_sstable()) {
      is_major_exist = true;
      LOG_INFO("tablet me says with major but no major, meaning its a migrated deleted tablet, skip");
    } else if (OB_FAIL(tenant_direct_load_mgr->get_tablet_mgr(merge_param_.tablet_id_,
                                                              true /* is_full_direct_load */,
                                                               tablet_mgr_hdl))) {
      LOG_WARN("get tablet direct load mgr failed", K(ret), K(merge_param_));
    } else if (OB_FAIL(tablet_mgr_hdl.get_full_obj()->prepare_major_merge_param(ddl_param))) {
      LOG_WARN("preare full direct load sstable param failed", K(ret));
    } else if (merge_param_.start_scn_ > SCN::min_scn() && merge_param_.start_scn_ < ddl_param.start_scn_) {
      ret = OB_TASK_EXPIRED;
      LOG_INFO("ddl merge task expired, do nothing", K(merge_param_), "new_start_scn", ddl_param.start_scn_);
    } else if (OB_FAIL(ObTabletDDLUtil::get_compact_scn(ddl_param.start_scn_, ddl_table_iter, frozen_ddl_kvs_, compact_start_scn, compact_end_scn))) {
      LOG_WARN("get compact scn failed", K(ret), K(merge_param_), K(ddl_param), K(ddl_table_iter), K(frozen_ddl_kvs_));
    } else if (ddl_param.commit_scn_.is_valid_and_not_min() && compact_end_scn > ddl_param.commit_scn_) {
      ret = OB_ERR_SYS;
      LOG_WARN("compact end scn is larger than commit scn", K(ret), K(ddl_param), K(compact_end_scn), K(frozen_ddl_kvs_), K(ddl_table_iter));
    } else {
      bool is_data_complete = merge_param_.is_commit_
        && compact_start_scn == SCN::scn_dec(merge_param_.start_scn_)
        && compact_end_scn == merge_param_.rec_scn_
#ifdef ERRSIM
        // skip build major until current time reach the delayed time
        && ObTimeUtility::current_time() > merge_param_.rec_scn_.convert_to_ts() + GCONF.errsim_ddl_major_delay_time
#endif
        ;
      if (!is_data_complete) {
        ddl_param.table_key_.table_type_ = ddl_param.table_key_.is_co_sstable() ? ObITable::DDL_MERGE_CO_SSTABLE : ObITable::DDL_DUMP_SSTABLE;
        ddl_param.table_key_.scn_range_.start_scn_ = compact_start_scn;
        ddl_param.table_key_.scn_range_.end_scn_ = compact_end_scn;
      } else {
        // use the final table key of major, do nothing
      }
      if (OB_FAIL(ObTabletDDLUtil::compact_ddl_kv(*ls_handle.get_ls(),
                                                  *tablet_handle.get_obj(),
                                                  ddl_table_iter,
                                                  frozen_ddl_kvs_,
                                                  ddl_param,
                                                  allocator,
                                                  compacted_sstable_handle))) {
        LOG_WARN("compact sstables failed", K(ret), K(ddl_param), K(is_data_complete));
      } else if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->release_ddl_kvs(compact_end_scn))) {
        LOG_WARN("release ddl kv failed", K(ret), K(ddl_param), K(compact_end_scn));
      }
      if (OB_SUCC(ret) && is_data_complete) {
        is_major_exist = true;
        LOG_INFO("create major sstable success", K(ret), K(ddl_param), KPC(compacted_sstable_handle.get_table()));
      }
    }

    if (OB_SUCC(ret) && merge_param_.is_commit_ && is_major_exist) {
      if (OB_FAIL(MTL(ObTabletTableUpdater*)->submit_tablet_update_task(merge_param_.ls_id_, merge_param_.tablet_id_))) {
        LOG_WARN("fail to submit tablet update task", K(ret), K(tenant_id), K(merge_param_));
      } else if (OB_FAIL(tenant_direct_load_mgr->remove_tablet_direct_load(ObTabletDirectLoadMgrKey(merge_param_.tablet_id_, true)))) {
        if (OB_ENTRY_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("remove tablet mgr failed", K(ret), K(MTL_ID()), K(merge_param_));
        }
      }
      LOG_INFO("commit ddl sstable finished", K(ret), K(ddl_param), K(merge_param_), KPC(tablet_mgr_hdl.get_full_obj()), "ddl_event_info", ObDDLEventInfo());
    }
  }
  return ret;
}

// the input ddl sstable is sorted with start_scn
int ObTabletDDLUtil::check_data_continue(
    ObTableStoreIterator &ddl_sstable_iter,
    bool &is_data_continue,
    share::SCN &compact_start_scn,
    share::SCN &compact_end_scn)
{
  int ret = OB_SUCCESS;
  is_data_continue = false;
  ddl_sstable_iter.resume();
  if (OB_UNLIKELY(!ddl_sstable_iter.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_sstable_iter.count()));
  } else if (1 == ddl_sstable_iter.count()) {
    ObITable *single_table = nullptr;
    if (OB_FAIL(ddl_sstable_iter.get_boundary_table(true/*is_last*/, single_table))) {
      LOG_WARN("get single table failed", K(ret));
    } else {
      is_data_continue = true;
      compact_start_scn = SCN::min(compact_start_scn, single_table->get_start_scn());
      compact_end_scn = SCN::max(compact_end_scn, single_table->get_end_scn());
    }
  } else {
    ObITable *first_ddl_sstable = nullptr;
    ObITable *last_ddl_sstable = nullptr;
    if (OB_FAIL(ddl_sstable_iter.get_boundary_table(false, first_ddl_sstable))) {
      LOG_WARN("fail to get first ddl sstable", K(ret));
    } else if (OB_FAIL(ddl_sstable_iter.get_boundary_table(true, last_ddl_sstable))) {
      LOG_WARN("fail to get last ddl sstable", K(ret));
    } else {
      is_data_continue = true;
      SCN last_end_scn = first_ddl_sstable->get_end_scn();
      ObITable *table = nullptr;
      while (OB_SUCC(ddl_sstable_iter.get_next(table))) {
        if (OB_ISNULL(table) || OB_UNLIKELY(!table->is_sstable())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected error, table is nullptr", K(ret), KPC(table));
        } else {
          ObSSTable *cur_ddl_sstable = static_cast<ObSSTable *>(table);
          if (cur_ddl_sstable->get_start_scn() <= last_end_scn) {
            last_end_scn = SCN::max(last_end_scn, cur_ddl_sstable->get_end_scn());
          } else {
            is_data_continue = false;
            LOG_INFO("ddl sstable not continue", K(cur_ddl_sstable->get_key()), K(last_end_scn));
            break;
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
      if (OB_SUCC(ret) && is_data_continue) {
        compact_start_scn = SCN::min(compact_start_scn, first_ddl_sstable->get_start_scn());
        compact_end_scn = SCN::max(compact_end_scn, last_ddl_sstable->get_end_scn());
      }
    }
  }
  return ret;
}


int ObTabletDDLUtil::check_data_continue(
    const ObIArray<ObDDLKVHandle> &ddl_kvs,
    bool &is_data_continue,
    share::SCN &compact_start_scn,
    share::SCN &compact_end_scn)
{
  int ret = OB_SUCCESS;
  is_data_continue = false;
  if (OB_UNLIKELY(ddl_kvs.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_kvs.count()));
  } else if (1 == ddl_kvs.count()) {
    is_data_continue = true;
    ObDDLKV *single_kv = ddl_kvs.at(0).get_obj();
    compact_start_scn = SCN::min(compact_start_scn, single_kv->get_start_scn());
    compact_end_scn = SCN::max(compact_end_scn, single_kv->get_end_scn());
  } else {
    ObDDLKVHandle first_kv_handle = ddl_kvs.at(0);
    ObDDLKVHandle last_kv_handle = ddl_kvs.at(ddl_kvs.count() - 1);
    is_data_continue = true;
    SCN last_end_scn = first_kv_handle.get_obj()->get_end_scn();
    for (int64_t i = 1; OB_SUCC(ret) && i < ddl_kvs.count(); ++i) {
      ObDDLKVHandle cur_kv = ddl_kvs.at(i);
      if (OB_ISNULL(cur_kv.get_obj())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ddl kv is null", K(ret), K(i));
      } else if (cur_kv.get_obj()->get_start_scn() <= last_end_scn) {
        last_end_scn = SCN::max(last_end_scn, cur_kv.get_obj()->get_end_scn());
      } else {
        is_data_continue = false;
        LOG_INFO("ddl kv not continue", K(i), K(last_end_scn), KPC(cur_kv.get_obj()));
        break;
      }
    }
    if (OB_SUCC(ret) && is_data_continue) {
      compact_start_scn = SCN::min(compact_start_scn, first_kv_handle.get_obj()->get_start_scn());
      compact_end_scn = SCN::max(compact_end_scn, last_kv_handle.get_obj()->get_end_scn());
    }
  }
  return ret;
}

int ObTabletDDLUtil::prepare_index_data_desc(ObTablet &tablet,
                                             const int64_t cg_idx,
                                             const int64_t snapshot_version,
                                             const uint64_t data_format_version,
                                             const ObSSTable *first_ddl_sstable,
                                             const SCN &end_scn,
                                             ObWholeDataStoreDesc &data_desc)
{
  int ret = OB_SUCCESS;
  data_desc.reset();
  ObLSService *ls_service = MTL(ObLSService *);
  ObArenaAllocator tmp_arena("DDLIdxDescTmp", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  const ObTabletID &tablet_id = tablet.get_tablet_meta().tablet_id_;
  const ObLSID &ls_id = tablet.get_tablet_meta().ls_id_;
  ObStorageSchema *storage_schema = nullptr;
  if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid() || snapshot_version <= 0 || data_format_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ls_id), K(tablet_id), K(snapshot_version), K(data_format_version));
  } else if (OB_FAIL(tablet.load_storage_schema(tmp_arena, storage_schema))) {
    LOG_WARN("fail to get storage schema", K(ret));
  } else if (cg_idx >= 0) {
    const ObIArray<ObStorageColumnGroupSchema > &cg_schemas = storage_schema->get_column_groups();
    if (cg_idx >= cg_schemas.count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid cg idx", K(ret), K(cg_idx), K(cg_schemas.count()));
    } else {
      const ObStorageColumnGroupSchema &cur_cg_schema = cg_schemas.at(cg_idx);
      if (OB_FAIL(data_desc.init(*storage_schema, ls_id, tablet_id,
              compaction::ObMergeType::MAJOR_MERGE, snapshot_version, data_format_version, end_scn, &cur_cg_schema, cg_idx))) {
        LOG_WARN("init data desc for cg failed", K(ret));
      } else {
        LOG_DEBUG("get data desc from column group schema", K(ret), K(tablet_id), K(cg_idx), K(data_desc), K(cur_cg_schema));
      }
    }
  } else if (OB_FAIL(data_desc.init(*storage_schema,
                                    ls_id,
                                    tablet_id,
                                    compaction::MAJOR_MERGE,
                                    snapshot_version,
                                    data_format_version))) {
    // use storage schema to init ObDataStoreDesc
    // all cols' default checksum will assigned to 0
    // means all macro should contain all columns in schema
    LOG_WARN("init data store desc failed", K(ret), K(tablet_id));
  }
  if (OB_SUCC(ret) && nullptr != first_ddl_sstable) {
    // use the param in first ddl sstable, which persist the param when ddl start
    ObSSTableMetaHandle meta_handle;
    if (OB_FAIL(first_ddl_sstable->get_meta(meta_handle))) {
      LOG_WARN("get sstable meta handle fail", K(ret), KPC(first_ddl_sstable));
    } else {
      const ObSSTableBasicMeta &basic_meta = meta_handle.get_sstable_meta().get_basic_meta();
      if (OB_FAIL(data_desc.get_desc().update_basic_info_from_macro_meta(basic_meta))) {
        LOG_WARN("failed to update basic info from macro_meta", KR(ret), K(basic_meta));
      }
    }
  }
  ObTabletObjLoadHelper::free(tmp_arena, storage_schema);
  LOG_DEBUG("prepare_index_data_desc", K(ret), K(data_desc));
  return ret;
}

int ObTabletDDLUtil::create_ddl_sstable(ObTablet &tablet,
                                        const ObTabletDDLParam &ddl_param,
                                        const ObIArray<const ObDataMacroBlockMeta *> &meta_array,
                                        const ObSSTable *first_ddl_sstable,
                                        common::ObArenaAllocator &allocator,
                                        ObTableHandleV2 &sstable_handle)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(ObSSTableIndexBuilder, sstable_index_builder) {
    ObIndexBlockRebuilder index_block_rebuilder;
    ObWholeDataStoreDesc data_desc(true/*is_ddl*/);
    int64_t macro_block_column_count = 0;
    if (OB_UNLIKELY(!ddl_param.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret), K(ddl_param));
    } else if (OB_FAIL(ObTabletDDLUtil::prepare_index_data_desc(
            tablet,
            ddl_param.table_key_.is_column_store_sstable() ? ddl_param.table_key_.get_column_group_id() : -1/*negative value means row store*/,
            ddl_param.snapshot_version_,
            ddl_param.data_format_version_,
            first_ddl_sstable,
            ddl_param.table_key_.get_end_scn(),
            data_desc))) {
      LOG_WARN("prepare data store desc failed", K(ret), K(ddl_param));
    } else if (FALSE_IT(macro_block_column_count = meta_array.empty() ? 0 : meta_array.at(0)->get_meta_val().column_count_)) {
    } else if (meta_array.count() > 0 && OB_FAIL(data_desc.get_col_desc().mock_valid_col_default_checksum_array(macro_block_column_count))) {
      LOG_WARN("mock valid column default checksum failed", K(ret), "firt_macro_block_meta", to_cstring(meta_array.at(0)), K(ddl_param));
    } else if (OB_FAIL(sstable_index_builder.init(data_desc.get_desc(),
                                                   nullptr, // macro block flush callback
                                                   ddl_param.table_key_.is_major_sstable() ? ObSSTableIndexBuilder::ENABLE : ObSSTableIndexBuilder::DISABLE))) {
      LOG_WARN("init sstable index builder failed", K(ret), K(data_desc));
    } else if (OB_FAIL(index_block_rebuilder.init(sstable_index_builder,
            false/*need_sort*/,
            nullptr/*task_idx*/,
            true/*use_absolute_offset*/))) {
      LOG_WARN("fail to alloc index builder", K(ret));
    } else if (meta_array.empty()) {
      // do nothing
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < meta_array.count(); ++i) {
        if (OB_FAIL(index_block_rebuilder.append_macro_row(*meta_array.at(i)))) {
          LOG_WARN("append block meta failed", K(ret), K(i));
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(index_block_rebuilder.close())) {
        LOG_WARN("close index block rebuilder failed", K(ret));
      } else if (OB_FAIL(ObTabletDDLUtil::create_ddl_sstable(tablet, &sstable_index_builder, ddl_param, first_ddl_sstable,
              macro_block_column_count, allocator, sstable_handle))) {
        LOG_WARN("create ddl sstable failed", K(ret), K(ddl_param));
      }
    }
  }
  return ret;
}


int ObTabletDDLUtil::create_ddl_sstable(
    ObTablet &tablet,
    ObSSTableIndexBuilder *sstable_index_builder,
    const ObTabletDDLParam &ddl_param,
    const ObSSTable *first_ddl_sstable,
    const int64_t macro_block_column_count,
    common::ObArenaAllocator &allocator,
    ObTableHandleV2 &sstable_handle)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_arena("CreateDDLSstTmp", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObStorageSchema *storage_schema = nullptr;
  SMART_VAR(ObSSTableMergeRes, res) {
    if (OB_UNLIKELY(nullptr == sstable_index_builder || !ddl_param.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret), KP(sstable_index_builder), K(ddl_param));
    } else if (OB_FAIL(tablet.load_storage_schema(tmp_arena, storage_schema))) {
      LOG_WARN("failed to load storage schema", K(ret), K(tablet.get_tablet_meta()));
    } else {
      int64_t column_count = 0;
      int64_t full_column_cnt = 0; // only used for co sstable
      share::schema::ObTableMode table_mode = storage_schema->get_table_mode_struct();
      share::schema::ObIndexType index_type = storage_schema->get_index_type();
      int64_t rowkey_column_cnt = storage_schema->get_rowkey_column_num() + ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();
      common::ObRowStoreType row_store_type = storage_schema->get_row_store_type();
      if (nullptr != first_ddl_sstable) {
        ObSSTableMetaHandle meta_handle;
        if (OB_FAIL(first_ddl_sstable->get_meta(meta_handle))) {
          LOG_WARN("get sstable meta handle fail", K(ret), KPC(first_ddl_sstable));
        } else {
          column_count = meta_handle.get_sstable_meta().get_column_count();
          table_mode = meta_handle.get_sstable_meta().get_basic_meta().table_mode_;
          index_type = static_cast<share::schema::ObIndexType>(meta_handle.get_sstable_meta().get_basic_meta().index_type_);
          rowkey_column_cnt = meta_handle.get_sstable_meta().get_basic_meta().rowkey_column_count_;
          row_store_type = meta_handle.get_sstable_meta().get_basic_meta().latest_row_store_type_;
          if (first_ddl_sstable->is_co_sstable()) {
            const ObCOSSTableV2 *first_co_sstable = static_cast<const ObCOSSTableV2 *>(first_ddl_sstable);
            if (OB_ISNULL((first_co_sstable))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("first co sstable is null", K(ret), KP(first_co_sstable), KPC(first_ddl_sstable));
            } else {
              full_column_cnt = first_co_sstable->get_cs_meta().full_column_cnt_;
            }
          }
        }
      } else if (ddl_param.table_key_.is_column_store_sstable()) {
        if (ddl_param.table_key_.is_normal_cg_sstable()) {
          rowkey_column_cnt = 0;
          column_count = 1;
        } else { // co sstable with all cg or rowkey cg
          const ObIArray<ObStorageColumnGroupSchema> &cg_schemas = storage_schema->get_column_groups();
          const int64_t cg_idx = ddl_param.table_key_.get_column_group_id();
          if (cg_idx < 0 || cg_idx >= cg_schemas.count()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected column group index", K(ret), K(cg_idx));
          } else if (OB_FAIL(storage_schema->get_stored_column_count_in_sstable(full_column_cnt))) { // set full_column_cnt in first ddl sstable
            LOG_WARN("fail to get stored column count in sstable", K(ret));
          } else if (cg_schemas.at(cg_idx).is_rowkey_column_group()) {
            column_count = rowkey_column_cnt;
          } else {
            column_count = full_column_cnt;
            if (macro_block_column_count > 0 && macro_block_column_count < column_count) {
              LOG_INFO("use macro block column count", K(ddl_param), K(macro_block_column_count), K(column_count));
              column_count = macro_block_column_count;
              full_column_cnt = macro_block_column_count;
            }
          }
        }
      } else { // row store sstable
        if (OB_FAIL(storage_schema->get_stored_column_count_in_sstable(column_count))) {
          LOG_WARN("fail to get stored column count in sstable", K(ret));
        } else if (macro_block_column_count > 0 && macro_block_column_count < column_count) {
          LOG_INFO("use macro block column count", K(ddl_param), K(macro_block_column_count), K(column_count));
          column_count = macro_block_column_count;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(sstable_index_builder->close(res))) {
        LOG_WARN("close sstable index builder close failed", K(ret));
      } else if (ddl_param.table_key_.is_normal_cg_sstable() // index builder of cg sstable cannot get trans_version from row, manually set it
          && FALSE_IT(res.max_merged_trans_version_ = ddl_param.snapshot_version_)) {
      } else if (OB_UNLIKELY((ddl_param.table_key_.is_major_sstable() ||
                              ddl_param.table_key_.is_ddl_sstable()) &&
                             res.row_count_ > 0 &&
                             res.max_merged_trans_version_ != ddl_param.snapshot_version_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("max_merged_trans_version_ in res is different from ddl snapshot version", K(ret),
                 K(res), K(ddl_param));
      } else {
        const int64_t create_schema_version_on_tablet = tablet.get_tablet_meta().create_schema_version_;
        ObTabletCreateSSTableParam param;
        param.table_key_ = ddl_param.table_key_;
        param.table_mode_ = table_mode;
        param.index_type_ = index_type;
        param.rowkey_column_cnt_ = rowkey_column_cnt;
        param.schema_version_ = create_schema_version_on_tablet;
        param.latest_row_store_type_ = row_store_type;
        param.create_snapshot_version_ = ddl_param.snapshot_version_;
        param.ddl_scn_ = ddl_param.start_scn_;
        ObSSTableMergeRes::fill_addr_and_data(res.root_desc_,
            param.root_block_addr_, param.root_block_data_);
        ObSSTableMergeRes::fill_addr_and_data(res.data_root_desc_,
            param.data_block_macro_meta_addr_, param.data_block_macro_meta_);
        param.is_meta_root_ = res.data_root_desc_.is_meta_root_;
        param.root_row_store_type_ = res.root_row_store_type_;
        param.data_index_tree_height_ = res.root_desc_.height_;
        param.index_blocks_cnt_ = res.index_blocks_cnt_;
        param.data_blocks_cnt_ = res.data_blocks_cnt_;
        param.micro_block_cnt_ = res.micro_block_cnt_;
        param.use_old_macro_block_count_ = res.use_old_macro_block_count_;
        param.row_count_ = res.row_count_;
        param.column_cnt_ = column_count;
        param.full_column_cnt_ = full_column_cnt;
        param.data_checksum_ = res.data_checksum_;
        param.occupy_size_ = res.occupy_size_;
        param.original_size_ = res.original_size_;
        param.max_merged_trans_version_ = ddl_param.snapshot_version_;
        param.contain_uncommitted_row_ = res.contain_uncommitted_row_;
        param.compressor_type_ = res.compressor_type_;
        param.encrypt_id_ = res.encrypt_id_;
        param.master_key_id_ = res.master_key_id_;
        param.nested_size_ = res.nested_size_;
        param.nested_offset_ = res.nested_offset_;
        param.data_block_ids_ = res.data_block_ids_;
        param.other_block_ids_ = res.other_block_ids_;
        MEMCPY(param.encrypt_key_, res.encrypt_key_, share::OB_MAX_TABLESPACE_ENCRYPT_KEY_LENGTH);
        if (ddl_param.table_key_.is_co_sstable()) {
          param.column_group_cnt_ = storage_schema->get_column_group_count();
          // only set true when build empty major sstable. ddl co sstable must set false and fill cg sstables
          param.is_empty_co_table_ = ddl_param.table_key_.is_major_sstable() && 0 == param.data_blocks_cnt_;
          const int64_t base_cg_idx = ddl_param.table_key_.get_column_group_id();
          if (base_cg_idx < 0 || base_cg_idx >= storage_schema->get_column_group_count()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("invalid column group index", K(ret), K(ddl_param.table_key_));
          } else {
            const ObStorageColumnGroupSchema &base_cg_schema = storage_schema->get_column_groups().at(base_cg_idx);
            if (base_cg_schema.is_all_column_group()) {
              param.co_base_type_ = ObCOSSTableBaseType::ALL_CG_TYPE;
            } else if (base_cg_schema.is_rowkey_column_group()) {
              param.co_base_type_ = ObCOSSTableBaseType::ROWKEY_CG_TYPE;
            } else {
              ret = OB_ERR_SYS;
              LOG_WARN("unknown type of base cg schema", K(ret), K(base_cg_idx));
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(param.column_checksums_.assign(res.data_column_checksums_))) {
          LOG_WARN("fail to fill column checksum for empty major", K(ret), K(param));
        } else if (OB_UNLIKELY(param.column_checksums_.count() != column_count)) {
          // we have corrected the col_default_checksum_array_ in prepare_index_data_desc
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected column checksums", K(ret), K(column_count), K(param));
        } else {
          if (ddl_param.table_key_.is_co_sstable()) {
            if (OB_FAIL(ObTabletCreateDeleteHelper::create_sstable<ObCOSSTableV2>(param, allocator, sstable_handle))) {
              LOG_WARN("create sstable failed", K(ret), K(param));
            }
          } else {
            if (OB_FAIL(ObTabletCreateDeleteHelper::create_sstable<ObSSTable>(param, allocator, sstable_handle))) {
              LOG_WARN("create sstable failed", K(ret), K(param));
            }
          }
          if (OB_SUCC(ret)) {
            LOG_INFO("create ddl sstable success", K(ddl_param), K(sstable_handle),
                "create_schema_version", create_schema_version_on_tablet);
          }
        }
      }
    }
    ObTabletObjLoadHelper::free(tmp_arena, storage_schema);
  }
  return ret;
}

int ObTabletDDLUtil::update_ddl_table_store(
    ObLS &ls,
    ObTablet &tablet,
    const ObTabletDDLParam &ddl_param,
    common::ObArenaAllocator &allocator,
    blocksstable::ObSSTable *sstable)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!ddl_param.is_valid() || nullptr == sstable)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_param), KP(sstable));
  } else {
    ObArenaAllocator allocator("DDLUtil_update", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    ObStorageSchema *tablet_storage_schema = nullptr;
    if (OB_FAIL(tablet.load_storage_schema(allocator, tablet_storage_schema))) {
      LOG_WARN("fail to load storage schema failed", K(ret));
    } else {
      const bool is_major_sstable = ddl_param.table_key_.is_major_sstable();
      const int64_t rebuild_seq = ls.get_rebuild_seq();
      const int64_t snapshot_version = is_major_sstable ? max(ddl_param.snapshot_version_, tablet.get_snapshot_version())
                                                       : tablet.get_snapshot_version();
      const int64_t multi_version_start = is_major_sstable ? max(ddl_param.snapshot_version_, tablet.get_multi_version_start())
                                              : 0;
      ObTabletHandle new_tablet_handle;
      ObUpdateTableStoreParam table_store_param(sstable,
                                                snapshot_version,
                                                multi_version_start,
                                                rebuild_seq,
                                                tablet_storage_schema,
                                                is_major_sstable, // update_with_major_flag
                                                /*DDL does not have verification between replicas,
                                                  So using medium merge to force verification between replicas*/
                                                compaction::MEDIUM_MERGE,
                                                is_major_sstable// need report checksum
                                                );
      table_store_param.ddl_info_.keep_old_ddl_sstable_ = !is_major_sstable;
      table_store_param.ddl_info_.data_format_version_ = ddl_param.data_format_version_;
      table_store_param.ddl_info_.ddl_commit_scn_ = ddl_param.commit_scn_;
      table_store_param.ddl_info_.ddl_checkpoint_scn_ = sstable->is_ddl_dump_sstable() ? sstable->get_end_scn() : ddl_param.commit_scn_;
      if (OB_FAIL(ls.update_tablet_table_store(ddl_param.table_key_.get_tablet_id(), table_store_param, new_tablet_handle))) {
        LOG_WARN("failed to update tablet table store", K(ret), K(ddl_param.table_key_), K(table_store_param));
      } else {
        LOG_INFO("ddl update table store success", K(ddl_param), K(table_store_param), KPC(sstable));
      }
    }
    ObTabletObjLoadHelper::free(allocator, tablet_storage_schema);
  }
  return ret;
}

int get_sstables(ObTableStoreIterator &ddl_sstable_iter, const int64_t cg_idx, ObIArray<ObSSTable *> &target_sstables)
{
  int ret = OB_SUCCESS;
  ddl_sstable_iter.resume();
  while (OB_SUCC(ret)) {
    ObITable *table = nullptr;
    if (OB_FAIL(ddl_sstable_iter.get_next(table))) {
      if (OB_ITER_END != ret) {
        LOG_WARN("get next table failed", K(ret));
      } else {
        ret = OB_SUCCESS;
        break;
      }
    } else if (OB_ISNULL(table) || OB_UNLIKELY(!table->is_sstable())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected error, table is nullptr", K(ret), KPC(table));
    } else if (cg_idx < 0) { // row store
      if (OB_FAIL(target_sstables.push_back(static_cast<ObSSTable *>(table)))) {
        LOG_WARN("push back target sstable failed", K(ret));
      }
    } else if (!table->is_co_sstable()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("current table not co sstable", K(ret), KPC(table));
    } else {
      ObCOSSTableV2 *cur_co_sstable = static_cast<ObCOSSTableV2 *>(table);
      ObSSTableWrapper cg_sstable_wrapper;
      ObSSTable *cg_sstable = nullptr;
      if (OB_ISNULL(cur_co_sstable)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("current co sstable is null", K(ret), KP(cur_co_sstable));
      } else if (cur_co_sstable->is_empty_co_table()) {
        // skip
      } else if (OB_FAIL(cur_co_sstable->fetch_cg_sstable(cg_idx, cg_sstable_wrapper))) {
        LOG_WARN("get all tables failed", K(ret));
      } else if (OB_FAIL(cg_sstable_wrapper.get_sstable(cg_sstable))) {
        LOG_WARN("get sstable failed", K(ret));
      } else if (OB_ISNULL(cg_sstable)) {
        // skip
      } else if (OB_FAIL(target_sstables.push_back(cg_sstable))) {
        LOG_WARN("push back cg sstable failed", K(ret));
      }
    }
  }
  return ret;
}

int get_sstables(const ObIArray<ObDDLKVHandle> &frozen_ddl_kvs, const int64_t cg_idx, ObIArray<ObSSTable *> &target_sstables)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < frozen_ddl_kvs.count(); ++i) {
    ObDDLKV *cur_kv = frozen_ddl_kvs.at(i).get_obj();
    ObDDLMemtable *target_sstable = nullptr;
    if (OB_ISNULL(cur_kv)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (cg_idx < 0) { // row store
      if (cur_kv->get_ddl_memtables().empty()) {
        // do nothing
      } else if (OB_ISNULL(target_sstable = cur_kv->get_ddl_memtables().at(0))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("current sstable is null", K(ret), KPC(cur_kv), K(target_sstable));
      } else if (OB_FAIL(target_sstables.push_back(target_sstable))) {
        LOG_WARN("push back target sstable failed", K(ret));
      }
    } else if (OB_FAIL(cur_kv->get_ddl_memtable(cg_idx, target_sstable))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get ddl memtable failed", K(ret), K(i), K(cg_idx));
      } else {
        ret = OB_SUCCESS;
      }
    } else if (OB_ISNULL(target_sstable)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("target sstable from ddl kv is null", K(ret), K(i), K(cg_idx), KPC(cur_kv), KP(target_sstable));
    } else if (OB_FAIL(target_sstables.push_back(target_sstable))) {
      LOG_WARN("push back target sstable failed", K(ret));
    }
  }
  return ret;
}
 // for cg sstable, endkey is end row id, confirm read_info not used
int get_sorted_meta_array(
    const ObIArray<ObSSTable *> &sstables,
    const ObITableReadInfo &read_info,
    ObBlockMetaTree &meta_tree,
    ObIAllocator &allocator,
    ObArray<const ObDataMacroBlockMeta *> &sorted_metas)
{
  int ret = OB_SUCCESS;
  sorted_metas.reset();
  if (OB_UNLIKELY(!read_info.is_valid() || !meta_tree.is_valid())) { // allow empty sstable array
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(sstables), K(read_info), K(meta_tree));
  } else {
    SMART_VAR(ObSSTableSecMetaIterator, meta_iter) {
      ObDatumRange query_range;
      query_range.set_whole_range();
      ObDataMacroBlockMeta data_macro_meta;
      for (int64_t i = 0; OB_SUCC(ret) && i < sstables.count(); ++i) {
        ObSSTable *cur_sstable = sstables.at(i);
        if (OB_ISNULL(cur_sstable)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected error, table is nullptr", K(ret), KPC(cur_sstable));
        } else {
          meta_iter.reset();
          ObDataMacroBlockMeta *copied_meta = nullptr; // copied meta will destruct in the meta tree
          if (OB_FAIL(meta_iter.open(query_range,
                  ObMacroBlockMetaType::DATA_BLOCK_META,
                  *cur_sstable,
                  read_info,
                  allocator))) {
            LOG_WARN("sstable secondary meta iterator open failed", K(ret), KPC(cur_sstable), K(read_info));
          } else {
            while (OB_SUCC(ret)) {
              if (OB_FAIL(meta_iter.get_next(data_macro_meta))) {
                if (OB_ITER_END != ret) {
                  LOG_WARN("get data macro meta failed", K(ret));
                } else {
                  ret = OB_SUCCESS;
                  break;
                }
              } else {
                ObDDLMacroHandle macro_handle;
                bool is_exist = false;
                if (OB_FAIL(meta_tree.exist(&data_macro_meta.end_key_, is_exist))) {
                  LOG_WARN("check block meta exist failed", K(ret), K(data_macro_meta));
                } else if (is_exist) {
                  // skip
                  FLOG_INFO("append meta tree skip", K(ret), "table_key", cur_sstable->get_key(), "macro_block_id", data_macro_meta.get_macro_id(),
                      "data_checksum", data_macro_meta.val_.data_checksum_, K(meta_tree.get_macro_block_cnt()), "macro_block_end_key", to_cstring(data_macro_meta.end_key_));
                } else if (OB_FAIL(macro_handle.set_block_id(data_macro_meta.get_macro_id()))) {
                  LOG_WARN("hold macro block failed", K(ret));
                } else if (OB_FAIL(data_macro_meta.deep_copy(copied_meta, allocator))) {
                  LOG_WARN("deep copy macro block meta failed", K(ret));
                } else if (OB_FAIL(meta_tree.insert_macro_block(macro_handle, &copied_meta->end_key_, copied_meta))) {
                  LOG_WARN("insert meta tree failed", K(ret), K(macro_handle), KPC(copied_meta));
                  copied_meta->~ObDataMacroBlockMeta();
                } else {
                  FLOG_INFO("append meta tree success", K(ret), "table_key", cur_sstable->get_key(), "macro_block_id", data_macro_meta.get_macro_id(),
                      "data_checksum", copied_meta->val_.data_checksum_, K(meta_tree.get_macro_block_cnt()), "macro_block_end_key", to_cstring(copied_meta->end_key_));
                }
              }
            }
          }
          LOG_INFO("append meta tree finished", K(ret), "table_key", cur_sstable->get_key(), "data_macro_block_cnt_in_sstable", cur_sstable->get_data_macro_block_count(),
              K(meta_tree.get_macro_block_cnt()), "sstable_end_key", OB_ISNULL(copied_meta) ? "NOT_EXIST": to_cstring(copied_meta->end_key_));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(meta_tree.get_sorted_meta_array(sorted_metas))) {
      LOG_WARN("get sorted meta array failed", K(ret));
    } else {
      int64_t sstable_checksum = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < sorted_metas.count(); ++i) {
        const ObDataMacroBlockMeta *cur_macro_meta = sorted_metas.at(i);
        sstable_checksum = ob_crc64_sse42(sstable_checksum, &cur_macro_meta->val_.data_checksum_, sizeof(cur_macro_meta->val_.data_checksum_));
        FLOG_INFO("sorted meta array", K(i), "macro_block_id", cur_macro_meta->get_macro_id(), "data_checksum", cur_macro_meta->val_.data_checksum_, K(sstable_checksum), "macro_block_end_key", cur_macro_meta->end_key_);
      }
    }
  }
  return ret;
}


int compact_sstables(
    ObTablet &tablet,
    ObIArray<ObSSTable *> &sstables,
    const ObTabletDDLParam &ddl_param,
    const ObITableReadInfo &read_info,
    ObArenaAllocator &allocator,
    ObTableHandleV2 &sstable_handle)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator arena("compact_sst", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObBlockMetaTree meta_tree;
  ObArray<const ObDataMacroBlockMeta *> sorted_metas;
  if (OB_FAIL(meta_tree.init(tablet, ddl_param.table_key_, ddl_param.start_scn_, ddl_param.data_format_version_))) {
    LOG_WARN("init meta tree failed", K(ret), K(ddl_param));
  } else if (OB_FAIL(get_sorted_meta_array(sstables, read_info, meta_tree, arena, sorted_metas))) {
    LOG_WARN("get sorted meta array failed", K(ret), K(read_info), K(sstables));
  } else if (OB_FAIL(ObTabletDDLUtil::create_ddl_sstable(
          tablet,
          ddl_param,
          sorted_metas,
          sstables.empty() ? nullptr : sstables.at(0)/*first ddl sstable*/,
          allocator,
          sstable_handle))) {
    LOG_WARN("create sstable failed", K(ret), K(ddl_param), K(sstables));
  }
  LOG_DEBUG("compact_sstables", K(ret), K(sstables), K(ddl_param), K(read_info), KPC(sstable_handle.get_table()));
  return ret;
}

int compact_co_ddl_sstable(
    ObTablet &tablet,
    ObTableStoreIterator &ddl_sstable_iter,
    const ObIArray<ObDDLKVHandle> &frozen_ddl_kvs,
    const ObTabletDDLParam &ddl_param,
    common::ObArenaAllocator &allocator,
    ObTablesHandleArray &compacted_cg_sstable_handles,
    ObTableHandleV2 &co_sstable_handle)
{
  int ret = OB_SUCCESS;
  compacted_cg_sstable_handles.reset();
  co_sstable_handle.reset();
  const ObITableReadInfo *cg_index_read_info = nullptr;
  ObArenaAllocator arena("compact_co_ddl", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObStorageSchema *storage_schema = nullptr;
  if (OB_UNLIKELY(ddl_sstable_iter.count() == 0 && frozen_ddl_kvs.count() == 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_sstable_iter.count()), K(frozen_ddl_kvs.count()));
  } else if (OB_FAIL(tablet.load_storage_schema(arena, storage_schema))) {
    LOG_WARN("load storage schema failed", K(ret), K(ddl_param));
  } else {
    const int64_t base_cg_idx = ddl_param.table_key_.get_column_group_id();
    ObArray<ObSSTable *> base_sstables;
    ObTabletDDLParam cg_ddl_param = ddl_param;
    bool need_fill_cg_sstables = true;
    if (OB_FAIL(get_sstables(ddl_sstable_iter, base_cg_idx, base_sstables))) {
      LOG_WARN("get base sstable from ddl sstables failed", K(ret), K(ddl_sstable_iter), K(base_cg_idx));
    } else if (OB_FAIL(get_sstables(frozen_ddl_kvs, base_cg_idx, base_sstables))) {
      LOG_WARN("get base sstable from ddl kv array failed", K(ret), K(frozen_ddl_kvs), K(base_cg_idx));
    } else if (OB_FAIL(compact_sstables(tablet, base_sstables, ddl_param, tablet.get_rowkey_read_info(), allocator, co_sstable_handle))) {
      LOG_WARN("compact base sstable failed", K(ret));
    } else {
      // empty major co sstable, no need fill cg sstables
      need_fill_cg_sstables = !static_cast<ObCOSSTableV2 *>(co_sstable_handle.get_table())->is_empty_co_table();
    }
    if (OB_SUCC(ret) && need_fill_cg_sstables) {
      if (OB_FAIL(MTL(ObTenantCGReadInfoMgr *)->get_index_read_info(cg_index_read_info))) {
        LOG_WARN("failed to get index read info from ObTenantCGReadInfoMgr", K(ret));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < storage_schema->get_column_group_count(); ++i) {
        const int64_t cur_cg_idx = i;
        ObArray<ObSSTable *> cur_cg_sstables;
        ObTableHandleV2 target_table_handle;
        cg_ddl_param.table_key_.table_type_ = ObITable::TableType::DDL_MERGE_CO_SSTABLE == ddl_param.table_key_.table_type_
          ? ObITable::TableType::DDL_MERGE_CG_SSTABLE : ObITable::TableType::NORMAL_COLUMN_GROUP_SSTABLE;
        cg_ddl_param.table_key_.column_group_idx_ = cur_cg_idx;
        if (cur_cg_idx == base_cg_idx) {
          // do nothing
        } else if (OB_FAIL(get_sstables(ddl_sstable_iter, cur_cg_idx, cur_cg_sstables))) {
          LOG_WARN("get current cg sstables failed", K(ret));
        } else if (OB_FAIL(get_sstables(frozen_ddl_kvs, cur_cg_idx, cur_cg_sstables))) {
          LOG_WARN("get current cg sstables failed", K(ret));
        } else if (OB_FAIL(compact_sstables(tablet, cur_cg_sstables, cg_ddl_param, *cg_index_read_info, allocator, target_table_handle))) {
          LOG_WARN("compact cg sstable failed", K(ret), K(cur_cg_idx), K(cur_cg_sstables.count()), K(cg_ddl_param), KPC(cg_index_read_info));
        } else if (OB_FAIL(compacted_cg_sstable_handles.add_table(target_table_handle))) {
          LOG_WARN("push back compacted cg sstable failed", K(ret), K(i), KP(target_table_handle.get_table()));
        }
      }
      if (OB_SUCC(ret)) { // assemble the cg sstables into co sstable
        ObArray<ObITable *> cg_sstables;
        if (OB_FAIL(compacted_cg_sstable_handles.get_tables(cg_sstables))) {
          LOG_WARN("get cg sstables failed", K(ret));
        } else if (OB_FAIL(static_cast<ObCOSSTableV2 *>(co_sstable_handle.get_table())->fill_cg_sstables(cg_sstables))) {
          LOG_WARN("fill cg sstables failed", K(ret));
        }
      }
    }
  }
  ObTabletObjLoadHelper::free(arena, storage_schema);
  LOG_INFO("compact_co_ddl_sstable", K(ret), K(ddl_sstable_iter), K(ddl_param), KP(&tablet), KPC(co_sstable_handle.get_table()));
  return ret;
}

int compact_ro_ddl_sstable(
    ObTablet &tablet,
    ObTableStoreIterator &ddl_sstable_iter,
    const ObIArray<ObDDLKVHandle> &frozen_ddl_kvs,
    const ObTabletDDLParam &ddl_param,
    common::ObArenaAllocator &allocator,
    ObTableHandleV2 &ro_sstable_handle)
{
  int ret = OB_SUCCESS;
  ro_sstable_handle.reset();
  if (OB_UNLIKELY(ddl_sstable_iter.count() == 0 && frozen_ddl_kvs.count() == 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_sstable_iter.count()), K(frozen_ddl_kvs.count()));
  } else {
    const int64_t base_cg_idx = -1; // negative value means row store
    ObArray<ObSSTable *> base_sstables;
    if (OB_FAIL(get_sstables(ddl_sstable_iter, base_cg_idx, base_sstables))) {
      LOG_WARN("get base sstable from ddl sstables failed", K(ret), K(ddl_sstable_iter), K(base_cg_idx));
    } else if (OB_FAIL(get_sstables(frozen_ddl_kvs, base_cg_idx, base_sstables))) {
      LOG_WARN("get base sstable from ddl kv array failed", K(ret), K(frozen_ddl_kvs), K(base_cg_idx));
    } else if (OB_FAIL(compact_sstables(tablet, base_sstables, ddl_param, tablet.get_rowkey_read_info(), allocator, ro_sstable_handle))) {
      LOG_WARN("compact base sstable failed", K(ret));
    }
  }
  LOG_INFO("compact_ro_ddl_sstable", K(ret), K(ddl_sstable_iter), K(ddl_param), KP(&tablet), KPC(ro_sstable_handle.get_table()));
  return ret;
}

int ObTabletDDLUtil::compact_ddl_kv(
    ObLS &ls,
    ObTablet &tablet,
    ObTableStoreIterator &ddl_sstable_iter,
    const ObIArray<ObDDLKVHandle> &frozen_ddl_kvs,
    const ObTabletDDLParam &ddl_param,
    common::ObArenaAllocator &allocator,
    ObTableHandleV2 &compacted_sstable_handle)
{
  int ret = OB_SUCCESS;
  compacted_sstable_handle.reset();
  ObArenaAllocator arena("compact_ddl_kv", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObArray<const ObDataMacroBlockMeta *> sorted_metas;
  bool is_data_continue = true;
  ObTablesHandleArray compacted_cg_sstable_handles; // for tmp hold handle of macro block until the tablet updated
  if (OB_UNLIKELY(!ddl_param.is_valid() || (0 == ddl_sstable_iter.count() && frozen_ddl_kvs.empty()))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_param), K(ddl_sstable_iter.count()), K(frozen_ddl_kvs.count()));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < frozen_ddl_kvs.count(); ++i) {
      if (OB_FAIL(frozen_ddl_kvs.at(i).get_obj()->close())) {
        LOG_WARN("close ddl kv failed", K(ret), K(i));
      }
    }

#ifdef ERRSIM
    if (OB_SUCC(ret) && ddl_param.table_key_.is_major_sstable()) {
      ret = OB_E(EventTable::EN_DDL_COMPACT_FAIL) OB_SUCCESS;
      if (OB_FAIL(ret)) {
        LOG_WARN("errsim compact ddl sstable failed", KR(ret));
      }
    }
#endif

    if (OB_FAIL(ret)) {
    } else if (ddl_param.table_key_.is_co_sstable()) {
      if (OB_FAIL(compact_co_ddl_sstable(tablet, ddl_sstable_iter, frozen_ddl_kvs, ddl_param, allocator, compacted_cg_sstable_handles, compacted_sstable_handle))) {
        LOG_WARN("compact co ddl sstable failed", K(ret), K(ddl_param));
      }
    } else {
      if (OB_FAIL(compact_ro_ddl_sstable(tablet, ddl_sstable_iter, frozen_ddl_kvs, ddl_param, allocator, compacted_sstable_handle))) {
        LOG_WARN("compact co ddl sstable failed", K(ret), K(ddl_param));
      }
    }
    if (OB_SUCC(ret)) { // update table store
      if (OB_FAIL(update_ddl_table_store(ls, tablet, ddl_param, allocator, static_cast<ObSSTable *>(compacted_sstable_handle.get_table())))) {
        LOG_WARN("update ddl table store failed", K(ret));
      } else {
        LOG_INFO("compact ddl sstable success", K(ddl_param));
      }
    }
  }
  return ret;
}

int check_ddl_sstable_expired(const SCN &ddl_start_scn, ObTableStoreIterator &ddl_sstable_iter)
{
  int ret = OB_SUCCESS;
  ObITable *table = nullptr;
  ObSSTable *ddl_sstable = nullptr;
  ObSSTableMetaHandle meta_handle;
  if (0 == ddl_sstable_iter.count()) {
    // do nothing
  } else if (OB_FAIL(ddl_sstable_iter.get_boundary_table(false, table))) {
    LOG_WARN("get first ddl sstable failed", K(ret));
  } else if (OB_ISNULL(ddl_sstable = static_cast<ObSSTable *>(table))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ddl sstable is null", K(ret), KPC(table));
  } else if (OB_FAIL(ddl_sstable->get_meta(meta_handle))) {
    LOG_WARN("get meta handle failed", K(ret));
  } else if (meta_handle.get_sstable_meta().get_ddl_scn() < ddl_start_scn) {
    ret = OB_TASK_EXPIRED;
    LOG_WARN("ddl sstable is expired", K(ret), K(meta_handle.get_sstable_meta()), K(ddl_start_scn));
  }
  return ret;
}

int check_ddl_kv_expired(const SCN &ddl_start_scn, const ObIArray<ObDDLKVHandle> &frozen_ddl_kvs)
{
  int ret = OB_SUCCESS;
  ObDDLKV *ddl_kv = nullptr;
  if (frozen_ddl_kvs.empty()) {
    // do nothing
  } else if (OB_ISNULL(ddl_kv = frozen_ddl_kvs.at(0).get_obj())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ddl kv is null", K(ret), K(frozen_ddl_kvs));
  } else if (ddl_kv->get_ddl_start_scn() < ddl_start_scn) {
    ret = OB_TASK_EXPIRED;
    LOG_WARN("ddl sstable is expired", K(ret), KPC(ddl_kv), K(ddl_start_scn));
  }
  return ret;
}

int ObTabletDDLUtil::get_compact_scn(
    const SCN &ddl_start_scn,
    ObTableStoreIterator &ddl_sstable_iter,
    const ObIArray<ObDDLKVHandle> &frozen_ddl_kvs,
    SCN &compact_start_scn,
    SCN &compact_end_scn)
{
  int ret = OB_SUCCESS;
  bool is_data_continue = true;
  compact_start_scn = SCN::max_scn();
  compact_end_scn = SCN::min_scn();
  ddl_sstable_iter.resume();
  if (OB_UNLIKELY((0 == ddl_sstable_iter.count() && frozen_ddl_kvs.empty()))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ddl_sstable_iter.count()), K(frozen_ddl_kvs.count()));
  } else if (OB_FAIL(check_ddl_sstable_expired(ddl_start_scn, ddl_sstable_iter))) {
    LOG_WARN("check ddl sstable expired failed", K(ret), K(ddl_start_scn), K(ddl_sstable_iter));
  } else if (ddl_sstable_iter.count() > 0 && OB_FAIL(check_data_continue(ddl_sstable_iter, is_data_continue, compact_start_scn, compact_end_scn))) {
    LOG_WARN("check ddl sstable continue failed", K(ret));
  } else if (!is_data_continue) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ddl sstable not continuous", K(ret), K(ddl_sstable_iter));
  } else if (OB_FAIL(check_ddl_kv_expired(ddl_start_scn, frozen_ddl_kvs))) {
    LOG_WARN("check ddl kv expired failed", K(ret), K(ddl_start_scn), K(frozen_ddl_kvs));
  } else if (frozen_ddl_kvs.count() > 0 && OB_FAIL(check_data_continue(frozen_ddl_kvs, is_data_continue, compact_start_scn, compact_end_scn))) {
    LOG_WARN("check ddl sstable continue failed", K(ret));
  } else if (!is_data_continue) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ddl kv not continuous", K(ret), K(frozen_ddl_kvs));
  } else if (ddl_sstable_iter.count() > 0 && frozen_ddl_kvs.count() > 0) {
    ObITable *first_ddl_sstable = nullptr;
    ObITable *last_ddl_sstable = nullptr;
    ObDDLKVHandle first_ddl_kv_handle = frozen_ddl_kvs.at(0);
    ObDDLKVHandle last_ddl_kv_handle = frozen_ddl_kvs.at(frozen_ddl_kvs.count() - 1);
    if (OB_FAIL(ddl_sstable_iter.get_boundary_table(false/*is_last*/, first_ddl_sstable))) {
      LOG_WARN("get last ddl sstable failed", K(ret));
    } else if (OB_FAIL(ddl_sstable_iter.get_boundary_table(true/*is_last*/, last_ddl_sstable))) {
      LOG_WARN("get last ddl sstable failed", K(ret));
    } else {
      // |___________________________________________________|
      // fisrt_ddl_sstable.start_scn                         last_ddl_sstable.end_scn
      //                                 |____________________________________________________________|
      //                                 first_ddl_kv.start_scn                                       last_ddl_kv.end_scn
      is_data_continue = first_ddl_kv_handle.get_obj()->get_start_scn() >= first_ddl_sstable->get_start_scn()
        && first_ddl_kv_handle.get_obj()->get_start_scn() <= last_ddl_sstable->get_end_scn()
        && last_ddl_kv_handle.get_obj()->get_end_scn() >= last_ddl_sstable->get_end_scn();
      if (!is_data_continue) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("scn range not continue between ddl sstable iter and ddl kv array", K(ret), K(ddl_sstable_iter), K(frozen_ddl_kvs));
      }
    }
  }
  return ret;
}

int ObTabletDDLUtil::report_ddl_checksum(
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id,
    const uint64_t table_id,
    const int64_t execution_id,
    const int64_t ddl_task_id,
    const int64_t *column_checksums,
    const int64_t column_count)
{
  int ret = OB_SUCCESS;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table_schema = nullptr;
  const uint64_t tenant_id = MTL_ID();
  if (OB_UNLIKELY(!tablet_id.is_valid() || OB_INVALID_ID == ddl_task_id
        || !is_valid_id(table_id) || 0 == table_id || execution_id < 0 || nullptr == column_checksums || column_count <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tablet_id), K(table_id), K(execution_id), KP(column_checksums), K(column_count));
  } else if (!is_valid_tenant_id(tenant_id) || OB_ISNULL(sql_proxy) || OB_ISNULL(schema_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("ls service or sql proxy is null", K(ret), K(tenant_id), KP(sql_proxy), KP(schema_service));
  } else if (OB_FAIL(schema_service->get_tenant_schema_guard(tenant_id, schema_guard))) {
    LOG_WARN("get tenant schema guard failed", K(ret), K(tenant_id));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id, table_id, table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(tenant_id), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_INFO("table not exit", K(ret), K(tenant_id), K(table_id));
    ret = OB_TASK_EXPIRED; // for ignore warning
  } else if (OB_FAIL(DDL_SIM(tenant_id, ddl_task_id, REPORT_DDL_CHECKSUM_FAILED))) {
    LOG_WARN("ddl sim failure", K(tenant_id), K(ddl_task_id));
  } else {
    ObArray<ObColDesc> column_ids;
    ObArray<ObDDLChecksumItem> ddl_checksum_items;
    if (OB_FAIL(table_schema->get_multi_version_column_descs(column_ids))) {
      LOG_WARN("fail to get column ids", K(ret), K(tablet_id));
    } else if (OB_UNLIKELY(column_count > column_ids.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpect error, column checksums count larger than column ids count", K(ret),
          K(tablet_id), K(column_count), K(column_ids.count()));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
      share::ObDDLChecksumItem item;
      item.execution_id_ = execution_id;
      item.tenant_id_ = tenant_id;
      item.table_id_ = table_id;
      item.ddl_task_id_ = ddl_task_id;
      item.column_id_ = column_ids.at(i).col_id_;
      item.task_id_ = tablet_id.id();
      item.checksum_ = column_checksums[i];
#ifdef ERRSIM
      if (OB_SUCC(ret)) {
        ret = OB_E(EventTable::EN_HIDDEN_CHECKSUM_DDL_TASK) OB_SUCCESS;
        // set the checksum of the second column inconsistent with the report checksum of data table. (report_ddl_column_checksum())
        if (OB_FAIL(ret) && 17 == item.column_id_) {
          item.checksum_ = i + 100;
        }
      }
#endif
      if (item.column_id_ >= OB_MIN_SHADOW_COLUMN_ID ||
          item.column_id_ == OB_HIDDEN_TRANS_VERSION_COLUMN_ID ||
          item.column_id_ == OB_HIDDEN_SQL_SEQUENCE_COLUMN_ID) {
        continue;
      } else if (OB_FAIL(ddl_checksum_items.push_back(item))) {
        LOG_WARN("push back column checksum item failed", K(ret));
      }
    }
#ifdef ERRSIM
    if (OB_SUCC(ret)) {
      ret = OB_E(EventTable::EN_DDL_REPORT_CHECKSUM_FAIL) OB_SUCCESS;
      if (OB_FAIL(ret)) {
        LOG_WARN("errsim report checksum failed", KR(ret));
      }
    }
#endif
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObDDLChecksumOperator::update_checksum(ddl_checksum_items, *sql_proxy))) {
      LOG_WARN("fail to update checksum", K(ret), K(tablet_id), K(table_id), K(ddl_checksum_items));
    } else {
      LOG_INFO("report ddl checkum success", K(tablet_id), K(table_id), K(execution_id), K(ddl_checksum_items));
    }
  }
  return ret;
}

int ObTabletDDLUtil::check_and_get_major_sstable(const share::ObLSID &ls_id,
                                                 const ObTabletID &tablet_id,
                                                 const blocksstable::ObSSTable *&first_major_sstable,
                                                 ObTabletMemberWrapper<ObTabletTableStore> &table_store_wrapper)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  first_major_sstable = nullptr;
  if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(ls_id));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                               tablet_id,
                                               tablet_handle,
                                               ObMDSGetTabletMode::READ_ALL_COMMITED))) {
    LOG_WARN("get tablet handle failed", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_UNLIKELY(nullptr == tablet_handle.get_obj())) {
    ret = OB_ERR_SYS;
    LOG_WARN("tablet handle is null", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(tablet_handle.get_obj()->fetch_table_store(table_store_wrapper))) {
    LOG_WARN("fail to fetch table store", K(ret));
  } else {
    first_major_sstable = static_cast<ObSSTable *>(
        table_store_wrapper.get_member()->get_major_sstables().get_boundary_table(false/*first*/));
  }
  return ret;
}

int ObTabletDDLUtil::freeze_ddl_kv(const ObDDLTableMergeDagParam &param)
{
  int ret = OB_SUCCESS;
  ObLSService *ls_service = MTL(ObLSService *);
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObDDLKvMgrHandle ddl_kv_mgr_handle;
  ObArray<ObDDLKVHandle> ddl_kvs_handle;
  ObDDLTableMergeTask *merge_task = nullptr;
  ObTenantDirectLoadMgr *tenant_direct_load_mgr = MTL(ObTenantDirectLoadMgr *);
  ObTabletDirectLoadMgrHandle tablet_mgr_hdl;
  if (OB_FAIL(ls_service->get_ls(param.ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(param));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                               param.tablet_id_,
                                               tablet_handle,
                                               ObMDSGetTabletMode::READ_ALL_COMMITED))) {
    LOG_WARN("get tablet failed", K(ret), K(param));
  } else if (OB_UNLIKELY(!tablet_handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), K(param));
  } else if (OB_FAIL(tablet_handle.get_obj()->get_ddl_kv_mgr(ddl_kv_mgr_handle))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      ret = OB_TASK_EXPIRED;
      LOG_INFO("ddl kv mgr not exist", K(ret), K(param));
    } else {
      LOG_WARN("get ddl kv mgr failed", K(ret), K(param));
    }
  } else if (is_full_direct_load(param.direct_load_type_)
      && param.start_scn_ < tablet_handle.get_obj()->get_tablet_meta().ddl_start_scn_) {
    ret = OB_TASK_EXPIRED;
    LOG_WARN("ddl task expired, skip it", K(ret), K(param), "new_start_scn", tablet_handle.get_obj()->get_tablet_meta().ddl_start_scn_);
  } else if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->freeze_ddl_kv(
      param.start_scn_, param.snapshot_version_, param.data_format_version_))) {
    LOG_WARN("ddl kv manager try freeze failed", K(ret), K(param));
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
