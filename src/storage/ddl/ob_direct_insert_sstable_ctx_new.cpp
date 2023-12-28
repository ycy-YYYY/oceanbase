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

#define USING_LOG_PREFIX STORAGE

#include "ob_direct_insert_sstable_ctx_new.h"
#include "share/ob_ddl_checksum.h"
#include "share/ob_ddl_error_message_table_operator.h"
#include "share/ob_ddl_common.h"
#include "share/ob_tablet_autoincrement_service.h"
#include "sql/engine/pdml/static/ob_px_sstable_insert_op.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/px/ob_sub_trans_ctrl.h"
#include "storage/blocksstable/index_block/ob_index_block_builder.h"
#include "storage/compaction/ob_schedule_dag_func.h"
#include "storage/compaction/ob_column_checksum_calculator.h"
#include "storage/compaction/ob_tenant_freeze_info_mgr.h"
#include "storage/ddl/ob_direct_load_struct.h"
#include "storage/ddl/ob_ddl_merge_task.h"
#include "storage/ddl/ob_ddl_redo_log_writer.h"
#include "storage/lob/ob_lob_util.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/column_store/ob_column_oriented_sstable.h"

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::storage;
using namespace oceanbase::blocksstable;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;
using namespace oceanbase::sql;

int64_t ObTenantDirectLoadMgr::generate_context_id()
{
  return ATOMIC_AAF(&context_id_generator_, 1);
}

ObTenantDirectLoadMgr::ObTenantDirectLoadMgr()
  : is_inited_(false), slice_id_generator_(0), context_id_generator_(0)
{
}

ObTenantDirectLoadMgr::~ObTenantDirectLoadMgr()
{
  destroy();
}

void ObTenantDirectLoadMgr::destroy()
{
  is_inited_ = false;
  int ret = OB_SUCCESS;
  bucket_lock_.destroy();
  common::ObArray<ObTabletDirectLoadMgrKey> tablet_mgr_keys;
  for (TABLET_MGR_MAP::const_iterator iter = tablet_mgr_map_.begin();
        iter != tablet_mgr_map_.end(); ++iter) {
    if (OB_FAIL(tablet_mgr_keys.push_back(iter->first))) {
      LOG_WARN("push back failed", K(ret));
    }
  }
  for (int64_t i = 0; i < tablet_mgr_keys.count(); i++) {
    if (OB_FAIL(remove_tablet_direct_load(tablet_mgr_keys.at(i)))) {
      LOG_WARN("remove tablet mgr failed", K(ret), K(tablet_mgr_keys.at(i)));
    }
  }
  allocator_.reset();
}

int64_t ObTenantDirectLoadMgr::generate_slice_id()
{
  return ATOMIC_AAF(&slice_id_generator_, 1);
}

int ObTenantDirectLoadMgr::mtl_init(ObTenantDirectLoadMgr *&tenant_direct_load_mgr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(tenant_direct_load_mgr)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error", K(ret));
  } else if (OB_FAIL(tenant_direct_load_mgr->init())) {
    LOG_WARN("init failed", K(ret));
  }
  return ret;
}

int ObTenantDirectLoadMgr::init()
{
  int ret = OB_SUCCESS;
  const uint64_t tenant_id = MTL_ID();
  const int64_t bucket_num = 1000L * 100L; // 10w
  const int64_t memory_limit = 1024L * 1024L * 1024L * 10L; // 10GB
  lib::ObMemAttr attr(tenant_id, "TenantDLMgr");
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tenant_id));
  } else if (OB_FAIL(allocator_.init(OB_MALLOC_MIDDLE_BLOCK_SIZE,
    attr.label_, tenant_id, memory_limit))) {
    LOG_WARN("init alloctor failed", K(ret));
  } else if (OB_FAIL(bucket_lock_.init(bucket_num, ObLatchIds::TENANT_DIRECT_LOAD_MGR_LOCK,
      ObLabel("TenDLBucket"), tenant_id))) {
    LOG_WARN("init bucket lock failed", K(ret), K(bucket_num));
  } else if (OB_FAIL(tablet_mgr_map_.create(bucket_num, attr, attr))) {
    LOG_WARN("create context map failed", K(ret));
  } else if (OB_FAIL(tablet_exec_context_map_.create(bucket_num, attr, attr))) {
    LOG_WARN("create context map failed", K(ret));
  } else {
    allocator_.set_attr(attr);
    slice_id_generator_ = ObTimeUtility::current_time();
    is_inited_ = true;
  }
  return ret;
}

// 1. Leader create it when start tablet direct load task;
// 2. Follower create it before replaying start log;
// 3. Migrate/Rebuild create tablet/ LS online create it.
int ObTenantDirectLoadMgr::create_tablet_direct_load(
    const int64_t context_id,
    const int64_t execution_id,
    const ObTabletDirectLoadInsertParam &build_param,
    const share::SCN checkpoint_scn)
{
  int ret = OB_SUCCESS;
  ObLSService *ls_service = nullptr;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObTabletBindingMdsUserData ddl_data;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!build_param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(context_id), K(build_param));
  } else if (OB_ISNULL(ls_service = MTL(ObLSService *))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), K(MTL_ID()));
  } else if (OB_FAIL(ls_service->get_ls(build_param.common_param_.ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(build_param));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, build_param.common_param_.tablet_id_,
    tablet_handle, ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
    LOG_WARN("get tablet handle failed", K(ret), K(build_param));
  } else if (OB_FAIL(tablet_handle.get_obj()->ObITabletMdsInterface::get_ddl_data(share::SCN::max_scn(), ddl_data))) {
    LOG_WARN("failed to get ddl data from tablet", K(ret), K(tablet_handle));
  } else {
    ObTabletHandle lob_tablet_handle;
    ObTabletMemberWrapper<ObTabletTableStore> table_store_wrapper;
    ObTabletMemberWrapper<ObTabletTableStore> lob_store_wrapper;
    ObTabletDirectLoadMgrHandle data_tablet_direct_load_mgr_handle;
    ObTabletDirectLoadMgrHandle lob_tablet_direct_load_mgr_handle;
    data_tablet_direct_load_mgr_handle.reset();
    lob_tablet_direct_load_mgr_handle.reset();
    const bool is_full_direct_load_task = is_full_direct_load(build_param.common_param_.direct_load_type_);
    const ObTabletID &lob_meta_tablet_id = ddl_data.lob_meta_tablet_id_;
    if (!lob_meta_tablet_id.is_valid() || checkpoint_scn.is_valid_and_not_min()) {
      // has no lob, or recover from checkpoint.
      LOG_DEBUG("do not create lob mgr handle when create data tablet mgr", K(ret), K(lob_meta_tablet_id), K(checkpoint_scn),
        K(build_param));
    } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, lob_meta_tablet_id,
      lob_tablet_handle, ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
      LOG_WARN("get tablet handle failed", K(ret), K(lob_meta_tablet_id));
    } else if (OB_FAIL(lob_tablet_handle.get_obj()->fetch_table_store(lob_store_wrapper))) {
      LOG_WARN("fail to fetch table store", K(ret));
    } else if (OB_FAIL(try_create_tablet_direct_load_mgr(context_id, execution_id,
        nullptr != lob_store_wrapper.get_member()->get_major_sstables().get_boundary_table(false/*first*/),
        allocator_, ObTabletDirectLoadMgrKey(lob_meta_tablet_id, is_full_direct_load_task), true /*is lob tablet*/,
        lob_tablet_direct_load_mgr_handle))) {
      LOG_WARN("try create data tablet direct load mgr failed", K(ret), K(build_param));
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(tablet_handle.get_obj()->fetch_table_store(table_store_wrapper))) {
      LOG_WARN("fetch table store failed", K(ret));
    } else if (OB_FAIL(try_create_tablet_direct_load_mgr(context_id, execution_id,
        nullptr != table_store_wrapper.get_member()->get_major_sstables().get_boundary_table(false/*first*/),
        allocator_, ObTabletDirectLoadMgrKey(build_param.common_param_.tablet_id_, is_full_direct_load_task), false /*is lob tablet*/,
        data_tablet_direct_load_mgr_handle))) {
      // Newly-allocated Lob meta tablet direct load mgr will be cleanuped when tablet gc task works.
      LOG_WARN("try create data tablet direct load mgr failed", K(ret), K(build_param));
    }

    if (OB_FAIL(ret)) {
    } else if (data_tablet_direct_load_mgr_handle.is_valid()) {
      if (OB_FAIL(data_tablet_direct_load_mgr_handle.get_obj()->update(
          lob_tablet_direct_load_mgr_handle.get_obj(), build_param))) {
        LOG_WARN("init tablet mgr failed", K(ret), K(build_param));
      }
    }
  }
  return ret;
}

int ObTenantDirectLoadMgr::try_create_tablet_direct_load_mgr(
    const int64_t context_id,
    const int64_t execution_id,
    const bool major_sstable_exist,
    ObIAllocator &allocator,
    const ObTabletDirectLoadMgrKey &mgr_key,
    const bool is_lob_tablet_mgr,
    ObTabletDirectLoadMgrHandle &direct_load_mgr_handle)
{
  int ret = OB_SUCCESS;
  direct_load_mgr_handle.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!mgr_key.is_valid()) || execution_id < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(mgr_key), K(execution_id));
  } else {
    ObTabletDirectLoadMgr *direct_load_mgr = nullptr;
    ObBucketHashWLockGuard guard(bucket_lock_, mgr_key.hash());
    if (OB_FAIL(get_tablet_mgr_no_lock(mgr_key, direct_load_mgr_handle))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("get refactored failed", K(ret), K(is_full_direct_load), K(mgr_key));
      }
    } else if (OB_ISNULL(direct_load_mgr = direct_load_mgr_handle.get_obj())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(mgr_key));
    }
    if (OB_SUCC(ret) && !major_sstable_exist) {
      if (nullptr == direct_load_mgr) {
        void *buf = nullptr;
        const int64_t buf_size = mgr_key.is_full_direct_load_ ?
            sizeof(ObTabletFullDirectLoadMgr) : sizeof(ObTabletIncDirectLoadMgr);
        if (OB_ISNULL(buf = allocator.alloc(buf_size))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("alloc memory failed", K(ret), K(mgr_key));
        } else if (mgr_key.is_full_direct_load_) {
          direct_load_mgr = new (buf) ObTabletFullDirectLoadMgr();
        } else {
          direct_load_mgr = new (buf) ObTabletIncDirectLoadMgr();
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(direct_load_mgr_handle.set_obj(direct_load_mgr))) {
          LOG_WARN("set direct load mgr failed", K(ret));
        } else if (OB_FAIL(tablet_mgr_map_.set_refactored(mgr_key, direct_load_mgr))) {
          LOG_WARN("set tablet mgr failed", K(ret));
        } else {
          direct_load_mgr->inc_ref();
          LOG_INFO("create tablet direct load mgr", K(mgr_key), K(execution_id), K(major_sstable_exist));
        }
        // cleanup if failed.
        if (OB_FAIL(ret)) {
          if (nullptr != direct_load_mgr) {
            direct_load_mgr->~ObTabletDirectLoadMgr();
            direct_load_mgr = nullptr;
          }
          if (buf != nullptr) {
            allocator.free(buf);
            buf = nullptr;
          }
        }
      }
    }
    if (OB_SUCC(ret) && context_id >= 0 && !is_lob_tablet_mgr) { // only build execution context map for data tablet
      ObTabletDirectLoadExecContextId exec_id;
      ObTabletDirectLoadExecContext exec_context;
      exec_id.tablet_id_ = mgr_key.tablet_id_;
      exec_id.context_id_ = context_id;
      exec_context.execution_id_ = execution_id;
      exec_context.start_scn_.reset();
      if (OB_FAIL(tablet_exec_context_map_.set_refactored(exec_id, exec_context, true /*overwrite*/))) {
        LOG_WARN("get table execution context failed", K(ret), K(exec_id));
      }
    }
  }
  return ret;
}

int ObTenantDirectLoadMgr::alloc_execution_context_id(
    int64_t &context_id)
{
  int ret = OB_SUCCESS;
  context_id = generate_context_id();
  return ret;
}

int ObTenantDirectLoadMgr::open_tablet_direct_load(
    const bool is_full_direct_load,
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id,
    const int64_t context_id,
    SCN &start_scn,
    ObTabletDirectLoadMgrHandle &handle)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadExecContextId exec_id;
  ObTabletDirectLoadExecContext exec_context;
  exec_id.tablet_id_ = tablet_id;
  exec_id.context_id_ = context_id;
  ObTabletDirectLoadMgrKey mgr_key(tablet_id, is_full_direct_load);
  bool is_mgr_exist = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || context_id < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(context_id));
  } else if (OB_FAIL(get_tablet_mgr(tablet_id, is_full_direct_load, handle))) {
    if (OB_ENTRY_NOT_EXIST == ret && is_full_direct_load) {
      if (OB_FAIL(check_and_process_finished_tablet(ls_id, tablet_id))) {
        LOG_WARN("check and report checksum if need failed", K(ret), K(ls_id), K(tablet_id));
      }
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(tablet_id), K(is_full_direct_load));
    }
  } else {
    is_mgr_exist = true;
  }

  if (OB_SUCC(ret)) {
    ObBucketHashRLockGuard guard(bucket_lock_, mgr_key.hash());
    if (OB_FAIL(tablet_exec_context_map_.get_refactored(exec_id, exec_context))) {
      LOG_WARN("get table execution context failed", K(ret), K(exec_id));
    }
  }

  if (OB_SUCC(ret) && is_mgr_exist) {
    if (OB_FAIL(handle.get_obj()->open(exec_context.execution_id_, start_scn))) {
      LOG_WARN("update tablet direct load failed", K(ret), K(is_full_direct_load), K(tablet_id), K(exec_context));
    }
  }

  if (OB_SUCC(ret)) {
    ObBucketHashWLockGuard guard(bucket_lock_, mgr_key.hash());
    exec_context.start_scn_ = start_scn;
    if (OB_FAIL(tablet_exec_context_map_.set_refactored(exec_id, exec_context, true/*overwrite*/))) {
      LOG_WARN("get table execution context failed", K(ret), K(exec_id));
    }
  }
  return ret;
}

int ObTenantDirectLoadMgr::open_sstable_slice(
    const blocksstable::ObMacroDataSeq &start_seq,
    ObDirectLoadSliceInfo &slice_info)
{
  int ret = OB_SUCCESS;
  slice_info.slice_id_ = 0;
  ObTabletDirectLoadMgrHandle handle;
  const int64_t new_slice_id = generate_slice_id();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid() || !start_seq.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(slice_info), K(start_seq));
  } else if (OB_FAIL(get_tablet_mgr(slice_info.data_tablet_id_, slice_info.is_full_direct_load_, handle))) {
    if (OB_ENTRY_NOT_EXIST == ret && slice_info.is_full_direct_load_) {
      if (OB_FAIL(check_and_process_finished_tablet(slice_info.ls_id_, slice_info.data_tablet_id_))) {
        LOG_WARN("check and report checksum if need failed", K(ret), K(slice_info));
      }
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(slice_info));
    }
  } else if (OB_FAIL(handle.get_obj()->open_sstable_slice(
    slice_info.is_lob_slice_/*is_data_tablet_process_for_lob*/, start_seq, new_slice_id))) {
    LOG_WARN("open sstable slice failed", K(ret), K(slice_info));
  }
  if (OB_SUCC(ret)) {
    // To simplify the logic of TabletDirectLoadMgr,
    // unique slice id is generated here.
    slice_info.slice_id_ = new_slice_id;
  }
  return ret;
}

int ObTenantDirectLoadMgr::fill_sstable_slice(
    const ObDirectLoadSliceInfo &slice_info,
    ObIStoreRowIterator *iter,
    int64_t &affected_rows,
    ObInsertMonitor *insert_monitor)
{
  int ret = OB_SUCCESS;
  bool need_iter_part_row = false;
  ObTabletDirectLoadMgrHandle handle;
  ObTabletDirectLoadExecContext exec_context;
  ObTabletDirectLoadExecContextId exec_id;
  exec_id.tablet_id_ = slice_info.data_tablet_id_;
  exec_id.context_id_ = slice_info.context_id_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(slice_info));
  } else if (OB_FAIL(get_tablet_mgr(slice_info.data_tablet_id_, slice_info.is_full_direct_load_, handle))) {
    if (OB_ENTRY_NOT_EXIST == ret && slice_info.is_full_direct_load_) {
      need_iter_part_row = true;
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(slice_info));
    }
  } else if (OB_FAIL(tablet_exec_context_map_.get_refactored(exec_id, exec_context))) {
    LOG_WARN("get tablet execution context failed", K(ret));
  } else if (OB_FAIL(handle.get_obj()->fill_sstable_slice(slice_info, exec_context.start_scn_, iter, affected_rows, insert_monitor))) {
    if (OB_TRANS_COMMITED == ret && slice_info.is_full_direct_load_) {
      need_iter_part_row = true;
    } else {
      LOG_WARN("fill sstable slice failed", K(ret), K(slice_info));
    }
  }

  if (need_iter_part_row &&
      OB_FAIL(check_and_process_finished_tablet(slice_info.ls_id_, slice_info.data_tablet_id_, iter))) {
    LOG_WARN("check and report checksum if need failed", K(ret), K(slice_info));
  }
  return ret;
}

int ObTenantDirectLoadMgr::fill_lob_sstable_slice(
    ObIAllocator &allocator,
    const ObDirectLoadSliceInfo &slice_info,
    share::ObTabletCacheInterval &pk_interval,
    const ObArray<int64_t> &lob_column_idxs,
    const ObArray<common::ObObjMeta> &col_types,
    blocksstable::ObDatumRow &datum_row)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadMgrHandle handle;
  ObTabletDirectLoadExecContext exec_context;
  ObTabletDirectLoadExecContextId exec_id;
  exec_id.tablet_id_ = slice_info.data_tablet_id_;
  exec_id.context_id_ = slice_info.context_id_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(slice_info));
  } else if (OB_FAIL(get_tablet_mgr(slice_info.data_tablet_id_, slice_info.is_full_direct_load_, handle))) {
    if (OB_ENTRY_NOT_EXIST == ret && slice_info.is_full_direct_load_) {
      if (OB_FAIL(check_and_process_finished_tablet(slice_info.ls_id_, slice_info.data_tablet_id_))) {
        LOG_WARN("check and report checksum if need failed", K(ret), K(slice_info));
      }
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(slice_info));
    }
  } else if (OB_FAIL(tablet_exec_context_map_.get_refactored(exec_id, exec_context))) {
    LOG_WARN("get tablet execution context failed", K(ret));
  } else if (OB_FAIL(handle.get_obj()->fill_lob_sstable_slice(allocator, slice_info, exec_context.start_scn_, pk_interval, lob_column_idxs, col_types, datum_row))) {
    LOG_WARN("fail to fill batch sstable slice", KR(ret), K(slice_info), K(datum_row));
  }
  return ret;
}

int ObTenantDirectLoadMgr::calc_range(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const bool is_full_direct_load)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadMgrHandle handle;
  bool is_major_sstable_exist = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguement", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(get_tablet_mgr_and_check_major(ls_id, tablet_id, is_full_direct_load, handle, is_major_sstable_exist))) {
    if (OB_ENTRY_NOT_EXIST == ret && is_major_sstable_exist) {
      ret = OB_TASK_EXPIRED;
      LOG_INFO("direct load mgr not exist, but major sstable exist", K(ret), K(tablet_id));
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(tablet_id));
    }
  } else {
    ObStorageSchema *storage_schema = nullptr;
    ObLSHandle ls_handle;
    ObTabletHandle tablet_handle;
    ObArenaAllocator arena_allocator("DIRECT_RESCAN", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    bool is_column_store = false;
    if (OB_FAIL(MTL(ObLSService *)->get_ls(handle.get_obj()->get_ls_id(), ls_handle, ObLSGetMod::DDL_MOD))) {
      LOG_WARN("failed to get log stream", K(ret), K(handle), "ls_id", handle.get_obj()->get_ls_id());
    } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                                  tablet_id,
                                                  tablet_handle,
                                                  ObMDSGetTabletMode::READ_ALL_COMMITED))) {
      LOG_WARN("failed to get tablet", K(ret), "ls_id", handle.get_obj()->get_ls_id(), K(tablet_id));
    } else if (OB_FAIL(tablet_handle.get_obj()->load_storage_schema(arena_allocator, storage_schema))) {
      LOG_WARN("load storage schema failed", K(ret), K(tablet_id));
    } else if (OB_FAIL(ObCODDLUtil::need_column_group_store(*storage_schema, is_column_store))) {
      LOG_WARN("fail to check need column group", K(ret));
    } else if (OB_UNLIKELY(!is_column_store)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table withou cg", K(ret));
    } else if (OB_FAIL(handle.get_obj()->calc_range(storage_schema, tablet_handle.get_obj()->get_rowkey_read_info().get_datum_utils()))) {
      LOG_WARN("calc range failed", K(ret));
    }
    ObTabletObjLoadHelper::free(arena_allocator, storage_schema);
    arena_allocator.reset();
  }
  return ret;
}

int ObTenantDirectLoadMgr::fill_column_group(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const bool is_full_direct_load,
    const int64_t thread_cnt,
    const int64_t thread_id)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadMgrHandle handle;
  bool is_major_sstable_exist = false;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid() || thread_cnt <= 0 || thread_id < 0 || thread_id > thread_cnt - 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguement", K(ret), K(ls_id), K(tablet_id), K(thread_cnt), K(thread_id));
  } else if (OB_FAIL(get_tablet_mgr_and_check_major(ls_id, tablet_id, is_full_direct_load, handle, is_major_sstable_exist))) {
    if (OB_ENTRY_NOT_EXIST == ret && is_major_sstable_exist) {
      ret = OB_TASK_EXPIRED;
      LOG_INFO("direct load mgr not exist, but major sstable exist", K(ret), K(tablet_id));
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(tablet_id));
    }
  } else if (OB_FAIL(handle.get_obj()->fill_column_group(thread_cnt, thread_id))) {
    LOG_WARN("fill sstable slice failed", K(ret), K(thread_cnt), K(thread_id));
  }
  return ret;
}

int ObTenantDirectLoadMgr::cancel(
    const share::ObLSID &ls_id,
    const common::ObTabletID &tablet_id,
    const bool is_full_direct_load)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadMgrHandle handle;
  bool is_major_sstable_exist = false;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguement", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(get_tablet_mgr_and_check_major(ls_id, tablet_id, is_full_direct_load, handle, is_major_sstable_exist))) {
    if (OB_ENTRY_NOT_EXIST == ret && is_major_sstable_exist) {
      ret = OB_TASK_EXPIRED;
      LOG_INFO("direct load mgr not exist, but major sstable exist", K(ret), K(tablet_id));
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(tablet_id));
    }
  } else if (OB_FAIL(handle.get_obj()->cancel())) {
    LOG_WARN("cancel fill sstable slice failed", K(ret));
  }
  return ret;
}

int ObTenantDirectLoadMgr::close_sstable_slice(const ObDirectLoadSliceInfo &slice_info, ObInsertMonitor* insert_monitor)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadMgrHandle handle;
  ObTabletDirectLoadExecContext exec_context;
  ObTabletDirectLoadExecContextId exec_id;
  exec_id.tablet_id_ = slice_info.data_tablet_id_;
  exec_id.context_id_ = slice_info.context_id_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(slice_info));
  } else if (OB_FAIL(get_tablet_mgr(slice_info.data_tablet_id_, slice_info.is_full_direct_load_, handle))) {
    if (OB_ENTRY_NOT_EXIST == ret && slice_info.is_full_direct_load_) {
      if (OB_FAIL(check_and_process_finished_tablet(slice_info.ls_id_, slice_info.data_tablet_id_))) {
        LOG_WARN("check and report checksum if need failed", K(ret), K(slice_info));
      }
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(slice_info));
    }
  } else if (OB_FAIL(tablet_exec_context_map_.get_refactored(exec_id, exec_context))) {
    LOG_WARN("get tablet execution context failed", K(ret));
  } else if (OB_FAIL(handle.get_obj()->close_sstable_slice(
      slice_info.is_lob_slice_/*is_data_tablet_process_for_lob*/, slice_info, exec_context.start_scn_, exec_context.execution_id_, insert_monitor))) {
    LOG_WARN("close sstable slice failed", K(ret), K(slice_info), "execution_start_scn", exec_context.start_scn_, "execution_id", exec_context.execution_id_);
  }
  return ret;
}

int ObTenantDirectLoadMgr::close_tablet_direct_load(
    const int64_t context_id,
    const bool is_full_direct_load,
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id,
    const bool need_commit,
    const bool emergent_finish,
    const int64_t task_id,
    const int64_t table_id,
    const int64_t execution_id)
{
  int ret = OB_SUCCESS;
  UNUSED(emergent_finish);
  ObTabletDirectLoadMgrHandle handle;
  ObTabletDirectLoadMgrKey mgr_key(tablet_id, is_full_direct_load);
  ObTabletDirectLoadExecContextId exec_id;
  exec_id.tablet_id_ = tablet_id;
  exec_id.context_id_ = context_id;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || context_id < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tablet_id), K(context_id));
  } else if (OB_FAIL(get_tablet_mgr(tablet_id, is_full_direct_load, handle))) {
    if (OB_ENTRY_NOT_EXIST == ret && is_full_direct_load) {
      if (OB_FAIL(check_and_process_finished_tablet(ls_id, tablet_id, nullptr/*row_iterator*/, task_id, table_id, execution_id))) {
        LOG_WARN("check and report checksum if need failed", K(ret), K(ls_id), K(tablet_id), K(task_id), K(execution_id));
      }
    } else {
      LOG_WARN("get table mgr failed", K(ret), K(ls_id), K(tablet_id));
    }
  } else {
    if (need_commit) {
      ObTabletDirectLoadExecContext exec_context;
      {
        ObBucketHashRLockGuard guard(bucket_lock_, mgr_key.hash());
        if (OB_FAIL(tablet_exec_context_map_.get_refactored(exec_id, exec_context))) {
          LOG_WARN("get tablet execution context failed", K(ret));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(handle.get_obj()->close(exec_context.execution_id_, exec_context.start_scn_))) {
        LOG_WARN("close failed", K(ret));
      }
    } else {
      // For full/incremental direct load, the ObTabletDirectLoadMgr will be removed from MTL when,
      // 1. the direct load task abort indicated by `need_commit = false`, and we do not care about
      //    the error code triggered by the not found ObTabletDirectLoadMgr after.
      // 2. the direct load task commit and all ddl kvs persist successfully.

      // But how to notify the follower to remove it, with write commit failed log or tablet gc task ??
    }
    if (OB_SUCC(ret)) {
      ObBucketHashWLockGuard guard(bucket_lock_, mgr_key.hash());
      if (OB_FAIL(tablet_exec_context_map_.erase_refactored(exec_id))) {
        LOG_WARN("erase tablet execution context failed", K(ret), K(exec_id));
      } else {
        LOG_INFO("erase execution context", K(exec_id), K(tablet_id));
      }
    }
  }
  return ret;
}

// Other utils function.
int ObTenantDirectLoadMgr::get_online_stat_collect_result(
    const bool is_full_direct_load,
    const ObTabletID &tablet_id,
    const ObArray<ObOptColumnStat*> *&column_stat_array)
{
  int ret = OB_NOT_IMPLEMENT;
  // ObTableDirectLoadMgr *table_mgr = nullptr;
  // if (OB_FAIL(get_table_mgr(task_id, table_mgr))) {
  //   LOG_WARN("get context failed", K(ret));
  // } else if (OB_FAIL(table_mgr->get_online_stat_collect_result(tablet_id, column_stat_array))) {
  //   LOG_WARN("finish table context failed", K(ret));
  // }
  return ret;
}

int ObTenantDirectLoadMgr::get_tablet_cache_interval(
    const int64_t context_id,
    const ObTabletID &tablet_id,
    ObTabletCacheInterval &interval)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadMgrKey mgr_key(tablet_id, true/*full direct load*/);  // only support in ddl, which is full direct load
  ObBucketHashWLockGuard guard(bucket_lock_, mgr_key.hash());
  ObTabletAutoincrementService &autoinc_service = ObTabletAutoincrementService::get_instance();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(context_id < 0 || !tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(context_id), K(tablet_id));
  } else if (OB_FAIL(autoinc_service.get_tablet_cache_interval(MTL_ID(), interval))) {
    LOG_WARN("failed to get tablet cache intervals", K(ret));
  } else {
    ObTabletDirectLoadExecContext exec_context;
    ObTabletDirectLoadExecContextId exec_id;
    exec_id.tablet_id_ = tablet_id;
    exec_id.context_id_ = context_id;
    if (OB_FAIL(tablet_exec_context_map_.get_refactored(exec_id, exec_context))) {
      LOG_WARN("get tablet execution context failed", K(ret));
    } else {
      interval.task_id_ = exec_context.seq_interval_task_id_++;
      if (OB_FAIL(tablet_exec_context_map_.set_refactored(exec_id, exec_context, true/*overwrite*/))) {
        LOG_WARN("set tablet execution context map", K(ret));
      }
    }
  }

  return ret;
}

int get_co_column_checksums_if_need(
    ObTabletHandle &tablet_handle,
    const ObSSTable *sstable,
    ObIArray<int64_t> &column_checksum_array)
{
  int ret = OB_SUCCESS;
  column_checksum_array.reset();
  if (OB_UNLIKELY(!tablet_handle.is_valid() || nullptr == sstable)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tablet_handle), KP(sstable));
  } else if (!sstable->is_co_sstable()) {
    // do nothing
  } else {
    bool is_rowkey_based_co_sstable = false;
    ObStorageSchema *storage_schema = nullptr;
    ObArenaAllocator arena("co_ddl_cksm", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
    if (OB_FAIL(tablet_handle.get_obj()->load_storage_schema(arena, storage_schema))) {
      LOG_WARN("load storage schema failed", K(ret));
    } else if (OB_FAIL(ObCODDLUtil::is_rowkey_based_co_sstable(
            static_cast<const ObCOSSTableV2 *>(sstable), storage_schema, is_rowkey_based_co_sstable))) {
      LOG_WARN("check is rowkey based co sstable failed", K(ret));
    } else if (is_rowkey_based_co_sstable) {
      if (OB_FAIL(ObCODDLUtil::get_column_checksums(
                static_cast<const ObCOSSTableV2 *>(sstable),
                storage_schema,
                column_checksum_array))) {
        LOG_WARN("get column checksum from co sstable failed", K(ret));
      }
    }
    ObTabletObjLoadHelper::free(arena, storage_schema);
  }
  return ret;
}

int ObTenantDirectLoadMgr::check_and_process_finished_tablet(
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id,
    ObIStoreRowIterator *row_iter,
    const int64_t task_id,
    const int64_t table_id,
    const int64_t execution_id)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObSSTableMetaHandle sst_meta_hdl;
  const ObSSTable *first_major_sstable = nullptr;
  ObTabletMemberWrapper<ObTabletTableStore> table_store_wrapper;
  const int64_t max_wait_timeout_us = 30L * 1000L * 1000L; // 30s
  ObTimeGuard tg("ddl_retry_tablet", max_wait_timeout_us);
  if (OB_UNLIKELY(!ls_id.is_valid() || !tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(ls_id), K(tablet_id));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(ls_id));
  }
  while (OB_SUCC(ret)) {
    if (OB_FAIL(THIS_WORKER.check_status())) {
      LOG_WARN("check status failed", K(ret), K(ls_id), K(tablet_id));
    } else if (tg.get_diff() > max_wait_timeout_us) {
      ret = OB_NEED_RETRY;
      LOG_WARN("process finished tablet timeout, need retry", K(ret), K(ls_id), K(tablet_id), K(tg));
    } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
        tablet_id, tablet_handle, ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
      LOG_WARN("get tablet handle failed", K(ret), K(ls_id), K(tablet_id));
    } else if (OB_UNLIKELY(nullptr == tablet_handle.get_obj())) {
      ret = OB_ERR_SYS;
      LOG_WARN("tablet handle is null", K(ret), K(ls_id), K(tablet_id));
    } else if (task_id <= 0 || common::OB_INVALID_ID == table_id || execution_id < 0
      || tablet_handle.get_obj()->get_tablet_meta().ddl_execution_id_ > execution_id) {
      // no need to report checkksum.
      LOG_INFO("no need to report checksum", K(ret), K(task_id), K(table_id), K(execution_id),
        "tablet_meta", tablet_handle.get_obj()->get_tablet_meta());
      break;
    } else if (OB_FAIL(tablet_handle.get_obj()->fetch_table_store(table_store_wrapper))) {
      LOG_WARN("fail to fetch table store", K(ret));
    } else if (FALSE_IT(first_major_sstable = static_cast<ObSSTable *>(
          table_store_wrapper.get_member()->get_major_sstables().get_boundary_table(false/*first*/)))) {
    } else if (nullptr == first_major_sstable) {
      LOG_INFO("major not exist, retry later", K(ret), K(ls_id), K(tablet_id), K(tg));
      usleep(100L * 1000L); // 100ms
    } else if (OB_FAIL(ObTabletDDLUtil::check_and_get_major_sstable(
        ls_id, tablet_id, first_major_sstable, table_store_wrapper))) {
      LOG_WARN("check if major sstable exist failed", K(ret), K(ls_id), K(tablet_id));
    } else if (OB_FAIL(first_major_sstable->get_meta(sst_meta_hdl))) {
      LOG_WARN("fail to get sstable meta handle", K(ret));
    } else {
      const int64_t *column_checksums = sst_meta_hdl.get_sstable_meta().get_col_checksum();
      int64_t column_count = sst_meta_hdl.get_sstable_meta().get_col_checksum_cnt();
      ObArray<int64_t> co_column_checksums;
      co_column_checksums.set_attr(ObMemAttr(MTL_ID(), "TblDL_Ccc"));
      if (OB_FAIL(get_co_column_checksums_if_need(tablet_handle, first_major_sstable, co_column_checksums))) {
        LOG_WARN("get column checksum from co sstable failed", K(ret));
      } else if (OB_FAIL(ObTabletDDLUtil::report_ddl_checksum(
            ls_id,
            tablet_id,
            table_id,
            execution_id,
            task_id,
            co_column_checksums.empty() ? column_checksums : co_column_checksums.get_data(),
            co_column_checksums.empty() ? column_count : co_column_checksums.count()))) {
        LOG_WARN("report ddl column checksum failed", K(ret), K(ls_id), K(tablet_id), K(execution_id));
      } else {
        break;
      }
    }
  }
  if (OB_SUCC(ret) && nullptr != row_iter) {
    const ObDatumRow *row = nullptr;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(THIS_WORKER.check_status())) {
        LOG_WARN("check status failed", K(ret));
      } else if (OB_FAIL(row_iter->get_next_row(row))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        } else {
          // ignore error, iter part row completely.
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObTenantDirectLoadMgr::get_tablet_mgr_and_check_major(
    const share::ObLSID &ls_id,
    const ObTabletID &tablet_id,
    const bool is_full_direct_load,
    ObTabletDirectLoadMgrHandle &direct_load_mgr_handle,
    bool &is_major_sstable_exist)
{
  int ret = get_tablet_mgr(tablet_id, is_full_direct_load, direct_load_mgr_handle);
  is_major_sstable_exist = false;
  if (OB_ENTRY_NOT_EXIST == ret) {
    int tmp_ret = OB_SUCCESS;
    ObLSHandle ls_handle;
    ObTabletHandle tablet_handle;
    if (OB_TMP_FAIL(MTL(ObLSService *)->get_ls(ls_id, ls_handle, ObLSGetMod::DDL_MOD))) {
      LOG_WARN("failed to get log stream", K(tmp_ret), K(ls_id));
    } else if (OB_TMP_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, tablet_id, tablet_handle))) {
      LOG_WARN("get tablet handle failed", K(tmp_ret), K(ls_id), K(tablet_id));
    } else {
      is_major_sstable_exist = tablet_handle.get_obj()->get_major_table_count() > 0
        || tablet_handle.get_obj()->get_tablet_meta().table_store_flag_.with_major_sstable();
    }
    if (!is_major_sstable_exist) {
      ret = OB_TASK_EXPIRED;
    }
  }
  return ret;
}

int ObTenantDirectLoadMgr::get_tablet_mgr(
    const ObTabletID &tablet_id,
    const bool is_full_direct_load,
    ObTabletDirectLoadMgrHandle &direct_load_mgr_handle)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!tablet_id.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tablet_id));
  } else {
    ObTabletDirectLoadMgrKey mgr_key(tablet_id, is_full_direct_load);
    ObBucketHashRLockGuard guard(bucket_lock_, mgr_key.hash());
    if (OB_FAIL(get_tablet_mgr_no_lock(mgr_key, direct_load_mgr_handle))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("get table mgr without lock failed", K(ret), K(mgr_key));
      }
    }
  }
  return ret;
}

int ObTenantDirectLoadMgr::get_tablet_mgr_no_lock(
    const ObTabletDirectLoadMgrKey &mgr_key,
    ObTabletDirectLoadMgrHandle &direct_load_mgr_handle)
{
  int ret = OB_SUCCESS;
  ObTabletDirectLoadMgr *tablet_mgr = nullptr;
  if (OB_UNLIKELY(!mgr_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(mgr_key));
  } else if (OB_FAIL(tablet_mgr_map_.get_refactored(mgr_key, tablet_mgr))) {
    if (OB_HASH_NOT_EXIST != ret) {
      LOG_WARN("get refactored failed", K(ret), K(mgr_key));
    } else {
      ret = OB_HASH_NOT_EXIST == ret ? OB_ENTRY_NOT_EXIST : ret;
    }
  } else if (OB_FAIL(direct_load_mgr_handle.set_obj(tablet_mgr))) {
    LOG_WARN("set handle failed", K(ret), K(mgr_key));
  } else if (!direct_load_mgr_handle.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), K(mgr_key));
  }
  return ret;
}

int ObTenantDirectLoadMgr::remove_tablet_direct_load(const ObTabletDirectLoadMgrKey &mgr_key)
{
  ObBucketHashWLockGuard guard(bucket_lock_, mgr_key.hash());
  return remove_tablet_direct_load_nolock(mgr_key);
}

int ObTenantDirectLoadMgr::remove_tablet_direct_load_nolock(const ObTabletDirectLoadMgrKey &mgr_key)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!mgr_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(mgr_key));
  } else {
    ObTabletDirectLoadMgr *tablet_direct_load_mgr = nullptr;
    if (OB_FAIL(tablet_mgr_map_.get_refactored(mgr_key, tablet_direct_load_mgr))) {
      ret = OB_HASH_NOT_EXIST == ret ? OB_ENTRY_NOT_EXIST : ret;
      LOG_TRACE("get table mgr failed", K(ret), K(mgr_key), K(common::lbt()));
    } else if (OB_ISNULL(tablet_direct_load_mgr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(mgr_key));
    } else if (OB_FAIL(tablet_mgr_map_.erase_refactored(mgr_key))) {
      LOG_WARN("erase from map failed", K(ret));
    } else {
      LOG_INFO("remove tablet direct load mgr from MTL", K(ret), K(mgr_key), K(common::lbt()), K(tablet_direct_load_mgr->get_ref()));
      if (0 == tablet_direct_load_mgr->dec_ref()) {
        tablet_direct_load_mgr->~ObTabletDirectLoadMgr();
        allocator_.free(tablet_direct_load_mgr);
      }
    }
  }
  return ret;
}

struct DestroySliceWriterMapFn
{
public:
  DestroySliceWriterMapFn(ObIAllocator *allocator) :allocator_(allocator) {}
  int operator () (hash::HashMapPair<int64_t, ObDirectLoadSliceWriter *> &entry) {
    int ret = OB_SUCCESS;
    if (nullptr != allocator_) {
      if (nullptr != entry.second) {
        LOG_INFO("erase a slice writer", K(&entry.second), "slice_id", entry.first);
        entry.second->~ObDirectLoadSliceWriter();
        allocator_->free(entry.second);
        entry.second = nullptr;
      }
    }
    return ret;
  }

private:
  ObIAllocator *allocator_;
};

ObTabletDirectLoadBuildCtx::ObTabletDirectLoadBuildCtx()
  : allocator_(), slice_writer_allocator_(), build_param_(), slice_mgr_map_(), data_block_desc_(true/*is ddl*/), index_builder_(nullptr),
    column_stat_array_(), sorted_slice_writers_(), is_task_end_(false), task_finish_count_(0), fill_column_group_finish_count_(0)
{
  column_stat_array_.set_attr(ObMemAttr(MTL_ID(), "TblDL_CSA"));
  sorted_slice_writers_.set_attr(ObMemAttr(MTL_ID(), "TblDL_SSR"));
}

ObTabletDirectLoadBuildCtx::~ObTabletDirectLoadBuildCtx()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(index_builder_)) {
    index_builder_->~ObSSTableIndexBuilder();
    allocator_.free(index_builder_);
    index_builder_ = nullptr;
  }
  for (int64_t i = 0; i < column_stat_array_.count(); i++) {
    ObOptColumnStat *col_stat = column_stat_array_.at(i);
    col_stat->~ObOptColumnStat();
    allocator_.free(col_stat);
    col_stat = nullptr;
  }
  column_stat_array_.reset();
  sorted_slice_writers_.reset();

  if (!slice_mgr_map_.empty()) {
    DestroySliceWriterMapFn destroy_map_fn(&slice_writer_allocator_);
    slice_mgr_map_.foreach_refactored(destroy_map_fn);
    slice_mgr_map_.destroy();
  }
  allocator_.reset();
  slice_writer_allocator_.reset();
}

bool ObTabletDirectLoadBuildCtx::is_valid() const
{
  return build_param_.is_valid();
}

void ObTabletDirectLoadBuildCtx::reset_slice_ctx_on_demand()
{
  ATOMIC_STORE(&task_finish_count_, 0);
  ATOMIC_STORE(&fill_column_group_finish_count_, 0);
  ATOMIC_STORE(&task_total_cnt_, build_param_.runtime_only_param_.task_cnt_);
}

ObTabletDirectLoadMgr::ObTabletDirectLoadMgr()
  : is_inited_(false), is_schema_item_ready_(false), ls_id_(), tablet_id_(), table_key_(), data_format_version_(0),
    lock_(), ref_cnt_(0), direct_load_type_(ObDirectLoadType::DIRECT_LOAD_INVALID), sqc_build_ctx_(),
    column_items_(), lob_column_idxs_(), lob_col_types_(), tablet_handle_(), schema_item_()
{
  column_items_.set_attr(ObMemAttr(MTL_ID(), "DL_schema"));
  lob_column_idxs_.set_attr(ObMemAttr(MTL_ID(), "DL_schema"));
  lob_col_types_.set_attr(ObMemAttr(MTL_ID(), "DL_schema"));
}

ObTabletDirectLoadMgr::~ObTabletDirectLoadMgr()
{
  FLOG_INFO("deconstruct tablet direct load mgr", KP(this), KPC(this));
  ObLatchWGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
  is_inited_ = false;
  ls_id_.reset();
  tablet_id_.reset();
  table_key_.reset();
  data_format_version_ = 0;
  ATOMIC_STORE(&ref_cnt_, 0);
  direct_load_type_ = ObDirectLoadType::DIRECT_LOAD_INVALID;
  column_items_.reset();
  lob_column_idxs_.reset();
  lob_col_types_.reset();
  tablet_handle_.reset();
  schema_item_.reset();
  is_schema_item_ready_ = false;
}

bool ObTabletDirectLoadMgr::is_valid()
{
  return is_inited_ == true && ls_id_.is_valid() && tablet_id_.is_valid()
      && is_valid_direct_load(direct_load_type_);
}

int ObTabletDirectLoadMgr::update(
    ObTabletDirectLoadMgr *lob_tablet_mgr,
    const ObTabletDirectLoadInsertParam &build_param)
{
  UNUSED(lob_tablet_mgr);
  int ret = OB_SUCCESS;
  const int64_t bucket_num = 97L; // 97
  const int64_t memory_limit = 1024L * 1024L * 1024L * 10L; // 10GB
  if (OB_UNLIKELY(!build_param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(build_param));
  } else if (!build_param.is_replay_ && !sqc_build_ctx_.slice_mgr_map_.created()) {
    // 1. Create slice_mgr_map if the tablet_direct_load_mgr is created firstly.
    // 2. Create slice_mgr_map if the node is switched from follower to leader.
    const uint64_t tenant_id = MTL_ID();
    lib::ObMemAttr attr(tenant_id, "TabletDLMgr");
    lib::ObMemAttr slice_writer_attr(tenant_id, "SliceWriter");
    lib::ObMemAttr slice_writer_map_attr(tenant_id, "SliceWriterMap");
    if (OB_FAIL(sqc_build_ctx_.allocator_.init(OB_MALLOC_MIDDLE_BLOCK_SIZE,
      attr.label_, tenant_id, memory_limit))) {
      LOG_WARN("init alloctor failed", K(ret));
    } else if (OB_FAIL(sqc_build_ctx_.slice_writer_allocator_.init(OB_MALLOC_MIDDLE_BLOCK_SIZE,
      slice_writer_attr.label_, tenant_id, memory_limit))) {
      LOG_WARN("init allocator failed", K(ret));
    } else if (OB_FAIL(sqc_build_ctx_.slice_mgr_map_.create(bucket_num,
                                                      slice_writer_map_attr, slice_writer_map_attr))) {
      LOG_WARN("create slice writer map failed", K(ret));
    } else if (OB_FAIL(cond_.init(ObWaitEventIds::COLUMN_STORE_DDL_RESCAN_LOCK_WAIT))) {
      LOG_WARN("init condition failed", K(ret));
    } else {
      sqc_build_ctx_.allocator_.set_attr(attr);
      sqc_build_ctx_.slice_writer_allocator_.set_attr(slice_writer_attr);
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(sqc_build_ctx_.build_param_.assign(build_param))) {
      LOG_WARN("assign build param failed", K(ret));
    } else {
      ls_id_ = build_param.common_param_.ls_id_;
      tablet_id_ = build_param.common_param_.tablet_id_;
      direct_load_type_ = build_param.common_param_.direct_load_type_;
      is_inited_ = true;
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::open_sstable_slice(
    const bool is_data_tablet_process_for_lob,
    const blocksstable::ObMacroDataSeq &start_seq,
    const int64_t slice_id)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), KPC(this));
  } else if (OB_UNLIKELY(!start_seq.is_valid() || slice_id <= 0 || !sqc_build_ctx_.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(tablet_id_), K(start_seq), K(slice_id), K(sqc_build_ctx_));
  } else if (is_data_tablet_process_for_lob) {
    if (OB_UNLIKELY(!lob_mgr_handle_.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), KPC(this));
    } else if (OB_FAIL(lob_mgr_handle_.get_obj()->open_sstable_slice(
        false, start_seq, slice_id))) {
      LOG_WARN("open sstable slice for lob failed", K(ret), KPC(this));
    }
  } else if (OB_UNLIKELY(!is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), KPC(this));
  } else if (OB_FAIL(prepare_schema_item_on_demand(sqc_build_ctx_.build_param_.runtime_only_param_.table_id_))) {
    LOG_WARN("prepare table schema item on demand", K(ret), K(sqc_build_ctx_.build_param_));
  } else {
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    if (OB_ISNULL(slice_writer = OB_NEWx(ObDirectLoadSliceWriter, (&sqc_build_ctx_.slice_writer_allocator_)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to new ObDirectLoadSliceWriter", KR(ret));
    } else if (OB_FAIL(slice_writer->init(this, start_seq))) {
      LOG_WARN("init sstable slice writer failed", K(ret), KPC(this));
    } else if (OB_FAIL(sqc_build_ctx_.slice_mgr_map_.set_refactored(slice_id, slice_writer))) {
      LOG_WARN("set refactored failed", K(ret), K(slice_id), KPC(this));
    } else {
      LOG_INFO("add a slice writer", KP(slice_writer), K(slice_id), K(sqc_build_ctx_.slice_mgr_map_.size()));
    }
    if (OB_FAIL(ret)) {
      if (OB_NOT_NULL(slice_writer)) {
        slice_writer->~ObDirectLoadSliceWriter();
        sqc_build_ctx_.slice_writer_allocator_.free(slice_writer);
        slice_writer = nullptr;
      }
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::prepare_schema_item_on_demand(const uint64_t table_id)
{
  int ret = OB_SUCCESS;
  bool is_schema_item_ready = ATOMIC_LOAD(&is_schema_item_ready_);
  if (!is_schema_item_ready) {
    ObLatchRGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
    is_schema_item_ready = is_schema_item_ready_;
  }
  if (!is_schema_item_ready) {
    ObLatchWGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
    if (is_schema_item_ready_) {
      // do nothing
    } else if (OB_UNLIKELY(OB_INVALID_ID == table_id)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid arguments", K(ret), K(table_id));
    } else {
      const uint64_t tenant_id = MTL_ID();
      ObSchemaGetterGuard schema_guard;
      const ObDataStoreDesc &data_desc = sqc_build_ctx_.data_block_desc_.get_desc();
      const ObTableSchema *table_schema = nullptr;
      if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(tenant_id, schema_guard))) {
        LOG_WARN("get tenant schema failed", K(ret), K(tenant_id), K(table_id));
      } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id, table_id, table_schema))) {
        LOG_WARN("get table schema failed", K(ret), K(tenant_id), K(table_id));
      } else if (OB_ISNULL(table_schema)) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("table not exist", K(ret), K(tenant_id), K(table_id));
      } else if (OB_FAIL(prepare_index_builder_if_need(*table_schema))) {
        LOG_WARN("prepare sstable index builder failed", K(ret), K(sqc_build_ctx_));
      } else if (OB_FAIL(table_schema->get_is_column_store(schema_item_.is_column_store_))) {
        LOG_WARN("fail to get is column store", K(ret));
      } else {
        schema_item_.is_index_table_ = table_schema->is_index_table();
        schema_item_.rowkey_column_num_ = table_schema->get_rowkey_column_num();
        schema_item_.is_unique_index_ = table_schema->is_unique_index();

        if (OB_FAIL(column_items_.reserve(data_desc.get_col_desc_array().count()))) {
          LOG_WARN("reserve column schema array failed", K(ret), K(data_desc.get_col_desc_array().count()), K(column_items_));
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < data_desc.get_col_desc_array().count(); ++i) {
            const ObColDesc &col_desc = data_desc.get_col_desc_array().at(i);
            const schema::ObColumnSchemaV2 *column_schema = nullptr;
            ObColumnSchemaItem column_item;
            if (i >= table_schema->get_rowkey_column_num() && i < table_schema->get_rowkey_column_num() + ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt()) {
              // skip multi version column, keep item invalid
            } else if (OB_ISNULL(column_schema = table_schema->get_column_schema(col_desc.col_id_))) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("column schema is null", K(ret), K(i), K(data_desc.get_col_desc_array()), K(col_desc.col_id_));
            } else {
              column_item.is_valid_ = true;
              column_item.col_type_ = column_schema->get_meta_type();
              column_item.col_accuracy_ = column_schema->get_accuracy();
            }
            if (OB_SUCC(ret)) {
              if (OB_FAIL(column_items_.push_back(column_item))) {
                LOG_WARN("push back null column schema failed", K(ret));
              } else if (OB_NOT_NULL(column_schema) && column_schema->get_meta_type().is_lob_storage()) { // not multi version column
                if (OB_FAIL(lob_column_idxs_.push_back(i))) {
                  LOG_WARN("push back lob column idx failed", K(ret), K(i));
                } else if (OB_FAIL(lob_col_types_.push_back(column_schema->get_meta_type()))) {
                  LOG_WARN("push back lob col_type  failed", K(ret), K(i));
                }
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          // get compress type
          uint64_t tenant_id = table_schema->get_tenant_id();
          omt::ObTenantConfigGuard tenant_config(TENANT_CONF(tenant_id));
          if (OB_UNLIKELY(!tenant_config.is_valid())) {
            //tenant config获取失败时，租户不存在;返回默认值
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("fail get tenant_config", K(ret), K(tenant_id));
          } else if (tenant_config->enable_store_compression) {
            schema_item_.compress_type_ = table_schema->get_compressor_type();
          }
          LOG_INFO("load compress type is:", K(schema_item_.compress_type_), K(tenant_config->enable_store_compression));
        }
      }
      if (OB_SUCC(ret)) {
        is_schema_item_ready_ = true;
      }
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::fill_sstable_slice(
    const ObDirectLoadSliceInfo &slice_info,
    const SCN &start_scn,
    ObIStoreRowIterator *iter,
    int64_t &affected_rows,
    ObInsertMonitor *insert_monitor)
{
  int ret = OB_SUCCESS;
  affected_rows = 0;
  share::SCN commit_scn;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid() || !start_scn.is_valid_and_not_min()) || !sqc_build_ctx_.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(slice_info), K(start_scn), K(sqc_build_ctx_));
  } else if (OB_UNLIKELY(!is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), KPC(this));
  } else if (is_full_direct_load(direct_load_type_)) {
    if (OB_UNLIKELY(!tablet_handle_.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid tablet handle", K(ret), K(tablet_handle_));
    } else {
      commit_scn = get_commit_scn(tablet_handle_.get_obj()->get_tablet_meta());
      if (commit_scn.is_valid_and_not_min()) {
        ret = OB_TRANS_COMMITED;
        FLOG_INFO("already committed", K(commit_scn), KPC(this));
      } else if (start_scn != get_start_scn()) {
        ret = OB_TASK_EXPIRED;
        LOG_WARN("task expired", K(ret), "start_scn of current execution", start_scn, "start_scn latest", get_start_scn());
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    if (OB_FAIL(sqc_build_ctx_.slice_mgr_map_.get_refactored(slice_info.slice_id_, slice_writer))) {
      LOG_WARN("get refactored failed", K(ret), K(slice_info));
    } else if (OB_ISNULL(slice_writer) || OB_UNLIKELY(!ATOMIC_LOAD(&is_schema_item_ready_))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(slice_info), K(is_schema_item_ready_));
    } else if (OB_FAIL(slice_writer->fill_sstable_slice(start_scn, sqc_build_ctx_.build_param_.runtime_only_param_.table_id_, tablet_id_,
        iter, schema_item_, direct_load_type_, column_items_, affected_rows, insert_monitor))) {
      LOG_WARN("fill sstable slice failed", K(ret), KPC(this));
    }
  }
  if (OB_FAIL(ret) && (OB_TRANS_COMMITED != ret)) {
    // cleanup when failed.
    int tmp_ret = OB_SUCCESS;
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    if (OB_TMP_FAIL(sqc_build_ctx_.slice_mgr_map_.erase_refactored(slice_info.slice_id_, &slice_writer))) {
      LOG_ERROR("erase failed", K(ret), K(tmp_ret), K(slice_info));
    } else {
      LOG_INFO("erase a slice writer", KP(slice_writer), "slice_id", slice_info.slice_id_, K(sqc_build_ctx_.slice_mgr_map_.size()));
      slice_writer->~ObDirectLoadSliceWriter();
      sqc_build_ctx_.slice_writer_allocator_.free(slice_writer);
      slice_writer = nullptr;
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::fill_lob_sstable_slice(
    ObIAllocator &allocator,
    const ObDirectLoadSliceInfo &slice_info,
    const SCN &start_scn,
    share::ObTabletCacheInterval &pk_interval,
    blocksstable::ObDatumRow &datum_row)
{
  int ret = OB_SUCCESS;
  share::SCN commit_scn;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid() || !sqc_build_ctx_.is_valid() || !start_scn.is_valid_and_not_min() ||
      !lob_mgr_handle_.is_valid() || !lob_mgr_handle_.get_obj()->get_sqc_build_ctx().is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(slice_info), "lob_direct_load_mgr is valid", lob_mgr_handle_.is_valid(), KPC(this), K(start_scn));
  } else if (is_full_direct_load(direct_load_type_)) {
    commit_scn = get_commit_scn(tablet_handle_.get_obj()->get_tablet_meta());
    if (commit_scn.is_valid_and_not_min()) {
      ret = OB_TRANS_COMMITED;
      FLOG_INFO("already committed", K(commit_scn), KPC(this));
    } else if (start_scn != get_start_scn()) {
      ret = OB_TASK_EXPIRED;
      LOG_WARN("task expired", K(ret), "start_scn of current execution", start_scn, "start_scn latest", get_start_scn());
    }
  }

  if (OB_SUCC(ret)) {
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    const int64_t trans_version = is_full_direct_load(direct_load_type_) ? table_key_.get_snapshot_version() : INT64_MAX;
    ObBatchSliceWriteInfo info(tablet_id_, ls_id_, trans_version, direct_load_type_, sqc_build_ctx_.build_param_.runtime_only_param_.trans_id_,
        sqc_build_ctx_.build_param_.runtime_only_param_.seq_no_);

    if (OB_FAIL(lob_mgr_handle_.get_obj()->get_sqc_build_ctx().slice_mgr_map_.get_refactored(slice_info.slice_id_, slice_writer))) {
      LOG_WARN("get refactored failed", K(ret), K(slice_info), K(sqc_build_ctx_.slice_mgr_map_.size()));
    } else if (OB_ISNULL(slice_writer) || OB_UNLIKELY(!ATOMIC_LOAD(&(lob_mgr_handle_.get_obj()->is_schema_item_ready_)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(slice_info), K(lob_mgr_handle_.get_obj()->is_schema_item_ready_));
    } else if (OB_FAIL(slice_writer->fill_lob_sstable_slice(lob_mgr_handle_.get_obj()->sqc_build_ctx_.build_param_.runtime_only_param_.table_id_, allocator, sqc_build_ctx_.allocator_,
          start_scn, info, pk_interval, lob_column_idxs_, lob_col_types_, schema_item_.lob_inrow_threshold_, datum_row))) {
        LOG_WARN("fail to fill batch sstable slice", K(ret), K(start_scn), K(tablet_id_), K(pk_interval));
    }
  }
  if (OB_FAIL(ret) && lob_mgr_handle_.is_valid()) {
    // cleanup when failed.
    int tmp_ret = OB_SUCCESS;
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    if (OB_TMP_FAIL(lob_mgr_handle_.get_obj()->get_sqc_build_ctx().slice_mgr_map_.erase_refactored(slice_info.slice_id_, &slice_writer))) {
      LOG_ERROR("erase failed", K(ret), K(tmp_ret), K(slice_info));
    } else {
      LOG_INFO("erase a slice writer", KP(slice_writer), "slice_id", slice_info.slice_id_, K(sqc_build_ctx_.slice_mgr_map_.size()));
      slice_writer->~ObDirectLoadSliceWriter();
      lob_mgr_handle_.get_obj()->get_sqc_build_ctx().slice_writer_allocator_.free(slice_writer);
      slice_writer = nullptr;
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::fill_lob_sstable_slice(
    ObIAllocator &allocator,
    const ObDirectLoadSliceInfo &slice_info,
    const SCN &start_scn,
    share::ObTabletCacheInterval &pk_interval,
    const ObArray<int64_t> &lob_column_idxs,
    const ObArray<common::ObObjMeta> &col_types,
    blocksstable::ObDatumRow &datum_row)
{
  int ret = OB_SUCCESS;
  share::SCN commit_scn;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid() || !sqc_build_ctx_.is_valid() || !start_scn.is_valid_and_not_min() ||
      !lob_mgr_handle_.is_valid() || !lob_mgr_handle_.get_obj()->get_sqc_build_ctx().is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(slice_info), "lob_direct_load_mgr is valid", lob_mgr_handle_.is_valid(), KPC(this), K(start_scn));
  } else if (is_full_direct_load(direct_load_type_)) {
    commit_scn = get_commit_scn(tablet_handle_.get_obj()->get_tablet_meta());
    if (commit_scn.is_valid_and_not_min()) {
      ret = OB_TRANS_COMMITED;
      FLOG_INFO("already committed", K(commit_scn), KPC(this));
    } else if (start_scn != get_start_scn()) {
      ret = OB_TASK_EXPIRED;
      LOG_WARN("task expired", K(ret), "start_scn of current execution", start_scn, "start_scn latest", get_start_scn());
    }
  }

  if (OB_SUCC(ret)) {
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    const int64_t trans_version = is_full_direct_load(direct_load_type_) ? table_key_.get_snapshot_version() : INT64_MAX;
    ObBatchSliceWriteInfo info(tablet_id_, ls_id_, trans_version, direct_load_type_, sqc_build_ctx_.build_param_.runtime_only_param_.trans_id_,
        sqc_build_ctx_.build_param_.runtime_only_param_.seq_no_);

    if (OB_FAIL(lob_mgr_handle_.get_obj()->get_sqc_build_ctx().slice_mgr_map_.get_refactored(slice_info.slice_id_, slice_writer))) {
      LOG_WARN("get refactored failed", K(ret), K(slice_info), K(sqc_build_ctx_.slice_mgr_map_.size()));
    } else if (OB_ISNULL(slice_writer) || OB_UNLIKELY(!ATOMIC_LOAD(&(lob_mgr_handle_.get_obj()->is_schema_item_ready_)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(slice_info), K(lob_mgr_handle_.get_obj()->is_schema_item_ready_));
    } else if (OB_FAIL(slice_writer->fill_lob_sstable_slice(lob_mgr_handle_.get_obj()->sqc_build_ctx_.build_param_.runtime_only_param_.table_id_,
        allocator, sqc_build_ctx_.allocator_, start_scn, info,
        pk_interval, lob_column_idxs, col_types, schema_item_.lob_inrow_threshold_, datum_row))) {
      LOG_WARN("fail to fill batch sstable slice", K(ret), K(start_scn), K(tablet_id_), K(pk_interval));
    }
  }
  if (OB_FAIL(ret) && lob_mgr_handle_.is_valid()) {
    // cleanup when failed.
    int tmp_ret = OB_SUCCESS;
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    if (OB_TMP_FAIL(lob_mgr_handle_.get_obj()->get_sqc_build_ctx().slice_mgr_map_.erase_refactored(slice_info.slice_id_, &slice_writer))) {
      LOG_ERROR("erase failed", K(ret), K(tmp_ret), K(slice_info));
    } else {
      LOG_INFO("erase a slice writer", KP(slice_writer), "slice_id", slice_info.slice_id_, K(sqc_build_ctx_.slice_mgr_map_.size()));
      slice_writer->~ObDirectLoadSliceWriter();
      lob_mgr_handle_.get_obj()->get_sqc_build_ctx().slice_writer_allocator_.free(slice_writer);
      slice_writer = nullptr;
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::wait_notify(const ObDirectLoadSliceWriter *slice_writer, const share::SCN &start_scn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_ISNULL(slice_writer) || !start_scn.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(slice_writer), K(start_scn));
  } else {
    while (OB_SUCC(ret)) {
      const SCN tablet_start_scn = get_start_scn();
      if (OB_FAIL(THIS_WORKER.check_status())) {
        LOG_WARN("check status failed", K(ret));
      } else if (start_scn != tablet_start_scn) {
        ret = OB_TASK_EXPIRED;
        LOG_WARN("task expired", K(ret), K(start_scn), K(tablet_start_scn));
      } else if (slice_writer->get_row_offset() >= 0) {
        // row offset already set
        break;
      } else {
        const int64_t wait_interval_ms = 100L;
        ObThreadCondGuard guard(cond_);
        if (OB_FAIL(cond_.wait(wait_interval_ms))) {
          if (OB_TIMEOUT != ret) {
            LOG_WARN("wait thread condition failed", K(ret));
          } else {
            ret = OB_SUCCESS;
          }
        }
      }
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::notify_all()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else {
    ObThreadCondGuard guard(cond_);
    if (OB_FAIL(cond_.broadcast())) {
      LOG_WARN("broadcast thread condition failed", K(ret));
    }
  }
  return ret;
}

struct SliceEndkeyCompareFunctor
{
public:
  SliceEndkeyCompareFunctor(const ObStorageDatumUtils &datum_utils) : datum_utils_(datum_utils), ret_code_(OB_SUCCESS) {}
  bool operator ()(const ObDirectLoadSliceWriter *left, const ObDirectLoadSliceWriter *right)
  {
    bool bret = false;
    int ret = ret_code_;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(left) || OB_ISNULL(right)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret));
    } else if (!left->is_empty() && !right->is_empty()) {
      const ObChunkSliceStore *left_slice_store = static_cast<const ObChunkSliceStore *>(left->get_slice_store());
      const ObChunkSliceStore *right_slice_store = static_cast<const ObChunkSliceStore *>(right->get_slice_store());
      int cmp_ret = 0;
      if (OB_FAIL(left_slice_store->endkey_.compare(right_slice_store->endkey_, datum_utils_, cmp_ret))) {
        LOG_WARN("endkey compare failed", K(ret));
      } else {
        bret = cmp_ret < 0;
      }
    } else if (left->is_empty() && right->is_empty()) {
      // both empty, compare pointer
      bret = left < right;
    } else {
      // valid formmer, empty latter
      bret = !left->is_empty();
    }
    ret_code_ = OB_SUCCESS == ret_code_ ? ret : ret_code_;
    return bret;
  }
public:
  const ObStorageDatumUtils &datum_utils_;
  int ret_code_;
};

int ObTabletDirectLoadMgr::calc_range(const ObStorageSchema *storage_schema, const ObStorageDatumUtils &datum_utils)
{
  int ret = OB_SUCCESS;
  ObArray<ObDirectLoadSliceWriter *> sorted_slices;
  sorted_slices.set_attr(ObMemAttr(MTL_ID(), "DL_SortS_tmp"));
  if (OB_UNLIKELY(nullptr == storage_schema || !datum_utils.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), KP(storage_schema), K(datum_utils));
  } else if (OB_FAIL(sorted_slices.reserve(sqc_build_ctx_.slice_mgr_map_.size()))) {
    LOG_WARN("reserve slice array failed", K(ret), K(sqc_build_ctx_.slice_mgr_map_.size()));
  } else {
    for (ObTabletDirectLoadBuildCtx::SLICE_MGR_MAP::const_iterator iter = sqc_build_ctx_.slice_mgr_map_.begin();
      OB_SUCC(ret) && iter != sqc_build_ctx_.slice_mgr_map_.end(); ++iter) {
      ObDirectLoadSliceWriter *cur_slice = iter->second;
      if (OB_ISNULL(cur_slice)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("invalid argument", K(ret), KP(cur_slice));
      } else if (OB_FAIL(sorted_slices.push_back(cur_slice))) {
        LOG_WARN("push back slice failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      SliceEndkeyCompareFunctor cmp(datum_utils);
      std::sort(sorted_slices.begin(), sorted_slices.end(), cmp);
      ret = cmp.ret_code_;
      if (OB_FAIL(ret)) {
        LOG_WARN("sort slice failed", K(ret), K(sorted_slices));
      }
    }
    int64_t offset = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < sorted_slices.count(); ++i) {
      sorted_slices.at(i)->set_row_offset(offset);
      offset += sorted_slices.at(i)->get_row_count();
    }
  }
  if (OB_SUCC(ret) && is_data_direct_load(direct_load_type_)) {
    bool is_column_store = false;
    if (OB_FAIL(ObCODDLUtil::need_column_group_store(*storage_schema, is_column_store))) {
      LOG_WARN("fail to check need column group", K(ret));
    } else if (is_column_store) {
      if (OB_FAIL(sqc_build_ctx_.sorted_slice_writers_.assign(sorted_slices))) {
        LOG_WARN("copy slice array failed", K(ret), K(sorted_slices.count()));
      }
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::cancel()
{
  int ret = OB_SUCCESS;
  for (ObTabletDirectLoadBuildCtx::SLICE_MGR_MAP::const_iterator iter = sqc_build_ctx_.slice_mgr_map_.begin();
      OB_SUCC(ret) && iter != sqc_build_ctx_.slice_mgr_map_.end(); ++iter) {
    ObDirectLoadSliceWriter *cur_slice = iter->second;
    if (OB_ISNULL(cur_slice)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret), KP(cur_slice));
    } else {
      cur_slice->cancel();
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::close_sstable_slice(
    const bool is_data_tablet_process_for_lob,
    const ObDirectLoadSliceInfo &slice_info,
    const share::SCN &start_scn,
    const int64_t execution_id,
    ObInsertMonitor *insert_monitor)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!slice_info.is_valid() || !start_scn.is_valid_and_not_min() || !sqc_build_ctx_.is_valid() || execution_id < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(slice_info), K(start_scn), K(execution_id), K(sqc_build_ctx_));
  } else if (OB_UNLIKELY(!is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), KPC(this));
  } else if (is_data_tablet_process_for_lob) {
    if (OB_UNLIKELY(!lob_mgr_handle_.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(slice_info));
    } else if (OB_FAIL(lob_mgr_handle_.get_obj()->close_sstable_slice(
        false, slice_info, start_scn, execution_id))) {
      LOG_WARN("close lob sstable slice failed", K(ret), K(slice_info));
    }
  } else {
    ObDirectLoadSliceWriter *slice_writer = nullptr;
    if (OB_FAIL(sqc_build_ctx_.slice_mgr_map_.get_refactored(slice_info.slice_id_, slice_writer))) {
      ret = OB_HASH_NOT_EXIST == ret ? OB_ENTRY_NOT_EXIST : ret;
      LOG_WARN("get refactored failed", K(ret), K(slice_info));
    } else if (OB_ISNULL(slice_writer)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(slice_info));
    } else if (OB_FAIL(slice_writer->close())) {
      LOG_WARN("close failed", K(ret), K(slice_info));
    } else if (!slice_info.is_lob_slice_ && is_ddl_direct_load(direct_load_type_)) {
      int64_t task_finish_count = -1;
      {
        ObLatchRGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
        if (start_scn == get_start_scn()) {
          task_finish_count = ATOMIC_AAF(&sqc_build_ctx_.task_finish_count_, 1);
        }
      }
      LOG_INFO("inc task finish count", K(tablet_id_), K(execution_id), K(task_finish_count), K(sqc_build_ctx_.task_total_cnt_));
      ObTablet *tablet = nullptr;
      ObStorageSchema *storage_schema = nullptr;
      ObArenaAllocator arena_allocator("DDL_RESCAN", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
      bool is_column_group_store = false;
      if (OB_UNLIKELY(!tablet_handle_.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid tablet handle", K(ret), K(tablet_handle_));
      } else if (OB_ISNULL(tablet = tablet_handle_.get_obj())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tablet is null", K(ret), K(ls_id_), K(tablet_id_));
      } else if (OB_FAIL(tablet->load_storage_schema(arena_allocator, storage_schema))) {
        LOG_WARN("load storage schema failed", K(ret), K(tablet_id_));
      } else if (OB_FAIL(ObCODDLUtil::need_column_group_store(*storage_schema, is_column_group_store))) {
        LOG_WARN("fail to check is column group store", K(ret));
      } else if (!is_column_group_store) {
        if (task_finish_count >= sqc_build_ctx_.task_total_cnt_) {
          // for ddl, write commit log when all slices ready.
          if (OB_FAIL(close(execution_id, start_scn))) {
            LOG_WARN("close sstable slice failed", K(ret), K(sqc_build_ctx_.build_param_));
          }
        }
      } else {
        if (task_finish_count < sqc_build_ctx_.task_total_cnt_) {
          if (OB_FAIL(wait_notify(slice_writer, start_scn))) {
            LOG_WARN("wait notify failed", K(ret));
          } else if (OB_FAIL(slice_writer->fill_column_group(storage_schema, start_scn, insert_monitor))) {
            LOG_WARN("slice writer fill column group failed", K(ret));
          }
        } else {
          if (OB_FAIL(calc_range(storage_schema, tablet->get_rowkey_read_info().get_datum_utils()))) {
            LOG_WARN("calc range failed", K(ret));
          } else if (OB_FAIL(notify_all())) {
            LOG_WARN("notify all failed", K(ret));
          } else if (OB_FAIL(slice_writer->fill_column_group(storage_schema, start_scn, insert_monitor))) {
            LOG_WARN("slice fill column group failed", K(ret));
          }
        }
        if (OB_SUCC(ret)) {
          int64_t fill_cg_finish_count = -1;
          {
            ObLatchRGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
            if (start_scn == get_start_scn()) {
              fill_cg_finish_count = ATOMIC_AAF(&sqc_build_ctx_.fill_column_group_finish_count_, 1);
            }
          }
          LOG_INFO("inc fill cg finish count", K(tablet_id_), K(execution_id), K(fill_cg_finish_count), K(sqc_build_ctx_.task_total_cnt_));
          if (fill_cg_finish_count >= sqc_build_ctx_.task_total_cnt_) {
            // for ddl, write commit log when all slices ready.
            if (OB_FAIL(close(execution_id, start_scn))) {
              LOG_WARN("close sstable slice failed", K(ret));
            }
          }
        }
      }
      ObTabletObjLoadHelper::free(arena_allocator, storage_schema);
    }
    if (OB_NOT_NULL(slice_writer)) {
      if (is_data_direct_load(direct_load_type_) && slice_writer->need_column_store()) {
        //ignore, free after rescan
      } else {
        int tmp_ret = OB_SUCCESS;
        if (OB_TMP_FAIL(sqc_build_ctx_.slice_mgr_map_.erase_refactored(slice_info.slice_id_))) {
          LOG_ERROR("erase failed", K(ret), K(tmp_ret), K(slice_info));
        } else {
          LOG_INFO("erase a slice writer", KP(slice_writer), K(sqc_build_ctx_.slice_mgr_map_.size()));
          slice_writer->~ObDirectLoadSliceWriter();
          sqc_build_ctx_.slice_writer_allocator_.free(slice_writer);
          slice_writer = nullptr;
        }
        ret = OB_SUCC(ret) ? tmp_ret : ret;
      }
    }
  }
  return ret;
}

void ObTabletDirectLoadMgr::calc_cg_idx(const int64_t thread_cnt, const int64_t thread_id, int64_t &strat_idx, int64_t &end_idx)
{
  int ret = OB_SUCCESS;
  const int64_t each_thread_task_cnt = sqc_build_ctx_.sorted_slice_writers_.count() / thread_cnt;
  const int64_t need_plus_thread_cnt = sqc_build_ctx_.sorted_slice_writers_.count() % thread_cnt; // handle +1 task
  const int64_t pre_handle_cnt = need_plus_thread_cnt * (each_thread_task_cnt + 1);
  if (need_plus_thread_cnt != 0) {
    if (thread_id < need_plus_thread_cnt) {
      strat_idx = (each_thread_task_cnt + 1) * thread_id;
      end_idx = strat_idx + (each_thread_task_cnt + 1);
    } else {
      strat_idx = pre_handle_cnt + (thread_id - need_plus_thread_cnt) * each_thread_task_cnt;
      end_idx = strat_idx + each_thread_task_cnt;
    }
  } else {
    strat_idx = each_thread_task_cnt * thread_id;
    end_idx = strat_idx + each_thread_task_cnt;
  }
}

int ObTabletDirectLoadMgr::fill_column_group(const int64_t thread_cnt, const int64_t thread_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(thread_cnt <= 0 || thread_id < 0 || thread_id > thread_cnt - 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguement", K(ret), K(thread_cnt), K(thread_id));
  } else if (sqc_build_ctx_.sorted_slice_writers_.count() == 0) {
    //ignore
  } else if (sqc_build_ctx_.sorted_slice_writers_.count() != sqc_build_ctx_.slice_mgr_map_.size()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("wrong slice writer num", K(ret), K(sqc_build_ctx_.sorted_slice_writers_.count()), K(sqc_build_ctx_.slice_mgr_map_.size()), K(common::lbt()));
  } else {
    int64_t strat_idx = 0;
    int64_t last_idx = 0;
    calc_cg_idx(thread_cnt, thread_id, strat_idx, last_idx);
    LOG_INFO("direct load start fill column group", K(tablet_id_), K(sqc_build_ctx_.sorted_slice_writers_.count()), K(thread_cnt), K(thread_id), K(strat_idx), K(last_idx));
    if (strat_idx < 0 || strat_idx >= sqc_build_ctx_.sorted_slice_writers_.count() || last_idx > sqc_build_ctx_.sorted_slice_writers_.count()) {
      //skip
    } else {
      ObArenaAllocator arena_allocator("DIRECT_RESCAN", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
      ObTablet *tablet = nullptr;
      ObStorageSchema *storage_schema = nullptr;
      int64_t fill_cg_finish_count = -1;
      if (OB_UNLIKELY(!tablet_handle_.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid tablet handle", K(ret), K(tablet_handle_));
      } else if (OB_ISNULL(tablet = tablet_handle_.get_obj())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tablet is null", K(ret), K(ls_id_), K(tablet_id_));
      } else if (OB_FAIL(tablet->load_storage_schema(arena_allocator, storage_schema))) {
        LOG_WARN("load storage schema failed", K(ret), K(tablet_id_));
      } else {
        for (int64_t i = strat_idx; OB_SUCC(ret) && i < last_idx; ++i) {
          ObDirectLoadSliceWriter *slice_writer = sqc_build_ctx_.sorted_slice_writers_.at(i);
          if (OB_ISNULL(slice_writer) || !slice_writer->need_column_store()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("wrong slice writer", KPC(slice_writer));
          } else if (OB_FAIL(slice_writer->fill_column_group(storage_schema, get_start_scn()))) {
            LOG_WARN("slice writer rescan failed", K(ret), KP(storage_schema), K(get_start_scn()));
          } else {
            fill_cg_finish_count = ATOMIC_AAF(&sqc_build_ctx_.fill_column_group_finish_count_, 1);
          }
        }
      }
      ObTabletObjLoadHelper::free(arena_allocator, storage_schema); //arena cannot free
      arena_allocator.reset();
      if (OB_SUCC(ret)) {
        if (fill_cg_finish_count == sqc_build_ctx_.sorted_slice_writers_.count()) {
          sqc_build_ctx_.sorted_slice_writers_.reset();
          FLOG_INFO("tablet_direct_mgr finish fill column group", K(sqc_build_ctx_.slice_mgr_map_.size()), K(this), K(fill_cg_finish_count));
          if (!sqc_build_ctx_.slice_mgr_map_.empty()) {
            DestroySliceWriterMapFn destroy_map_fn(&sqc_build_ctx_.slice_writer_allocator_);
            int tmp_ret = sqc_build_ctx_.slice_mgr_map_.foreach_refactored(destroy_map_fn);
            if (tmp_ret == OB_SUCCESS) {
              sqc_build_ctx_.slice_mgr_map_.destroy();
            } else {
              ret = tmp_ret;
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      LOG_INFO("direct load finish fill column group", K(tablet_id_), K(sqc_build_ctx_.sorted_slice_writers_.count()), K(thread_cnt), K(thread_id), K(strat_idx), K(last_idx),
                                                       K(sqc_build_ctx_.slice_mgr_map_.size()));
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::prepare_index_builder_if_need(const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObWholeDataStoreDesc index_block_desc(true/*is ddl*/);
  if (sqc_build_ctx_.index_builder_ != nullptr) {
    LOG_INFO("index builder is already prepared");
  } else if (OB_FAIL(index_block_desc.init(table_schema, ls_id_, tablet_id_,
          is_full_direct_load(direct_load_type_) ? compaction::ObMergeType::MAJOR_MERGE : compaction::ObMergeType::MINOR_MERGE,
          is_full_direct_load(direct_load_type_) ? table_key_.get_snapshot_version() : 1L,
          data_format_version_))) {
    LOG_WARN("fail to init data desc", K(ret));
  } else {
    void *builder_buf = nullptr;

    if (OB_ISNULL(builder_buf = sqc_build_ctx_.allocator_.alloc(sizeof(ObSSTableIndexBuilder)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to alloc memory", K(ret));
    } else if (OB_ISNULL(sqc_build_ctx_.index_builder_ = new (builder_buf) ObSSTableIndexBuilder())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to new ObSSTableIndexBuilder", K(ret));
    } else if (OB_FAIL(sqc_build_ctx_.index_builder_->init(
            index_block_desc.get_desc(), // index_block_desc is copied in index_builder
            nullptr, // macro block flush callback
            ObSSTableIndexBuilder::DISABLE))) {
      LOG_WARN("failed to init index builder", K(ret), K(index_block_desc));
    } else if (OB_FAIL(sqc_build_ctx_.data_block_desc_.init(table_schema, ls_id_, tablet_id_,
            is_full_direct_load(direct_load_type_) ? compaction::ObMergeType::MAJOR_MERGE : compaction::ObMergeType::MINOR_MERGE,
            is_full_direct_load(direct_load_type_) ? table_key_.get_snapshot_version() : 1L,
            data_format_version_))) {
      LOG_WARN("fail to init data block desc", K(ret));
    } else {
      sqc_build_ctx_.data_block_desc_.get_desc().sstable_index_builder_ = sqc_build_ctx_.index_builder_; // for build the tail index block in macro block
    }


    if (OB_FAIL(ret)) {
      if (nullptr != sqc_build_ctx_.index_builder_) {
        sqc_build_ctx_.index_builder_->~ObSSTableIndexBuilder();
        sqc_build_ctx_.index_builder_ = nullptr;
      }
      if (nullptr != builder_buf) {
        sqc_build_ctx_.allocator_.free(builder_buf);
        builder_buf = nullptr;
      }
      sqc_build_ctx_.data_block_desc_.reset();
    }
  }
  return ret;
}

int ObTabletDirectLoadMgr::wrlock(const int64_t timeout_us, uint32_t &tid)
{
  int ret = OB_SUCCESS;
  const int64_t abs_timeout_us = timeout_us + ObTimeUtility::current_time();
  if (OB_SUCC(lock_.wrlock(ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK, abs_timeout_us))) {
    tid = static_cast<uint32_t>(GETTID());
  }
  if (OB_TIMEOUT == ret) {
    ret = OB_EAGAIN;
  }
  return ret;
}

void ObTabletDirectLoadMgr::unlock(const uint32_t tid)
{
  if (OB_SUCCESS != lock_.unlock(&tid)) {
    ob_abort();
  }
}


ObTabletFullDirectLoadMgr::ObTabletFullDirectLoadMgr()
  : ObTabletDirectLoadMgr(), start_scn_(share::SCN::min_scn()),
    commit_scn_(share::SCN::min_scn()), execution_id_(-1)
{
}

ObTabletFullDirectLoadMgr::~ObTabletFullDirectLoadMgr()
{
}

int ObTabletFullDirectLoadMgr::update(
    ObTabletDirectLoadMgr *lob_tablet_mgr,
    const ObTabletDirectLoadInsertParam &build_param)
{
  int ret = OB_SUCCESS;
  ObLSService *ls_service = nullptr;
  ObLSHandle ls_handle;
  ObStorageSchema *storage_schema = nullptr;
  ObArenaAllocator arena_allocator("dl_mgr_update", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObLatchWGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
  if (OB_UNLIKELY(!build_param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(build_param));
  } else if (OB_ISNULL(ls_service = MTL(ObLSService *))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), K(MTL_ID()));
  } else if (OB_FAIL(ls_service->get_ls(build_param.common_param_.ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(build_param));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                               build_param.common_param_.tablet_id_,
                                               tablet_handle_,
                                               ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
    LOG_WARN("get tablet handle failed", K(ret), K(build_param));
  } else if (OB_FAIL(tablet_handle_.get_obj()->load_storage_schema(arena_allocator, storage_schema))) {
    LOG_WARN("load storage schema failed", K(ret));
  } else if (nullptr != lob_tablet_mgr) {
    // has lob
    ObTabletDirectLoadInsertParam lob_param;
    ObSchemaGetterGuard schema_guard;
    ObTabletBindingMdsUserData ddl_data;
    const ObTableSchema *table_schema = nullptr;
    if (OB_FAIL(lob_param.assign(build_param))) {
      LOG_WARN("assign lob parameter failed", K(ret));
    } else if (OB_FAIL(tablet_handle_.get_obj()->ObITabletMdsInterface::get_ddl_data(share::SCN::max_scn(), ddl_data))) {
      LOG_WARN("get ddl data failed", K(ret));
    } else if (OB_FALSE_IT(lob_param.common_param_.tablet_id_ = ddl_data.lob_meta_tablet_id_)) {
    } else if (build_param.is_replay_) {
      // no need to update table id.
    } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
      MTL_ID(), schema_guard, lob_param.runtime_only_param_.schema_version_))) {
      LOG_WARN("get tenant schema failed", K(ret), K(MTL_ID()), K(lob_param));
    } else if (OB_FAIL(schema_guard.get_table_schema(MTL_ID(),
              lob_param.runtime_only_param_.table_id_, table_schema))) {
      LOG_WARN("get table schema failed", K(ret), K(lob_param));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table not exist", K(ret), K(lob_param));
    } else {
      lob_param.runtime_only_param_.table_id_ = table_schema->get_aux_lob_meta_tid();
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(lob_mgr_handle_.set_obj(lob_tablet_mgr))) {
      LOG_WARN("set lob direct load mgr failed", K(ret), K(lob_param));
    } else if (OB_FAIL(lob_mgr_handle_.get_obj()->update(nullptr, lob_param))) {
      LOG_WARN("init lob failed", K(ret), K(lob_param));
    } else {
      LOG_INFO("set lob mgr handle", K(lob_param));
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObTabletDirectLoadMgr::update(nullptr, build_param))) {
      LOG_WARN("init failed", K(ret), K(build_param));
    } else {
      table_key_.reset();
      table_key_.tablet_id_ = build_param.common_param_.tablet_id_;
      bool is_column_group_store = false;
      if (OB_FAIL(ObCODDLUtil::need_column_group_store(*storage_schema, is_column_group_store))) {
        LOG_WARN("fail to get schema is column group store", K(ret));
      } else if (is_column_group_store) {
        table_key_.table_type_ = ObITable::COLUMN_ORIENTED_SSTABLE;
        int64_t base_cg_idx = -1;
        if (OB_FAIL(ObCODDLUtil::get_base_cg_idx(storage_schema, base_cg_idx))) {
          LOG_WARN("get base cg idx failed", K(ret));
        } else {
          table_key_.column_group_idx_ = static_cast<uint16_t>(base_cg_idx);
        }
      } else {
        table_key_.table_type_ = ObITable::MAJOR_SSTABLE;
      }
      table_key_.version_range_.snapshot_version_ = build_param.common_param_.read_snapshot_;
    }
  }
  ObTabletObjLoadHelper::free(arena_allocator, storage_schema);
  LOG_INFO("init tablet direct load mgr finished", K(ret), K(build_param), KPC(this));
  return ret;
}

int ObTabletFullDirectLoadMgr::open(const int64_t current_execution_id, share::SCN &start_scn)
{
  int ret = OB_SUCCESS;
  uint32_t lock_tid = 0;
  ObLSService *ls_service = nullptr;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObTabletFullDirectLoadMgr *lob_tablet_mgr = nullptr;
  start_scn.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!is_valid() || !sqc_build_ctx_.is_valid() || current_execution_id < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), KPC(this), K(current_execution_id));
  } else if (OB_FAIL(wrlock(TRY_LOCK_TIMEOUT, lock_tid))) {
    LOG_WARN("failed to wrlock", K(ret), KPC(this));
  } else if (lob_mgr_handle_.is_valid()
    && OB_ISNULL(lob_tablet_mgr = lob_mgr_handle_.get_full_obj())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), KPC(this));
  } else if (OB_ISNULL(ls_service = MTL(ObLSService*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ls service should not be null", K(ret));
  } else if (OB_FAIL(ls_service->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ls_id_));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, tablet_id_, tablet_handle))) {
    LOG_WARN("fail to get tablet handle", K(ret), K(tablet_id_));
  } else if (OB_UNLIKELY(!tablet_handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet handle is invalid", K(ret), K(tablet_handle));
  } else if (current_execution_id < execution_id_
    || current_execution_id < tablet_handle.get_obj()->get_tablet_meta().ddl_execution_id_) {
    ret = OB_TASK_EXPIRED;
    LOG_INFO("receive a old execution id, don't do start", K(ret), K(current_execution_id), K(sqc_build_ctx_),
      "tablet_meta", tablet_handle.get_obj()->get_tablet_meta());
  } else if (get_commit_scn(tablet_handle.get_obj()->get_tablet_meta()).is_valid_and_not_min()) {
    // has already committed.
    start_scn = start_scn_;
    if (!start_scn.is_valid_and_not_min()) {
      start_scn = tablet_handle.get_obj()->get_tablet_meta().ddl_start_scn_;
    }
    if (!start_scn.is_valid_and_not_min()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("start scn must be valid after commit", K(ret), K(start_scn));
    }
  } else {
    ObDDLKvMgrHandle ddl_kv_mgr_handle;
    ObDDLKvMgrHandle lob_kv_mgr_handle;
    ObTabletDirectLoadMgrHandle direct_load_mgr_handle;
    if (OB_FAIL(direct_load_mgr_handle.set_obj(this))) {
      LOG_WARN("set handle failed", K(ret));
    } else if (OB_FAIL(tablet_handle.get_obj()->get_ddl_kv_mgr(ddl_kv_mgr_handle, true/*try_create*/))) {
      LOG_WARN("create ddl kv mgr failed", K(ret));
    } else if (nullptr != lob_tablet_mgr) {
      ObTabletHandle lob_tablet_handle;
      if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, lob_tablet_mgr->get_tablet_id(), lob_tablet_handle))) {
        LOG_WARN("get tablet handle failed", K(ret), K(ls_id_), KPC(lob_tablet_mgr));
      } else if (OB_FAIL(lob_tablet_handle.get_obj()->get_ddl_kv_mgr(lob_kv_mgr_handle, true/*try_create*/))) {
        LOG_WARN("create ddl kv mgr failed", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      ObDDLRedoLogWriter redo_writer;
      if (OB_FAIL(redo_writer.init(ls_id_, tablet_id_))) {
        LOG_WARN("init redo writer failed", K(ret), K(ls_id_), K(tablet_id_));
      } else if (OB_FAIL(redo_writer.write_start_log(table_key_,
        current_execution_id, sqc_build_ctx_.build_param_.common_param_.data_format_version_, direct_load_type_,
        ddl_kv_mgr_handle, lob_kv_mgr_handle, direct_load_mgr_handle, lock_tid, start_scn))) {
        LOG_WARN("fail write start log", K(ret), K(table_key_), K(data_format_version_), K(sqc_build_ctx_));
      } else if (OB_UNLIKELY(!start_scn.is_valid_and_not_min())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected err", K(ret), K(start_scn));
      } else if (OB_FAIL(init_ddl_table_store(start_scn, table_key_.get_snapshot_version(), start_scn))) {
        LOG_WARN("clean up ddl sstable failed", K(ret), K(start_scn), K(table_key_));
      } else if (nullptr != lob_tablet_mgr
        && OB_FAIL(lob_tablet_mgr->init_ddl_table_store(start_scn, table_key_.get_snapshot_version(), start_scn))) {
        LOG_WARN("clean up ddl sstable failed", K(ret), K(start_scn), K(table_key_));
      }
    }
  }
  if (lock_tid != 0) {
    unlock(lock_tid);
  }
  return ret;
}

int ObTabletFullDirectLoadMgr::close(const int64_t execution_id, const SCN &start_scn)
{
  int ret = OB_SUCCESS;
  SCN commit_scn;
  bool is_remote_write = false;
  ObLSService *ls_service = nullptr;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObTabletHandle new_tablet_handle;
  ObTabletFullDirectLoadMgr *lob_tablet_mgr = nullptr;
  bool sstable_already_created = false;
  const uint64_t tenant_id = MTL_ID();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(execution_id < 0 || !start_scn.is_valid_and_not_min())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(execution_id), K(start_scn));
  } else if (lob_mgr_handle_.is_valid()
    && OB_ISNULL(lob_tablet_mgr = lob_mgr_handle_.get_full_obj())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), KPC(this));
  } else if (OB_ISNULL(ls_service = MTL(ObLSService*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ls service should not be null", K(ret));
  } else if (OB_FAIL(ls_service->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ls_id_));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, tablet_id_, tablet_handle))) {
    LOG_WARN("fail to get tablet handle", K(ret), K(tablet_id_));
  } else if (OB_UNLIKELY(!tablet_handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet handle is invalid", K(ret), K(tablet_handle));
  } else {
    uint32_t lock_tid = 0;
    ObDDLRedoLogWriter redo_writer;
    if (OB_FAIL(wrlock(TRY_LOCK_TIMEOUT, lock_tid))) {
      LOG_WARN("failed to wrlock", K(ret), KPC(this));
    } else if (FALSE_IT(sstable_already_created = sqc_build_ctx_.is_task_end_)) {
    } else if (sstable_already_created) {
      // Why use is_task_end_ rather than commit_scn_.
      // sqc may switch to follower, and the commit_scn will not be set.
      LOG_INFO("had already closed", K(ret));
    } else if (get_commit_scn(tablet_handle.get_obj()->get_tablet_meta()).is_valid_and_not_min()) {
      commit_scn = get_commit_scn(tablet_handle.get_obj()->get_tablet_meta());
      FLOG_INFO("already committed", K(ret), K(commit_scn), "tablet_meta", tablet_handle.get_obj()->get_tablet_meta());
    } else if (OB_FAIL(redo_writer.init(ls_id_, tablet_id_))) {
      LOG_WARN("init redo writer failed", K(ret), K(ls_id_), K(tablet_id_));
    } else {
      DEBUG_SYNC(AFTER_REMOTE_WRITE_DDL_PREPARE_LOG);
      ObTabletDirectLoadMgrHandle direct_load_mgr_handle;
      if (OB_FAIL(direct_load_mgr_handle.set_obj(this))) {
        LOG_WARN("set direct load mgr handle failed", K(ret));
      } else if (OB_FAIL(redo_writer.write_commit_log(true, table_key_,
          start_scn, direct_load_mgr_handle, commit_scn, is_remote_write, lock_tid))) {
        LOG_WARN("fail write ddl commit log", K(ret), K(table_key_), K(sqc_build_ctx_));
      }
    }
    if (0 != lock_tid) {
      unlock(lock_tid);
    }
  }

  bool is_delay_build_major = false;
#ifdef ERRSIM
    is_delay_build_major = 0 != GCONF.errsim_ddl_major_delay_time;
    sqc_build_ctx_.is_task_end_ = is_delay_build_major ? true : sqc_build_ctx_.is_task_end_;  // skip report checksum
#endif
  if (OB_FAIL(ret) || sstable_already_created) {
  } else if (is_remote_write) {
    LOG_INFO("ddl commit log is written in remote, need wait replay", K(sqc_build_ctx_), K(start_scn), K(commit_scn));
  } else if (OB_UNLIKELY(!start_scn.is_valid_and_not_min()) || !commit_scn.is_valid_and_not_min()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected err", K(ret), KPC(this));
  } else if (OB_FAIL(commit(*tablet_handle.get_obj(), start_scn, commit_scn,
      sqc_build_ctx_.build_param_.runtime_only_param_.table_id_, sqc_build_ctx_.build_param_.runtime_only_param_.task_id_))) {
    LOG_WARN("failed to do ddl kv commit", K(ret), KPC(this));
  }

  if (OB_FAIL(ret)) {
  } else if (sstable_already_created || is_delay_build_major) {
    LOG_INFO("sstable had already created, skip waiting for major generated and reporting chksum", K(start_scn), K(commit_scn),
        K(sstable_already_created), K(is_delay_build_major));
  } else if (OB_FAIL(schedule_merge_task(start_scn, commit_scn, true/*wait_major_generate*/))) {
    LOG_WARN("schedule merge task and wait real major generate", K(ret),
        K(is_remote_write), K(sstable_already_created), K(start_scn), K(commit_scn));
  } else if (lob_mgr_handle_.is_valid() &&
      OB_FAIL(lob_mgr_handle_.get_full_obj()->schedule_merge_task(start_scn, commit_scn, true/*wait_major_generate*/))) {
    LOG_WARN("schedule merge task and wait real major generate for lob failed", K(ret),
        K(is_remote_write), K(sstable_already_created), K(start_scn), K(commit_scn));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, tablet_id_, new_tablet_handle))) {
    LOG_WARN("fail to get tablet handle", K(ret), K(tablet_id_));
  } else {
    ObSSTableMetaHandle sst_meta_hdl;
    ObSSTable *first_major_sstable = nullptr;
    ObTabletMemberWrapper<ObTabletTableStore> table_store_wrapper;
    if (OB_FAIL(new_tablet_handle.get_obj()->fetch_table_store(table_store_wrapper))) {
      LOG_WARN("fetch table store failed", K(ret));
    } else if (OB_ISNULL(first_major_sstable = static_cast<ObSSTable *>
      (table_store_wrapper.get_member()->get_major_sstables().get_boundary_table(false/*first*/)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("no major after wait merge success", K(ret), K(ls_id_), K(tablet_id_));
    } else if (OB_UNLIKELY(first_major_sstable->get_key() != table_key_)) {
      ret = OB_SNAPSHOT_DISCARDED;
      LOG_WARN("ddl major sstable dropped, snapshot holding may have bug",
        K(ret), KPC(first_major_sstable), K(table_key_), K(tablet_id_), K(sqc_build_ctx_.build_param_), K(sqc_build_ctx_.build_param_.runtime_only_param_.task_id_));
    } else if (OB_FAIL(first_major_sstable->get_meta(sst_meta_hdl))) {
      LOG_WARN("fail to get sstable meta handle", K(ret));
    } else {
      const int64_t *column_checksums = sst_meta_hdl.get_sstable_meta().get_col_checksum();
      int64_t column_count = sst_meta_hdl.get_sstable_meta().get_col_checksum_cnt();
      ObArray<int64_t> co_column_checksums;
      co_column_checksums.set_attr(ObMemAttr(MTL_ID(), "TblDL_Ccc"));
      if (OB_FAIL(get_co_column_checksums_if_need(tablet_handle, first_major_sstable, co_column_checksums))) {
        LOG_WARN("get column checksum from co sstable failed", K(ret));
      } else {
        for (int64_t retry_cnt = 10; retry_cnt > 0; retry_cnt--) { // overwrite ret
          if (OB_FAIL(ObTabletDDLUtil::report_ddl_checksum(
                  ls_id_,
                  tablet_id_,
                  sqc_build_ctx_.build_param_.runtime_only_param_.table_id_,
                  execution_id,
                  sqc_build_ctx_.build_param_.runtime_only_param_.task_id_,
                  co_column_checksums.empty() ? column_checksums : co_column_checksums.get_data(),
                  co_column_checksums.empty() ? column_count : co_column_checksums.count()))) {
            LOG_WARN("report ddl column checksum failed", K(ret), K(ls_id_), K(tablet_id_), K(execution_id), K(sqc_build_ctx_));
          } else {
            break;
          }
          ob_usleep(100L * 1000L);
        }
      }
    }

    if (OB_SUCC(ret)) {
      sqc_build_ctx_.is_task_end_ = true;
    }
  }
  return ret;
}

int ObTabletFullDirectLoadMgr::start_with_checkpoint(
    ObTablet &tablet,
    const share::SCN &start_scn,
    const uint64_t data_format_version,
    const int64_t execution_id,
    const share::SCN &checkpoint_scn)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!checkpoint_scn.is_valid_and_not_min())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(checkpoint_scn));
  } else if (OB_UNLIKELY(!table_key_.is_valid())) {
    ret = OB_ERR_SYS;
    LOG_WARN("the table key not updated", K(ret), KPC(this));
  } else {
    ObITable::TableKey table_key = table_key_;
    ret = start(tablet, table_key, start_scn, data_format_version, execution_id, checkpoint_scn);
  }
  return ret;
}

// For Leader and follower both.
// For replay start log only, migration_create_tablet and online will no call the intrface.
int ObTabletFullDirectLoadMgr::start(
    ObTablet &tablet,
    const ObITable::TableKey &table_key,
    const share::SCN &start_scn,
    const uint64_t data_format_version,
    const int64_t execution_id,
    const share::SCN &checkpoint_scn)
{
  int ret = OB_SUCCESS;
  share::SCN saved_start_scn;
  int64_t saved_snapshot_version = 0;
  ObDDLKvMgrHandle ddl_kv_mgr_handle;
  ObDDLKvMgrHandle lob_kv_mgr_handle;
  ddl_kv_mgr_handle.reset();
  lob_kv_mgr_handle.reset();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(table_key != table_key_)
    || !start_scn.is_valid_and_not_min()
    || execution_id < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arg", K(ret), K(table_key), K(table_key_), K(start_scn), K(execution_id));
  } else if (OB_FAIL(tablet.get_ddl_kv_mgr(ddl_kv_mgr_handle, true/*try_create*/))) {
    LOG_WARN("create tablet ddl kv mgr handle failed", K(ret));
  } else if (lob_mgr_handle_.is_valid()) {
    ObLSHandle ls_handle;
    ObTabletHandle lob_tablet_handle;
    if (OB_ISNULL(MTL(ObLSService *))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret));
    } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
      LOG_WARN("get ls failed", K(ret), K(ls_id_));
    } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, lob_mgr_handle_.get_obj()->get_tablet_id(), lob_tablet_handle))) {
      LOG_WARN("get tablet failed", K(ret));
    } else if (OB_FAIL(lob_tablet_handle.get_obj()->get_ddl_kv_mgr(lob_kv_mgr_handle, true/*try_create*/))) {
      LOG_WARN("create tablet ddl kv mgr handle failed", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    ObLSHandle ls_handle;
    if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
      LOG_WARN("get ls handle failed", K(ret), K(ls_id_));
    } else if (OB_ISNULL(ls_handle.get_ls()) || OB_ISNULL(ls_handle.get_ls()->get_ddl_log_handler())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ls or ddl log handler is null", K(ret), KPC(ls_handle.get_ls()), K(ls_id_));
    } else if (OB_FAIL(ls_handle.get_ls()->get_ddl_log_handler()->add_tablet(tablet_id_))) {
      LOG_WARN("add tablet id failed", K(ret), K(ls_id_), K(tablet_id_));
    } else if (lob_kv_mgr_handle.is_valid() && OB_FAIL(ls_handle.get_ls()->get_ddl_log_handler()->add_tablet(lob_mgr_handle_.get_obj()->get_tablet_id()))) {
      LOG_WARN("add lob tablet id failed", K(ret), "lob_tablet_id", lob_mgr_handle_.get_obj()->get_tablet_id());
    }
  }
  if (OB_SUCC(ret)) {
    ObLatchWGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
    if (OB_FAIL(start_nolock(table_key, start_scn, data_format_version, execution_id, checkpoint_scn,
        ddl_kv_mgr_handle, lob_kv_mgr_handle))) {
      LOG_WARN("failed to ddl start", K(ret));
    } else {
      // save variables under lock
      saved_start_scn = start_scn_;
      saved_snapshot_version = table_key_.get_snapshot_version();
      const SCN ddl_commit_scn = get_commit_scn(tablet.get_tablet_meta());
      commit_scn_.atomic_store(ddl_commit_scn);
      if (lob_mgr_handle_.is_valid()) {
        lob_mgr_handle_.get_full_obj()->set_commit_scn_nolock(ddl_commit_scn);
      }
    }
  }
  if (OB_SUCC(ret) && !checkpoint_scn.is_valid_and_not_min()) {
    // remove ddl sstable if exists and flush ddl start log ts and snapshot version into tablet meta.
    // persist lob meta tablet before data tablet is necessary, to avoid start-loss for lob meta tablet when recovered from checkpoint.
    if (lob_mgr_handle_.is_valid() &&
      OB_FAIL(lob_mgr_handle_.get_full_obj()->init_ddl_table_store(saved_start_scn, saved_snapshot_version, saved_start_scn))) {
      LOG_WARN("clean up ddl sstable failed", K(ret));
    } else if (OB_FAIL(init_ddl_table_store(saved_start_scn, saved_snapshot_version, saved_start_scn))) {
      LOG_WARN("clean up ddl sstable failed", K(ret), K(tablet_id_));
    }
  }
  FLOG_INFO("start full direct load mgr finished", K(ret), K(start_scn), K(execution_id), KPC(this));
  return ret;
}

int ObTabletFullDirectLoadMgr::start_nolock(
    const ObITable::TableKey &table_key,
    const share::SCN &start_scn,
    const uint64_t data_format_version,
    const int64_t execution_id,
    const SCN &checkpoint_scn,
    ObDDLKvMgrHandle &ddl_kv_mgr_handle,
    ObDDLKvMgrHandle &lob_kv_mgr_handle)
{
  int ret = OB_SUCCESS;
  bool is_brand_new = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!table_key.is_valid() || !start_scn.is_valid_and_not_min() || data_format_version < 0 || execution_id < 0
      || (checkpoint_scn.is_valid_and_not_min() && checkpoint_scn < start_scn)) || !ddl_kv_mgr_handle.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(table_key), K(start_scn), K(data_format_version), K(execution_id), K(checkpoint_scn),
      "kv_mgr_handle is valid", ddl_kv_mgr_handle.is_valid());
  } else if (table_key.get_tablet_id() != tablet_id_ || table_key_ != table_key) {
    ret = OB_ERR_SYS;
    LOG_WARN("tablet id not same", K(ret), K(table_key), K(table_key_), K(tablet_id_));
  } else {
    if (start_scn_.is_valid_and_not_min()) {
      if (execution_id >= execution_id_ && start_scn >= start_scn_) {
        is_brand_new = true;
        LOG_INFO("execution id changed, need cleanup", K(ls_id_), K(tablet_id_), K(execution_id_), K(execution_id), K(start_scn_), K(start_scn));
      } else {
        if (!checkpoint_scn.is_valid_and_not_min()) {
          // only return error code when not start from checkpoint.
          ret = OB_TASK_EXPIRED;
        }
        LOG_INFO("ddl start ignored", K(ls_id_), K(tablet_id_), K(execution_id_), K(execution_id), K(start_scn_), K(start_scn), K(checkpoint_scn));
      }
    } else {
      is_brand_new = true;
      FLOG_INFO("ddl start brand new", K(table_key), K(start_scn), K(execution_id), KPC(this));
    }
    if (OB_SUCC(ret) && is_brand_new) {
      if (OB_FAIL(cleanup_unlock())) {
        LOG_WARN("cleanup unlock failed", K(ret));
      } else {
        table_key_ = table_key;
        data_format_version_ = data_format_version;
        execution_id_ = execution_id;
        start_scn_.atomic_store(start_scn);
        ddl_kv_mgr_handle.get_obj()->set_max_freeze_scn(SCN::max(start_scn, checkpoint_scn));
        sqc_build_ctx_.reset_slice_ctx_on_demand();
      }
    }
  }
  if (OB_SUCC(ret) && lob_mgr_handle_.is_valid()) {
    // For lob meta tablet recover from checkpoint, execute start itself to avoid the data loss when,
    // 1. lob meta tablet recover from checkpoint;
    // 2. replay some data redo log on lob meta tablet.
    // 3. data tablet recover from checkpoint, and cleanup will be triggered if lob meta tablet
    //    execute start again.
    ObDDLKvMgrHandle unused_kv_mgr_handle;
    ObITable::TableKey lob_table_key;
    lob_table_key.tablet_id_ = lob_mgr_handle_.get_full_obj()->get_tablet_id();
    lob_table_key.table_type_ = ObITable::TableType::MAJOR_SSTABLE; // lob tablet not support column group store
    lob_table_key.version_range_ = table_key.version_range_;
    if (OB_FAIL(lob_mgr_handle_.get_full_obj()->start_nolock(lob_table_key, start_scn, data_format_version, execution_id, checkpoint_scn,
        lob_kv_mgr_handle, unused_kv_mgr_handle))) {
      LOG_WARN("start nolock for lob meta tablet failed", K(ret));
    }
  }
  FLOG_INFO("start_nolock full direct load mgr finished", K(ret), K(start_scn), K(execution_id), KPC(this));
  return ret;
}

int ObTabletFullDirectLoadMgr::commit(
    ObTablet &tablet,
    const share::SCN &start_scn,
    const share::SCN &commit_scn,
    const uint64_t table_id,
    const int64_t ddl_task_id)
{
  int ret = OB_SUCCESS;
  ObDDLKvMgrHandle ddl_kv_mgr_handle;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (!is_started()) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("ddl not started", K(ret), KPC(this));
  } else if (start_scn < get_start_scn()) {
    ret = OB_TASK_EXPIRED;
    LOG_INFO("skip ddl commit log", K(start_scn), K(*this));
  } else if (OB_FAIL(set_commit_scn(commit_scn))) {
    LOG_WARN("failed to set commit scn", K(ret));
  } else if (OB_FAIL(tablet.get_ddl_kv_mgr(ddl_kv_mgr_handle))) {
    LOG_WARN("create ddl kv mgr failed", K(ret));
  } else if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->freeze_ddl_kv(
    start_scn, table_key_.get_snapshot_version(), data_format_version_, commit_scn))) {
    LOG_WARN("failed to start prepare", K(ret), K(tablet_id_), K(commit_scn));
  } else {
    ret = OB_EAGAIN;
    while (OB_EAGAIN == ret) {
      if (OB_FAIL(update_major_sstable())) {
        LOG_WARN("update ddl major sstable failed", K(ret), K(tablet_id_), K(start_scn), K(commit_scn));
      }
      if (OB_EAGAIN == ret) {
        usleep(1000L);
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(schedule_merge_task(start_scn, commit_scn, false/*wait_major_generate*/))) {
        LOG_WARN("schedule major merge task failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret) && lob_mgr_handle_.is_valid()) {
    const ObLSID &ls_id = lob_mgr_handle_.get_full_obj()->get_ls_id();
    const ObTabletID &lob_tablet_id = lob_mgr_handle_.get_full_obj()->get_tablet_id();
    ObLSHandle ls_handle;
    ObLS *ls = nullptr;
    ObTabletHandle lob_tablet_handle;
    if (OB_ISNULL(MTL(ObLSService *))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected err", K(ret), K(MTL_ID()));
    } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id, ls_handle, ObLSGetMod::DDL_MOD))) {
      LOG_WARN("get ls failed", K(ret), K(ls_id));
    } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("ls should not be null", K(ret));
    } else if (OB_FAIL(ls->get_tablet(lob_tablet_id, lob_tablet_handle, ObTabletCommon::DEFAULT_GET_TABLET_DURATION_US, ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
      LOG_WARN("get tablet handle failed", K(ret), K(ls_id), K(lob_tablet_id));
    } else if (OB_FAIL(lob_mgr_handle_.get_full_obj()->commit(*lob_tablet_handle.get_obj(), start_scn, commit_scn))) {
      LOG_WARN("commit for lob failed", K(ret), K(start_scn), K(commit_scn));
    }
  }
  return ret;
}

int ObTabletFullDirectLoadMgr::schedule_merge_task(const share::SCN &start_scn, const share::SCN &commit_scn, const bool wait_major_generated)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!start_scn.is_valid_and_not_min() || !commit_scn.is_valid_and_not_min())) {
    ret = OB_ERR_SYS;
    LOG_WARN("unknown start scn or commit snc", K(ret), K(start_scn), K(commit_scn));
  } else {
    const int64_t wait_start_ts = ObTimeUtility::fast_current_time();
    while (OB_SUCC(ret)) {
      if (OB_FAIL(THIS_WORKER.check_status())) {
        LOG_WARN("check status failed", K(ret));
      } else {
        ObDDLTableMergeDagParam param;
        param.direct_load_type_    = direct_load_type_;
        param.ls_id_               = ls_id_;
        param.tablet_id_           = tablet_id_;
        param.rec_scn_             = commit_scn;
        param.is_commit_           = true;
        param.start_scn_           = start_scn;
        param.data_format_version_ = data_format_version_;
        param.snapshot_version_    = table_key_.get_snapshot_version();
        if (OB_FAIL(compaction::ObScheduleDagFunc::schedule_ddl_table_merge_dag(param))) {
          if (OB_SIZE_OVERFLOW != ret && OB_EAGAIN != ret) {
            LOG_WARN("schedule ddl merge dag failed", K(ret), K(param));
          } else {
            ret = OB_SUCCESS;
          }
        } else if (!wait_major_generated) {
          // schedule successfully and no need to wait physical major generates.
          break;
        }
      }
      if (OB_SUCC(ret)) {
        const ObSSTable *first_major_sstable = nullptr;
        ObTabletMemberWrapper<ObTabletTableStore> table_store_wrapper;
        if (OB_FAIL(ObTabletDDLUtil::check_and_get_major_sstable(ls_id_, tablet_id_, first_major_sstable, table_store_wrapper))) {
          LOG_WARN("check if major sstable exist failed", K(ret));
        } else if (nullptr != first_major_sstable) {
          FLOG_INFO("major has already existed", KPC(this));
          break;
        }
      }
      if (REACH_TIME_INTERVAL(10L * 1000L * 1000L)) {
        LOG_INFO("wait build ddl sstable", K(ret), K(ls_id_), K(tablet_id_), K(start_scn), K(commit_scn),
            "wait_elpased_s", (ObTimeUtility::fast_current_time() - wait_start_ts) / 1000000L);
      }
    }
  }
  return ret;
}

void ObTabletFullDirectLoadMgr::set_commit_scn_nolock(const share::SCN &scn)
{
  commit_scn_.atomic_store(scn);
  if (lob_mgr_handle_.is_valid()) {
    lob_mgr_handle_.get_full_obj()->set_commit_scn_nolock(scn);
  }
}

int ObTabletFullDirectLoadMgr::set_commit_scn(const share::SCN &commit_scn)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (OB_UNLIKELY(!commit_scn.is_valid_and_not_min())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(commit_scn));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(ls_id_));
  } else if (OB_FAIL(ls_handle.get_ls()->get_tablet(tablet_id_,
                                                    tablet_handle,
                                                    ObTabletCommon::DEFAULT_GET_TABLET_DURATION_US,
                                                    ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
    LOG_WARN("get tablet handle failed", K(ret), K(ls_id_), K(tablet_id_));
  } else {
    ObLatchWGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
    const share::SCN old_commit_scn = get_commit_scn(tablet_handle.get_obj()->get_tablet_meta());
    if (old_commit_scn.is_valid_and_not_min() && old_commit_scn != commit_scn) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("already committed by others", K(ret), K(commit_scn), KPC(this));
    } else {
      commit_scn_.atomic_store(commit_scn);
    }
  }
  return ret;
}

share::SCN ObTabletFullDirectLoadMgr::get_commit_scn(const ObTabletMeta &tablet_meta)
{
  share::SCN mgr_commit_scn = commit_scn_.atomic_load();
  share::SCN commit_scn = share::SCN::min_scn();
  if (tablet_meta.ddl_commit_scn_.is_valid_and_not_min() || mgr_commit_scn.is_valid_and_not_min()) {
    if (tablet_meta.ddl_commit_scn_.is_valid_and_not_min()) {
      commit_scn = tablet_meta.ddl_commit_scn_;
    } else {
      commit_scn = mgr_commit_scn;
    }
  } else {
    commit_scn = share::SCN::min_scn();
  }
  return commit_scn;
}

share::SCN ObTabletFullDirectLoadMgr::get_start_scn()
{
  return start_scn_.atomic_load();
}

int ObTabletFullDirectLoadMgr::can_schedule_major_compaction_nolock(
    const ObTablet &tablet,
    bool &can_schedule)
{
  int ret = OB_SUCCESS;
  can_schedule = false;
  share::SCN commit_scn;
  const ObTabletMeta &tablet_meta = tablet.get_tablet_meta();
  ObTabletMemberWrapper<ObTabletTableStore> table_store_wrapper;

  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(tablet.fetch_table_store(table_store_wrapper))) {
    LOG_WARN("fetch table store failed", K(ret));
  } else if (nullptr != table_store_wrapper.get_member()->get_major_sstables().get_boundary_table(false/*first*/)) {
    // major sstable has already existed.
  } else {
    can_schedule = get_commit_scn(tablet_meta).is_valid_and_not_min() ? true : false;
  }
  return ret;
}

int ObTabletFullDirectLoadMgr::prepare_ddl_merge_param(
    const ObTablet &tablet,
    ObDDLTableMergeDagParam &merge_param)
{
  int ret = OB_SUCCESS;
  bool can_schedule = false;
  ObLatchRGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
  if (OB_FAIL(can_schedule_major_compaction_nolock(tablet, can_schedule))) {
    LOG_WARN("check can schedule major compaction failed", K(ret));
  } else if (can_schedule) {
    merge_param.direct_load_type_ = direct_load_type_;
    merge_param.ls_id_ = ls_id_;
    merge_param.tablet_id_ = tablet_id_;
    merge_param.rec_scn_ = get_commit_scn(tablet.get_tablet_meta());
    merge_param.is_commit_ = true;
    merge_param.start_scn_ = start_scn_;
    merge_param.data_format_version_ = data_format_version_;
    merge_param.snapshot_version_    = table_key_.get_snapshot_version();
  } else {
    merge_param.direct_load_type_ = direct_load_type_;
    merge_param.ls_id_ = ls_id_;
    merge_param.tablet_id_ = tablet_id_;
    merge_param.start_scn_ = start_scn_;
    merge_param.data_format_version_ = data_format_version_;
    merge_param.snapshot_version_    = table_key_.get_snapshot_version();
  }
  return ret;
}

int ObTabletFullDirectLoadMgr::prepare_major_merge_param(
    ObTabletDDLParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret), K(is_inited_));
  } else if (!is_started()) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("ddl not started", K(ret));
  } else {
    ObLatchRGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
    param.direct_load_type_ = direct_load_type_;
    param.ls_id_ = ls_id_;
    param.table_key_ = table_key_;
    param.start_scn_ = start_scn_;
    param.commit_scn_ = commit_scn_;
    param.snapshot_version_ = table_key_.get_snapshot_version();
    param.data_format_version_ = data_format_version_;
  }
  return ret;
}

int ObTabletFullDirectLoadMgr::cleanup_unlock()
{
  int ret = OB_SUCCESS;
  LOG_INFO("cleanup expired sstables", K(*this));
  ObLS *ls = nullptr;
  ObLSService *ls_service = nullptr;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObDDLKvMgrHandle ddl_kv_mgr_handle;
  if (OB_ISNULL(ls_service = MTL(ObLSService*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ls service should not be null", K(ret));
  } else if (OB_FAIL(ls_service->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("get ls failed", K(ret), K(ls_id_));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle, tablet_id_, tablet_handle))) {
    LOG_WARN("fail to get tablet handle", K(ret), K(tablet_id_));
  } else if (OB_UNLIKELY(!tablet_handle.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("need replay but tablet handle is invalid", K(ret), K(tablet_handle));
  } else if (OB_FAIL(tablet_handle.get_obj()->get_ddl_kv_mgr(ddl_kv_mgr_handle))) {
    LOG_WARN("create ddl kv mgr failed", K(ret));
  } else if (OB_FAIL(ddl_kv_mgr_handle.get_obj()->cleanup())) {
    LOG_WARN("cleanup failed", K(ret));
  } else {
    table_key_.reset();
    data_format_version_ = 0;
    start_scn_.atomic_store(share::SCN::min_scn());
    commit_scn_.atomic_store(share::SCN::min_scn());
    execution_id_ = -1;
  }
  return ret;
}

int ObTabletFullDirectLoadMgr::init_ddl_table_store(
    const share::SCN &start_scn,
    const int64_t snapshot_version,
    const share::SCN &ddl_checkpoint_scn)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  ObArenaAllocator tmp_arena("DDLUpdateTblTmp", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID());
  ObStorageSchema *storage_schema = nullptr;
  bool is_column_group_store = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_UNLIKELY(!start_scn.is_valid_and_not_min() || snapshot_version <= 0 || !ddl_checkpoint_scn.is_valid_and_not_min())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(start_scn), K(snapshot_version), K(ddl_checkpoint_scn));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(ls_id_));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                               tablet_id_,
                                               tablet_handle,
                                               ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
    LOG_WARN("get tablet handle failed", K(ret), K(ls_id_), K(tablet_id_));
  } else if (OB_FAIL(tablet_handle.get_obj()->load_storage_schema(tmp_arena, storage_schema))) {
    LOG_WARN("failed to load storage schema", K(ret), K(tablet_handle));
  } else if (OB_FAIL(ObCODDLUtil::need_column_group_store(*storage_schema, is_column_group_store))) {
    LOG_WARN("fail to check schema is column group store", K(ret));
  }
  else {
    ObTableHandleV2 table_handle; // empty
    const int64_t rebuild_seq = ls_handle.get_ls()->get_rebuild_seq();
    ObTableHandleV2 sstable_handle;
    ObTabletHandle new_tablet_handle;
    ObTablesHandleArray empty_cg_sstable_handles;
    ObArray<const ObDataMacroBlockMeta *> empty_meta_array;
    empty_meta_array.set_attr(ObMemAttr(MTL_ID(), "TblFDL_EMA"));

    ObLatchRGuard guard(lock_, ObLatchIds::TABLET_DIRECT_LOAD_MGR_LOCK);
    ObTabletDDLParam ddl_param;
    ddl_param.direct_load_type_ = direct_load_type_;
    ddl_param.ls_id_ = ls_id_;
    ddl_param.table_key_ = table_key_;
    ddl_param.start_scn_ = start_scn;
    ddl_param.commit_scn_ = commit_scn_;
    ddl_param.snapshot_version_ = table_key_.get_snapshot_version();
    ddl_param.data_format_version_ = data_format_version_;
    ddl_param.table_key_.table_type_ = is_column_group_store ? ObITable::DDL_MERGE_CO_SSTABLE : ObITable::DDL_DUMP_SSTABLE;
    ddl_param.table_key_.scn_range_.start_scn_ = SCN::scn_dec(start_scn);
    ddl_param.table_key_.scn_range_.end_scn_ = start_scn;

    ObUpdateTableStoreParam param(tablet_handle.get_obj()->get_snapshot_version(),
                                  ObVersionRange::MIN_VERSION, // multi_version_start
                                  storage_schema,
                                  rebuild_seq);
    param.ddl_info_.keep_old_ddl_sstable_ = false;
    param.ddl_info_.ddl_start_scn_ = start_scn;
    param.ddl_info_.ddl_snapshot_version_ = snapshot_version;
    param.ddl_info_.ddl_checkpoint_scn_ = ddl_checkpoint_scn;
    param.ddl_info_.ddl_execution_id_ = execution_id_;
    param.ddl_info_.data_format_version_ = data_format_version_;
    if (OB_FAIL(ObTabletDDLUtil::create_ddl_sstable(*tablet_handle.get_obj(), ddl_param, empty_meta_array, nullptr/*first_ddl_sstable*/,
        tmp_arena, sstable_handle))) {
      LOG_WARN("create empty ddl sstable failed", K(ret));
    } else if (ddl_param.table_key_.is_co_sstable()) {
      // add empty cg sstables
      ObCOSSTableV2 *co_sstable = static_cast<ObCOSSTableV2 *>(sstable_handle.get_table());
      const ObIArray<ObStorageColumnGroupSchema> &cg_schemas = storage_schema->get_column_groups();
      ObTabletDDLParam cg_ddl_param = ddl_param;
      cg_ddl_param.table_key_.table_type_ = ObITable::TableType::DDL_MERGE_CG_SSTABLE;
      for (int64_t i = 0; OB_SUCC(ret) && i < cg_schemas.count(); ++i) {
        ObTableHandleV2 cur_handle;
        cg_ddl_param.table_key_.column_group_idx_ = static_cast<uint16_t>(i);
        if (table_key_.get_column_group_id() == i) {
          // skip base cg idx
        } else if (OB_FAIL(ObTabletDDLUtil::create_ddl_sstable(*tablet_handle.get_obj(), cg_ddl_param, empty_meta_array, nullptr/*first_ddl_sstable*/, tmp_arena, cur_handle))) {
          LOG_WARN("create empty cg sstable failed", K(ret), K(i), K(cg_ddl_param));
        } else if (OB_FAIL(empty_cg_sstable_handles.add_table(cur_handle))) {
          LOG_WARN("add table handle failed", K(ret), K(i), K(cur_handle));
        }
      }
      if (OB_SUCC(ret)) {
        ObArray<ObITable *> cg_sstables;
        cg_sstables.set_attr(ObMemAttr(MTL_ID(), "TblFDL_CGS"));
        if (OB_FAIL(empty_cg_sstable_handles.get_tables(cg_sstables))) {
          LOG_WARN("get cg sstables failed", K(ret));
        } else if (OB_FAIL(co_sstable->fill_cg_sstables(cg_sstables))) {
          LOG_WARN("fill empty cg sstables failed", K(ret));
        } else {
          LOG_DEBUG("fill co sstable with empty cg sstables success", K(ret), K(ddl_param), KPC(co_sstable));
        }
      }
    }
    bool is_column_group_store = false;
    if (OB_FAIL(ret)) {
    } else if (FALSE_IT(param.sstable_ = static_cast<ObSSTable *>(sstable_handle.get_table()))) {
    } else if (OB_FAIL(ls_handle.get_ls()->update_tablet_table_store(tablet_id_, param, new_tablet_handle))) {
      LOG_WARN("failed to update tablet table store", K(ret), K(ls_id_), K(tablet_id_), K(param));
    } else if (OB_FAIL(ObCODDLUtil::need_column_group_store(*storage_schema, is_column_group_store))) {
      LOG_WARN("failed to check storage schema is column group store", K(ret));
    } else {
      LOG_INFO("update tablet success", K(ls_id_), K(tablet_id_),
          "is_column_store", is_column_group_store, K(ddl_param),
          "column_group_schemas", storage_schema->get_column_groups(),
          "update_table_store_param", param, K(start_scn), K(snapshot_version), K(ddl_checkpoint_scn));
    }
  }
  ObTabletObjLoadHelper::free(tmp_arena, storage_schema);
  return ret;
}

int ObTabletFullDirectLoadMgr::update_major_sstable()
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObTabletHandle tablet_handle;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ls_handle, ObLSGetMod::DDL_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(ls_id_));
  } else if (OB_FAIL(ObDDLUtil::ddl_get_tablet(ls_handle,
                                               tablet_id_,
                                               tablet_handle,
                                               ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
    LOG_WARN("get tablet handle failed", K(ret), K(ls_id_), K(tablet_id_));
  } else {
    SCN ddl_commit_scn = get_commit_scn(tablet_handle.get_obj()->get_tablet_meta());
    if (OB_ISNULL(ls_handle.get_ls()->get_tablet_svr())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ls tablet service is null", K(ret), K(ls_id_));
    } else if (OB_FAIL(ls_handle.get_ls()->get_tablet_svr()->update_tablet_ddl_commit_scn(tablet_id_, ddl_commit_scn))) {
      LOG_WARN("update ddl commit scn failed", K(ret), K(ls_id_), K(tablet_id_), K(ddl_commit_scn));
    }
  }
  return ret;
}
