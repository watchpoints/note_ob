/**
 * Copyright (c) 2024 OceanBase
 * OceanBase is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_OBSERVER_OB_TABLE_TENANT_GROUP_H_
#define OCEANBASE_OBSERVER_OB_TABLE_TENANT_GROUP_H_

#include "ob_table_group_execute.h"

namespace oceanbase
{

namespace table
{

#define TABLEAPI_GROUP_COMMIT_MGR (MTL(ObTableGroupCommitMgr*))
typedef hash::ObHashMap<uint64_t, ObTableLsGroup*> ObTableGroupCommitMap;
typedef hash::ObHashMap<uint64_t, ObTableLsGroupInfo> ObTableGroupInfoMap; // use for statistic display
struct ObGetExpiredLsGroupOp
{
  explicit ObGetExpiredLsGroupOp(int64_t max_active_ts);
  int operator()(common::hash::HashMapPair<uint64_t, ObTableLsGroup*> &entry);
  int64_t max_active_ts_;
  int64_t cur_ts_;
  common::ObSEArray<uint64_t, 16> expired_ls_groups_;
};

struct ObCreateLsGroupOp
{
  explicit ObCreateLsGroupOp(ObTableGroupInfoMap &group_info_map, ObTableGroupCommitKey &commit_key)
    : group_info_map_(group_info_map),
      commit_key_(commit_key)
  {}
  int operator()(const common::hash::HashMapPair<uint64_t, ObTableLsGroup*> &entry);
  ObTableGroupInfoMap &group_info_map_;
  ObTableGroupCommitKey &commit_key_;
};

struct ObEraseLsGroupIfEmptyOp
{
  explicit ObEraseLsGroupIfEmptyOp(ObTableGroupInfoMap &group_info_map, int64_t max_active_ts)
    : group_info_map_(group_info_map),
      max_active_ts_(max_active_ts)
  {
    cur_ts_ = common::ObTimeUtility::fast_current_time();
  }

  bool operator()(common::hash::HashMapPair<uint64_t, ObTableLsGroup*> &entry);
  ObTableGroupInfoMap &group_info_map_;
  int64_t max_active_ts_;
  int64_t cur_ts_;
};

class ObTableGroupCommitMgr final
{
public:
  static const int64_t DEFAULT_GROUP_SIZE = 32;
  static const int64_t DEFAULT_ENABLE_GROUP_COMMIT_OPS = 1000;
  friend class ObTableGroupInfoTask;

  ObTableGroupCommitMgr()
      : is_inited_(false),
        is_group_commit_disable_(false),
        allocator_(MTL_ID()),
        group_allocator_("TbGroupAlloc", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID()),
        op_allocator_("TbOpAlloc", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID()),
        failed_groups_allocator_("TbFgroupAlloc", OB_MALLOC_NORMAL_BLOCK_SIZE, MTL_ID()),
        group_map_(),
        group_info_map_(),
        failed_groups_(failed_groups_allocator_),
        expired_groups_(),
        statis_and_trigger_task_(*this),
        group_size_and_ops_task_(*this),
        group_factory_(group_allocator_),
        op_factory_(op_allocator_),
        put_op_group_size_(0),
        get_op_group_size_(0),
        last_write_ops_(0),
        last_read_ops_(0)
  {}
  virtual ~ObTableGroupCommitMgr() {}
  TO_STRING_KV(K_(is_inited),
               K_(failed_groups),
               K_(expired_groups),
               K_(group_factory),
               K_(op_factory),
               K_(put_op_group_size),
               K_(get_op_group_size),
               K_(last_write_ops),
               K_(last_read_ops));
public:
  static int mtl_init(ObTableGroupCommitMgr *&mgr) { return mgr->init(); }
  int start();
  void stop();
  void wait();
  void destroy();
  int init();
  int start_timer();
  OB_INLINE void set_group_commit_disable(bool disable) { ATOMIC_STORE(&is_group_commit_disable_, disable); }
  OB_INLINE bool is_group_commit_disable() const { return ATOMIC_LOAD(&is_group_commit_disable_); }
  OB_INLINE ObTableGroupOpsCounter& get_ops_counter() { return ops_counter_; }
  OB_INLINE ObTableGroupCommitMap& get_group_map() { return group_map_; }
  OB_INLINE ObTableGroupInfoMap& get_group_info_map() { return group_info_map_; }
  OB_INLINE void set_last_write_ops(int64_t ops) { last_write_ops_ = ops; }
  OB_INLINE int64_t get_last_write_ops() const { return last_write_ops_; }
  OB_INLINE void set_last_read_ops(int64_t ops) { last_read_ops_ = ops; }
  OB_INLINE int64_t get_last_read_ops() const { return last_read_ops_; }
  OB_INLINE int64_t get_last_ops() const { return last_read_ops_ + last_write_ops_; }
  OB_INLINE ObTableGroupCommitSingleOp* alloc_op() { return op_factory_.alloc(); }
  OB_INLINE void free_op(ObTableGroupCommitSingleOp *op) { op_factory_.free(op); }
  OB_INLINE ObTableGroupCommitOps* alloc_group() { return group_factory_.alloc(); }
  OB_INLINE void free_group(ObTableGroupCommitOps *group) { group_factory_.free(group); }
  OB_INLINE bool has_failed_groups() const { return !failed_groups_.empty(); }
  OB_INLINE ObTableFailedGroups& get_failed_groups() { return failed_groups_; }
  OB_INLINE bool has_expired_groups() const { return !expired_groups_.is_groups_empty(); }
  OB_INLINE ObTableExpiredGroups& get_expired_groups() { return expired_groups_; }
  OB_INLINE ObTableGroupFactory<ObTableGroupCommitOps>& get_group_factory() { return group_factory_; }
  OB_INLINE ObTableGroupFactory<ObTableGroupCommitSingleOp>& get_op_factory() { return op_factory_; }
  int64_t get_group_size(bool is_read) const;
  int create_and_add_ls_group(const ObTableGroupCtx &ctx);
private:
  int clean_group_map();
  int clean_expired_groups();
  int clean_failed_groups();
public:
	class ObTableGroupTriggerTask : public common::ObTimerTask
  {
  public:
    static const int64_t TASK_SCHEDULE_INTERVAL = 10 * 1000 ; // 10ms
    ObTableGroupTriggerTask(ObTableGroupCommitMgr &mgr)
        : group_mgr_(mgr)
    {}
    virtual void runTimerTask(void) override;
  private:
    int run_trigger_task();
    int trigger_other_group();
    int trigger_failed_group();
    int trigger_expire_group();
  private:
    ObTableGroupCommitMgr &group_mgr_;
  };
  class ObTableGroupInfoTask : public common::ObTimerTask
  {
  public:
    static const int64_t TASK_SCHEDULE_INTERVAL = 1000 * 1000; // 1s
    static const int64_t LS_GROUP_MAX_ACTIVE_TS = 10 * 60 * 1000 * 1000; // 10min
    static const int64_t MAX_CLEAN_GROUP_SIZE_EACH_TASK = 100;
    ObTableGroupInfoTask(ObTableGroupCommitMgr &mgr)
        : need_update_group_info_(false),
          group_mgr_(mgr)
    {}
    virtual void runTimerTask(void) override;
    void update_group_info_task();
    void update_ops_task();
    void clean_expired_group_task();
  private:
    bool need_update_group_info_;
    ObTableGroupCommitMgr &group_mgr_;
  };
private:
  bool is_inited_;
  bool is_group_commit_disable_;
  common::ObFIFOAllocator allocator_;
  common::ObArenaAllocator group_allocator_;
  common::ObArenaAllocator op_allocator_;
  common::ObArenaAllocator failed_groups_allocator_;
  common::ObTimer timer_;
  ObTableGroupCommitMap group_map_;
  ObTableGroupInfoMap group_info_map_;
  ObTableFailedGroups failed_groups_;
  ObTableExpiredGroups expired_groups_;
  ObTableGroupOpsCounter ops_counter_;
  ObTableGroupTriggerTask statis_and_trigger_task_;
  ObTableGroupInfoTask group_size_and_ops_task_;
  ObTableGroupFactory<ObTableGroupCommitOps> group_factory_;
  ObTableGroupFactory<ObTableGroupCommitSingleOp> op_factory_;
  ObTableAtomicValue<int64_t> put_op_group_size_;
  ObTableAtomicValue<int64_t> get_op_group_size_;
  int64_t last_write_ops_;
  int64_t last_read_ops_;
};

} // end namespace table
} // end namespace oceanbase
#endif /* OCEANBASE_OBSERVER_OB_TABLE_TENANT_GROUP_H_ */