//
// Created by vasilis on 11/05/20.
//

#ifndef KITE_KVS_UTILITY_H
#define KITE_KVS_UTILITY_H

#include <common_func.h>
#include "kvs.h"
#include "generic_util.h"
#include "debug_util.h"
#include "config_util.h"
#include "client_if_util.h"

static inline bool search_out_of_epoch_writes(struct pending_ops *p_ops,
                                              struct key *read_key,
                                              uint16_t t_id, void **val_ptr);
static inline void update_commit_logs(uint16_t t_id, uint32_t bkt, uint32_t log_no, uint8_t *old_value,
                                      uint8_t *value, const char* message, uint8_t flag);
static inline void activate_RMW_entry(uint8_t state, uint32_t new_version, mica_op_t  *kv_ptr,
                                      uint8_t opcode, uint8_t new_ts_m_id, uint64_t l_id, uint16_t glob_sess_id,
                                      uint32_t log_no, uint16_t t_id, const char* message);
static inline struct r_rep_big* get_r_rep_ptr(struct pending_ops *p_ops, uint64_t l_id,
                                              uint8_t rem_m_id, uint8_t read_opcode, bool coalesce,
                                              uint16_t t_id);
static inline bool does_rmw_fail_early(struct trace_op *op, mica_op_t *kv_ptr,
                                       struct kvs_resp *resp, uint16_t t_id);
static inline bool is_log_smaller_or_has_rmw_committed(uint32_t log_no, mica_op_t *kv_ptr,
                                                       uint64_t rmw_l_id,
                                                       uint16_t glob_sess_id, uint16_t t_id,
                                                       struct rmw_rep_last_committed *rep);
static inline bool is_log_too_high(uint32_t log_no, mica_op_t *kv_ptr,
                                   uint16_t t_id,
                                   struct rmw_rep_last_committed *rep);
static inline bool ts_is_not_greater_than_kvs_ts(mica_op_t *kv_ptr, struct network_ts_tuple *ts,
                                                 uint8_t m_id, uint16_t t_id,
                                                 struct rmw_rep_last_committed *rep);
static inline uint8_t handle_remote_prop_or_acc_in_kvs(mica_op_t *kv_ptr, void *prop_or_acc,
                                                       uint8_t sender_m_id, uint16_t t_id,
                                                       struct rmw_rep_last_committed *rep, uint32_t log_no,
                                                       bool is_prop);
static inline void register_last_committed_rmw_id_by_remote_accept(mica_op_t *kv_ptr,
                                                                   struct accept *acc , uint16_t t_id);
static inline uint16_t get_size_from_opcode(uint8_t opcode);
static inline void finish_r_rep_bookkeeping(struct pending_ops *p_ops, struct r_rep_big *rep,
                                            bool false_pos, uint8_t rem_m_id, uint16_t t_id);
static inline uint64_t handle_remote_commit_message(mica_op_t *kv_ptr, void* op, bool use_commit, uint16_t t_id);
static inline void set_up_r_rep_message_size(struct pending_ops *p_ops,
                                             struct r_rep_big *r_rep,
                                             struct network_ts_tuple *remote_ts,
                                             bool read_ts,
                                             uint16_t t_id);
static inline void set_up_rmw_acq_rep_message_size(struct pending_ops *p_ops,
                                                   uint8_t opcode, uint16_t t_id);
static inline void insert_r_rep(struct pending_ops *p_ops, uint64_t l_id, uint16_t t_id,
                                uint8_t rem_m_id, bool coalesce,  uint8_t read_opcode);

/* ---------------------------------------------------------------------------
//------------------------------ KVS------------------------------------------
//---------------------------------------------------------------------------*/

/*-----------------------------FROM TRACE---------------------------------------------*/

// Handle a local read/acquire in the KVS
static inline void KVS_from_trace_reads_and_acquires(struct trace_op *op,
                                                     mica_op_t *kv_ptr, struct kvs_resp *resp,
                                                     struct pending_ops *p_ops, uint32_t *r_push_ptr_,
                                                     uint16_t t_id)
{
  if (ENABLE_ASSERTIONS) assert(op->real_val_len <= VALUE_SIZE);
  uint64_t kv_epoch = 0;
  struct ts_tuple kvs_tuple;
  uint32_t r_push_ptr = *r_push_ptr_;
  struct read_info *r_info = &p_ops->read_info[r_push_ptr];
  //Lock free reads through versioning (successful when version is even)
  uint32_t debug_cntr = 0;
  bool value_forwarded = false; // has a pending out-of-epoch write forwarded its value to this
  if (op->opcode == KVS_OP_GET && p_ops->p_ooe_writes->size > 0) {
    uint8_t *val_ptr;
    if (search_out_of_epoch_writes(p_ops, &op->key, t_id, (void **) &val_ptr)) {
      memcpy(op->value_to_read, val_ptr, op->real_val_len);
      //memcpy(p_ops->read_info[r_push_ptr].value, val_ptr, VALUE_SIZE);
      //my_printf(red, "Wrkr %u Forwarding a value \n", t_id);
      value_forwarded = true;
    }
  }
  if (!value_forwarded) {
    uint64_t tmp_lock = read_seqlock_lock_free(&kv_ptr->seqlock);
    do {
      kv_epoch = kv_ptr->epoch_id;
      kvs_tuple = kv_ptr->ts;
      debug_stalling_on_lock(&debug_cntr, "trace read/acquire", t_id);
      //memcpy(p_ops->read_info[r_push_ptr].value, kv_ptr->value, VALUE_SIZE);
      if (ENABLE_ASSERTIONS) assert(op->value_to_read != NULL);
      memcpy(op->value_to_read, kv_ptr->value, op->real_val_len);
      //printf("Reading val %u from key %u \n", kv_ptr->value[0], kv_ptr->key.bkt);
    } while (!(check_seqlock_lock_free(&kv_ptr->seqlock, &tmp_lock)));
  }
  // Do a quorum read if the stored value is old and may be stale or it is an Acquire!
  if (!value_forwarded &&
      (kv_epoch < epoch_id || op->opcode == OP_ACQUIRE)) {
    r_info->opcode = op->opcode;
    r_info->ts_to_read.m_id = kvs_tuple.m_id;
    r_info->ts_to_read.version = kvs_tuple.version;
    r_info->key = op->key;
    r_info->r_ptr = r_push_ptr;
    // Copy the value in the read info too.
    memcpy(r_info->value, op->value_to_read, op->real_val_len);
    r_info->value_to_read = op->value_to_read;
    r_info->val_len = op->real_val_len;
    if (ENABLE_ASSERTIONS) op->ts.version = kvs_tuple.version;
    resp->type = KVS_GET_SUCCESS;
    if (ENABLE_STAT_COUNTING && op->opcode == KVS_OP_GET) {
      t_stats[t_id].quorum_reads++;
    }
    MOD_ADD(r_push_ptr, PENDING_READS);
  }
  else { //stored value can be read locally or has been forwarded
    resp->type = KVS_LOCAL_GET_SUCCESS;
    // this is needed to trick the version check in batch_from_trace_to_cache()
    if (ENABLE_ASSERTIONS) op->ts.version = 0;
  }
  (*r_push_ptr_) =  r_push_ptr;
}

// Handle a local write in the KVS
static inline void KVS_from_trace_writes(struct trace_op *op,
                                         mica_op_t *kv_ptr, struct kvs_resp *resp,
                                         struct pending_ops *p_ops, uint32_t *r_push_ptr_,
                                         uint16_t t_id)
{
  if (ENABLE_ASSERTIONS) assert(op->real_val_len <= VALUE_SIZE);
//  if (ENABLE_ASSERTIONS) assert(op->val_len == kv_ptr->val_len);
  struct node * new_node = (struct node *) op->value_to_write;
  lock_seqlock(&kv_ptr->seqlock);
  // OUT_OF_EPOCH--first round will be a read TS
  if (kv_ptr->epoch_id < epoch_id) {
    uint32_t r_push_ptr = *r_push_ptr_;
    struct read_info *r_info = &p_ops->read_info[r_push_ptr];
    r_info->ts_to_read.m_id = kv_ptr->ts.m_id;
    r_info->ts_to_read.version = kv_ptr->ts.version;
    unlock_seqlock(&kv_ptr->seqlock);
    r_info->opcode = op->opcode;
    r_info->key = op->key;
    r_info->r_ptr = r_push_ptr;
    if (ENABLE_ASSERTIONS) op->ts.version = r_info->ts_to_read.version;
    // Store the value to be written in the read_info to be used in the second round
    memcpy(r_info->value, op->value_to_write, op->real_val_len);

    //my_printf(yellow, "Out of epoch write key %u, node-next key_id %u \n",
    //             op->key.bkt, new_node->next_key_id);
    r_info->val_len = op->real_val_len;
    p_ops->p_ooe_writes->r_info_ptrs[p_ops->p_ooe_writes->push_ptr] = r_push_ptr;
    p_ops->p_ooe_writes->size++;
    MOD_ADD(p_ops->p_ooe_writes->push_ptr, PENDING_READS);
    MOD_ADD(r_push_ptr, PENDING_READS);
    resp->type = KVS_GET_TS_SUCCESS;
    (*r_push_ptr_) =  r_push_ptr;
  }
  else { // IN-EPOCH
    if (ENABLE_ASSERTIONS) {
      update_commit_logs(t_id, kv_ptr->key.bkt, op->ts.version, kv_ptr->value,
                         op->value_to_write, "local write", LOG_WS);
    }
    memcpy(kv_ptr->value, op->value_to_write, op->real_val_len);
    //printf("Wrote val %u to key %u \n", kv_ptr->value[0], kv_ptr->key.bkt);
    // This also writes the new version to op
    kv_ptr->ts.m_id = (uint8_t) machine_id;
    kv_ptr->ts.version++;
    op->ts.version = kv_ptr->ts.version;
    unlock_seqlock(&kv_ptr->seqlock);
    resp->type = KVS_PUT_SUCCESS;
  }
}


// Handle a local release in the KVS
static inline void KVS_from_trace_releases(struct trace_op *op,
                                           mica_op_t *kv_ptr, struct kvs_resp *resp,
                                           struct pending_ops *p_ops, uint32_t *r_push_ptr_,
                                           uint16_t t_id)
{
  if (ENABLE_ASSERTIONS) assert(op->real_val_len <= VALUE_SIZE);
  struct ts_tuple kvs_tuple;
  uint32_t r_push_ptr = *r_push_ptr_;
  struct read_info *r_info = &p_ops->read_info[r_push_ptr];
  uint32_t debug_cntr = 0;
  uint64_t tmp_lock = read_seqlock_lock_free(&kv_ptr->seqlock);
  do {
    kvs_tuple = kv_ptr->ts;
    debug_stalling_on_lock(&debug_cntr, "trace releases", t_id);
  } while (!(check_seqlock_lock_free(&kv_ptr->seqlock, &tmp_lock)));

  if (ENABLE_ASSERTIONS) op->ts.version = kvs_tuple.version;
  r_info->ts_to_read.m_id = kvs_tuple.m_id;
  r_info->ts_to_read.version = kvs_tuple.version;
  r_info->key = op->key;
  r_info->opcode = op->opcode;
  r_info->r_ptr = r_push_ptr;
  // Store the value to be written in the read_info to be used in the second round
  memcpy(r_info->value, op->value_to_write, op->real_val_len);
  r_info->val_len = op->real_val_len;
  MOD_ADD(r_push_ptr, PENDING_READS);
  resp->type = KVS_GET_TS_SUCCESS;
  (*r_push_ptr_) =  r_push_ptr;
}

// Handle a local rmw in the KVS
static inline void KVS_from_trace_rmw(struct trace_op *op,
                                      mica_op_t *kv_ptr, struct kvs_resp *resp,
                                      struct pending_ops *p_ops, uint64_t *rmw_l_id_,
                                      uint16_t op_i, uint16_t t_id)
{
  uint64_t rmw_l_id = *rmw_l_id_;
  if (DEBUG_RMW) my_printf(green, "Worker %u trying a local RMW on op %u\n", t_id, op_i);
  uint32_t new_version = (ENABLE_ALL_ABOARD && op->attempt_all_aboard) ?
                         ALL_ABOARD_TS : PAXOS_TS;
  lock_seqlock(&kv_ptr->seqlock);
  {
    check_trace_op_key_vs_kv_ptr(op, kv_ptr);
    check_log_nos_of_kv_ptr(kv_ptr, "KVS_batch_op_trace", t_id);
    if (kv_ptr->state == INVALID_RMW) {
      if (!does_rmw_fail_early(op, kv_ptr, resp, t_id)) {
        activate_RMW_entry(PROPOSED, new_version, kv_ptr, op->opcode,
                           (uint8_t) machine_id, rmw_l_id,
                           get_glob_sess_id((uint8_t) machine_id, t_id, op->session_id),
                           kv_ptr->last_committed_log_no + 1, t_id, ENABLE_ASSERTIONS ? "batch to trace" : NULL);
        resp->type = RMW_SUCCESS;
        if (ENABLE_ASSERTIONS) assert(kv_ptr->log_no == kv_ptr->last_committed_log_no + 1);
      }
    } else {
      // This is the state the RMW will wait on
      resp->kv_ptr_ts = kv_ptr->prop_ts;
      resp->kv_ptr_state = kv_ptr->state;
      resp->kv_ptr_rmw_id = kv_ptr->rmw_id;
      resp->type = RETRY_RMW_KEY_EXISTS;
    }
    resp->log_no = kv_ptr->log_no;
    resp->kv_ptr = kv_ptr;
    // We need to put the new timestamp in the op too, both to send it and to store it for later
    op->ts.version = new_version;
  }
  unlock_seqlock(&kv_ptr->seqlock);
  if (resp->type != RMW_FAILURE) (*rmw_l_id_)++;
}

// Handle a local rmw acquire in the KVS
static inline void KVS_from_trace_rmw_acquire(struct trace_op *op, mica_op_t *kv_ptr,
                                              struct kvs_resp *resp, struct pending_ops *p_ops,
                                              uint32_t *r_push_ptr_, uint16_t t_id)
{
  if (ENABLE_ASSERTIONS)
    assert((ENABLE_RMW_ACQUIRES && RMW_ACQUIRE_RATIO) || ENABLE_CLIENTS > 0);
  //printf("rmw acquire\n");
  uint32_t r_push_ptr = *r_push_ptr_;
  struct read_info *r_info = &p_ops->read_info[r_push_ptr];
  lock_seqlock(&kv_ptr->seqlock);
//  if (kv_ptr->opcode == KEY_HAS_NEVER_BEEN_RMWED) {
//    r_info->log_no = 0;
//  }
//  else {
//    if (ENABLE_ASSERTIONS) assert(kv_ptr->opcode == KEY_HAS_BEEN_RMWED);
  check_keys_with_one_trace_op(&op->key, kv_ptr);
  r_info->log_no = kv_ptr->last_committed_log_no;
  r_info->rmw_id = kv_ptr->last_committed_rmw_id;
//  }
  r_info->ts_to_read.version = kv_ptr->ts.version;
  r_info->ts_to_read.m_id = kv_ptr->ts.m_id;
  memcpy(op->value_to_read, kv_ptr->value, op->real_val_len);
  unlock_seqlock(&kv_ptr->seqlock);

  // Copy the value to the read_info too
  memcpy(r_info->value, op->value_to_read, op->real_val_len);
  r_info->value_to_read = op->value_to_read;
  r_info->val_len = op->real_val_len;
  if (ENABLE_ASSERTIONS) op->ts.version = r_info->ts_to_read.version;
  r_info->key = op->key;
  r_info->is_rmw = true;
  r_info->opcode = OP_ACQUIRE;
  r_info->r_ptr = r_push_ptr;
  MOD_ADD(r_push_ptr, PENDING_READS);
  resp->type = KVS_GET_SUCCESS;
  (*r_push_ptr_) =  r_push_ptr;
}

// Handle a local relaxed read in the KVS
static inline void KVS_from_trace_rmw_rlxd_read(struct trace_op *op, mica_op_t *kv_ptr,
                                                struct kvs_resp *resp, struct pending_ops *p_ops,
                                                uint32_t *r_push_ptr_, uint16_t t_id)
{

  //printf("rmw acquire\n");
  uint32_t r_push_ptr = *r_push_ptr_;
  struct read_info *r_info = &p_ops->read_info[r_push_ptr];
  lock_seqlock(&kv_ptr->seqlock);
  memcpy(op->value_to_read, kv_ptr->value, op->real_val_len);
  unlock_seqlock(&kv_ptr->seqlock);
  resp->type = KVS_LOCAL_GET_SUCCESS;
  // this is needed to trick the version check in batch_from_trace_to_cache()
  if (ENABLE_ASSERTIONS) op->ts.version = 0;

}

/*-----------------------------UPDATES---------------------------------------------*/

// Handle a remote release/write or acquire-write the KVS
static inline void KVS_updates_writes_or_releases_or_acquires(struct write *op,
                                                              mica_op_t *kv_ptr, uint16_t t_id)
{
  //my_printf(red, "received op %u with value %u \n", op->opcode, op->value[0]);
//  if (ENABLE_ASSERTIONS) assert(op->val_len == kv_ptr->val_len);
  lock_seqlock(&kv_ptr->seqlock);
  if (compare_netw_ts_with_ts((struct network_ts_tuple*) &op, &kv_ptr->ts) == GREATER) {
    update_commit_logs(t_id, kv_ptr->key.bkt, op->version, kv_ptr->value,
                       op->value, "rem write", LOG_WS);
    memcpy(kv_ptr->value, op->value, VALUE_SIZE);
    //printf("Wrote val %u to key %u \n", kv_ptr->value[0], kv_ptr->key.bkt);
    kv_ptr->ts.m_id = op->m_id;
    kv_ptr->ts.version = op->version;
    unlock_seqlock(&kv_ptr->seqlock);

  } else {
    unlock_seqlock(&kv_ptr->seqlock);
    if (ENABLE_STAT_COUNTING) t_stats[t_id].failed_rem_writes++;
  }
}


// Handle a remote RMW accept message in the KVS
static inline void KVS_updates_accepts(struct accept *acc, mica_op_t *kv_ptr,
                                       struct pending_ops *p_ops,
                                       uint16_t op_i, uint16_t t_id)
{
//  struct accept *acc = (struct accept *) (((void *)op) + 3); // the accept starts at an offset of 3 bytes
  if (ENABLE_ASSERTIONS) {
    assert(acc->last_registered_rmw_id.id != acc->t_rmw_id ||
           acc->last_registered_rmw_id.glob_sess_id != acc->glob_sess_id);
    assert(acc->ts.version > 0);
  }
  // on replying to the accept we may need to send on or more of TS, VALUE, RMW-id, log-no
  //struct rmw_help_entry reply_rmw;
  uint64_t rmw_l_id = acc->t_rmw_id;
  uint16_t glob_sess_id = acc->glob_sess_id;
  //my_printf(cyan, "Received accept with rmw_id %u, glob_sess %u \n", rmw_l_id, glob_sess_id);
  uint32_t log_no = acc->log_no;
  uint64_t l_id = acc->l_id;

  struct w_message *acc_mes = (struct w_message *) p_ops->ptrs_to_mes_headers[op_i];
  if (ENABLE_ASSERTIONS) check_accept_mes(acc_mes);
  uint8_t acc_m_id = acc_mes->m_id;
  uint8_t opcode_for_r_rep = (uint8_t)
    (acc_mes->opcode == ONLY_ACCEPTS ? ACCEPT_OP : ACCEPT_OP_NO_CREDITS);
  struct rmw_rep_last_committed *acc_rep =
    (struct rmw_rep_last_committed *) get_r_rep_ptr(p_ops, l_id, acc_m_id, opcode_for_r_rep,
                                                    p_ops->coalesce_r_rep[op_i], t_id);
  acc_rep->l_id = l_id;

  if (DEBUG_RMW) my_printf(green, "Worker %u is handling a remote RMW accept on op %u from m_id %u "
                             "l_id %u, rmw_l_id %u, glob_ses_id %u, log_no %u, version %u  \n",
                           t_id, op_i, acc_m_id, l_id, rmw_l_id, glob_sess_id, log_no, acc->ts.version);
  lock_seqlock(&kv_ptr->seqlock);
  // 1. check if it has been committed
  // 2. first check the log number to see if it's SMALLER!! (leave the "higher" part after the KVS ts is also checked)
  // Either way fill the reply_rmw fully, but have a specialized flag!
  if (!is_log_smaller_or_has_rmw_committed(log_no, kv_ptr, rmw_l_id, glob_sess_id,
                                           t_id, acc_rep)) {
    // 3. Check that the TS is higher than the KVS TS, setting the flag accordingly
    //if (!ts_is_not_greater_than_kvs_ts(kv_ptr, &acc->ts, acc_m_id, t_id, acc_rep)) {
    // 4. If the kv-pair has not been RMWed before grab an entry and ack
    // 5. Else if log number is bigger than the current one, ack without caring about the ongoing RMWs
    // 6. Else check the kv_ptr and send a response depending on whether there is an ongoing RMW and what that is
    acc_rep->opcode = handle_remote_prop_or_acc_in_kvs(kv_ptr, (void*) acc, acc_m_id, t_id, acc_rep, log_no, false);
    // if the accepted is going to be acked record its information in the kv_ptr
    if (acc_rep->opcode == RMW_ACK) {
      activate_RMW_entry(ACCEPTED, acc->ts.version, kv_ptr, acc->opcode,
                         acc->ts.m_id, rmw_l_id, glob_sess_id, log_no, t_id,
                         ENABLE_ASSERTIONS ? "received accept" : NULL);
      memcpy(kv_ptr->last_accepted_value, acc->value, (size_t) RMW_VALUE_SIZE);
      assign_netw_ts_to_ts(&kv_ptr->base_acc_ts, &acc->base_ts);
      if (log_no - 1 > kv_ptr->last_registered_log_no) {
        register_last_committed_rmw_id_by_remote_accept(kv_ptr, acc, t_id);
        assign_net_rmw_id_to_rmw_id(&kv_ptr->last_registered_rmw_id, &acc->last_registered_rmw_id);
        kv_ptr->last_registered_log_no = log_no -1;
      }
    }
  }
  uint64_t number_of_reqs = 0;
  if (ENABLE_DEBUG_GLOBAL_ENTRY) {
    // kv_ptr->dbg->prop_acc_num++;
    // number_of_reqs = kv_ptr->dbg->prop_acc_num;
  }
  check_log_nos_of_kv_ptr(kv_ptr, "Unlocking after received accept", t_id);
  unlock_seqlock(&kv_ptr->seqlock);
  if (PRINT_LOGS)
    fprintf(rmw_verify_fp[t_id], "Key: %u, log %u: Req %lu, Acc: m_id:%u, rmw_id %lu, glob_sess id: %u, "
              "version %u, m_id: %u, resp: %u \n",
            kv_ptr->key.bkt, log_no, number_of_reqs, acc_m_id, rmw_l_id, glob_sess_id ,acc->ts.version, acc->ts.m_id, acc_rep->opcode);
  //set_up_rmw_rep_message_size(p_ops, acc_rep->opcode, t_id);
  p_ops->r_rep_fifo->message_sizes[p_ops->r_rep_fifo->push_ptr]+= get_size_from_opcode(acc_rep->opcode);
  if (ENABLE_ASSERTIONS) assert(p_ops->r_rep_fifo->message_sizes[p_ops->r_rep_fifo->push_ptr] <= R_REP_SEND_SIZE);
  finish_r_rep_bookkeeping(p_ops, (struct r_rep_big*) acc_rep, false, acc_m_id, t_id);

}

// Handle a remote RMW commit message in the KVS
static inline void KVS_updates_commits(struct commit *com, mica_op_t *kv_ptr,
                                       struct pending_ops *p_ops,
                                       uint16_t op_i, uint16_t t_id)
{
  if (DEBUG_RMW)
    my_printf(green, "Worker %u is handling a remote RMW commit on com %u, "
                "rmw_l_id %u, glob_ses_id %u, log_no %u, version %u  \n",
              t_id, op_i, com->t_rmw_id, com->glob_sess_id, com->log_no, com->base_ts.version);

  uint64_t number_of_reqs;
  number_of_reqs = handle_remote_commit_message(kv_ptr, (void*) com, true, t_id);
  if (PRINT_LOGS) {
    struct w_message *com_mes = (struct w_message *) p_ops->ptrs_to_mes_headers[op_i];
    uint8_t acc_m_id = com_mes->m_id;
    fprintf(rmw_verify_fp[t_id], "Key: %u, log %u: Req %lu, Com: m_id:%u, rmw_id %lu, glob_sess id: %u, "
              "version %u, m_id: %u \n",
            kv_ptr->key.bkt, com->log_no, number_of_reqs, acc_m_id, com->t_rmw_id, com->glob_sess_id, com->base_ts.version, com->base_ts.m_id);
  }
}

/*-----------------------------READS---------------------------------------------*/

// Handle remote reads, acquires and acquires-fp
// (acquires-fp are acquires renamed by the receiver when a false positive is detected)
static inline void KVS_reads_gets_or_acquires_or_acquires_fp(struct read *read, mica_op_t *kv_ptr,
                                                             struct pending_ops *p_ops, uint16_t op_i,
                                                             uint16_t t_id)
{
  //Lock free reads through versioning (successful when version is even)
  uint32_t debug_cntr = 0;
  uint8_t rem_m_id = p_ops->ptrs_to_mes_headers[op_i]->m_id;
  struct r_rep_big *r_rep = get_r_rep_ptr(p_ops, p_ops->ptrs_to_mes_headers[op_i]->l_id,
                                          rem_m_id, read->opcode, p_ops->coalesce_r_rep[op_i], t_id);

  uint64_t tmp_lock = read_seqlock_lock_free(&kv_ptr->seqlock);
  do {
    debug_stalling_on_lock(&debug_cntr, "reads: gets_or_acquires_or_acquires_fp", t_id);
    r_rep->ts.m_id = kv_ptr->ts.m_id;
    r_rep->ts.version = kv_ptr->ts.version;
    if (compare_netw_ts(&r_rep->ts, &read->ts) == GREATER) {
      memcpy(r_rep->value, kv_ptr->value, VALUE_SIZE);
    }
  } while (!(check_seqlock_lock_free(&kv_ptr->seqlock, &tmp_lock)));
  set_up_r_rep_message_size(p_ops, r_rep, &read->ts, false, t_id);
  finish_r_rep_bookkeeping(p_ops, r_rep, read->opcode == OP_ACQUIRE_FP, rem_m_id, t_id);
  //if (r_rep->opcode > ACQ_LOG_EQUAL) printf("big opcode leaves \n");

}

// Handle remote requests to get TS that are the first round of a release or of an out-of-epoch write
static inline void KVS_reads_get_TS(struct read *read, mica_op_t *kv_ptr,
                                    struct pending_ops *p_ops, uint16_t op_i,
                                    uint16_t t_id)
{
  uint32_t debug_cntr = 0;
  uint8_t rem_m_id = p_ops->ptrs_to_mes_headers[op_i]->m_id;
  struct r_rep_big *r_rep = get_r_rep_ptr(p_ops, p_ops->ptrs_to_mes_headers[op_i]->l_id,
                                          rem_m_id, read->opcode, p_ops->coalesce_r_rep[op_i], t_id);

  uint64_t tmp_lock = read_seqlock_lock_free(&kv_ptr->seqlock);
  do {
    debug_stalling_on_lock(&debug_cntr, "reads: get-TS-read version", t_id);
    r_rep->ts.m_id = kv_ptr->ts.m_id;
    r_rep->ts.version = kv_ptr->ts.version;
  } while (!(check_seqlock_lock_free(&kv_ptr->seqlock, &tmp_lock)));
  set_up_r_rep_message_size(p_ops, r_rep, &read->ts, true, t_id);
  finish_r_rep_bookkeeping(p_ops, r_rep, false, rem_m_id, t_id);
}

// Handle remote proposes
static inline void KVS_reads_proposes(struct read *read, mica_op_t *kv_ptr,
                                      struct pending_ops *p_ops, uint16_t op_i,
                                      uint16_t t_id)
{
  struct propose *prop = (struct propose *) read; //(((void *)read) + 3); // the propose starts at an offset of 5 bytes
  if (DEBUG_RMW) my_printf(green, "Worker %u trying a remote RMW propose on op %u\n", t_id, op_i);
  if (ENABLE_ASSERTIONS) assert(prop->ts.version > 0);
  uint64_t number_of_reqs = 0;
  uint64_t rmw_l_id = prop->t_rmw_id;
  uint64_t l_id = prop->l_id;
  uint16_t glob_sess_id = prop->glob_sess_id;
  //my_printf(cyan, "Received propose with rmw_id %u, glob_sess %u \n", rmw_l_id, glob_sess_id);
  uint32_t log_no = prop->log_no;
  uint8_t prop_m_id = p_ops->ptrs_to_mes_headers[op_i]->m_id;
  struct rmw_rep_last_committed *prop_rep =
    (struct rmw_rep_last_committed *) get_r_rep_ptr(p_ops, l_id, prop_m_id, read->opcode,
                                                    p_ops->coalesce_r_rep[op_i], t_id);
  prop_rep->l_id = prop->l_id;
  //my_printf(green, "Sending prop_rep lid %u to m _id %u \n", prop_rep->l_id, prop_m_id);

  lock_seqlock(&kv_ptr->seqlock);
  {
    //check_for_same_ts_as_already_proposed(kv_ptr[I], prop, t_id);
    // 1. check if it has been committed
    // 2. first check the log number to see if it's SMALLER!! (leave the "higher" part after the KVS ts is also checked)
    // Either way fill the reply_rmw fully, but have a specialized flag!
    if (!is_log_smaller_or_has_rmw_committed(log_no, kv_ptr, rmw_l_id, glob_sess_id, t_id, prop_rep)) {
      if (!is_log_too_high(log_no, kv_ptr, t_id, prop_rep)) {
        // 3. Check that the TS is higher than the KVS TS, setting the flag accordingly
        //if (!ts_is_not_greater_than_kvs_ts(kv_ptr, &prop->ts, prop_m_id, t_id, prop_rep)) {
        // 4. If the kv-pair has not been RMWed before grab an entry and ack
        // 5. Else if log number is bigger than the current one, ack without caring about the ongoing RMWs
        // 6. Else check the kv_ptr and send a response depending on whether there is an ongoing RMW and what that is
        prop_rep->opcode = handle_remote_prop_or_acc_in_kvs(kv_ptr, (void *) prop, prop_m_id, t_id,
                                                            prop_rep, prop->log_no, true);
        // if the propose is going to be acked record its information in the kv_ptr
        if (prop_rep->opcode == RMW_ACK) {
          assert(prop->log_no >= kv_ptr->log_no);
          activate_RMW_entry(PROPOSED, prop->ts.version, kv_ptr, prop->opcode,
                             prop->ts.m_id, rmw_l_id, glob_sess_id, log_no, t_id,
                             ENABLE_ASSERTIONS ? "received propose" : NULL);
        }
        if (ENABLE_ASSERTIONS) {
          assert(kv_ptr->prop_ts.version >= prop->ts.version);
          check_keys_with_one_trace_op(&prop->key, kv_ptr);
        }
        //}
      }
    }
    if (ENABLE_DEBUG_GLOBAL_ENTRY) {
      // kv_ptr->dbg->prop_acc_num++;
      // number_of_reqs = kv_ptr->dbg->prop_acc_num;
    }
    check_log_nos_of_kv_ptr(kv_ptr, "Unlocking after received propose", t_id);
  }
  unlock_seqlock(&kv_ptr->seqlock);
  if (PRINT_LOGS && ENABLE_DEBUG_GLOBAL_ENTRY)
    fprintf(rmw_verify_fp[t_id], "Key: %u, log %u: Req %lu, Prop: m_id:%u, rmw_id %lu, glob_sess id: %u, "
              "version %u, m_id: %u, resp: %u \n",  kv_ptr->key.bkt, log_no, number_of_reqs, prop_m_id,
            rmw_l_id, glob_sess_id, prop->ts.version, prop->ts.m_id, prop_rep->opcode);
  p_ops->r_rep_fifo->message_sizes[p_ops->r_rep_fifo->push_ptr]+= get_size_from_opcode(prop_rep->opcode);
  if (ENABLE_ASSERTIONS) assert(p_ops->r_rep_fifo->message_sizes[p_ops->r_rep_fifo->push_ptr] <= R_REP_SEND_SIZE);
  bool false_pos = take_ownership_of_a_conf_bit(rmw_l_id, prop_m_id, true, t_id);
  finish_r_rep_bookkeeping(p_ops, (struct r_rep_big*) prop_rep, false_pos, prop_m_id, t_id);
  //struct rmw_rep_message *rmw_mes = (struct rmw_rep_message *) &p_ops->r_rep_fifo->r_rep_message[p_ops->r_rep_fifo->push_ptr];

}

// Handle remote rmw-acquires and rmw-acquires-fp
// (acquires-fp are acquires renamed by the receiver when a false positive is detected)
static inline void KVS_reads_rmw_acquires(struct read *read, mica_op_t *kv_ptr,
                                          struct pending_ops *p_ops, uint16_t op_i,
                                          uint16_t t_id)
{
  uint64_t  l_id = p_ops->ptrs_to_mes_headers[op_i]->l_id;
  uint8_t rem_m_id = p_ops->ptrs_to_mes_headers[op_i]->m_id;
  struct rmw_acq_rep *acq_rep =
    (struct rmw_acq_rep *) get_r_rep_ptr(p_ops, l_id, rem_m_id, read->opcode, p_ops->coalesce_r_rep[op_i], t_id);

  uint32_t acq_log_no = read->ts.version;
  lock_seqlock(&kv_ptr->seqlock);
  {

    check_keys_with_one_trace_op(&read->key, kv_ptr);
    if (kv_ptr->last_committed_log_no > acq_log_no) {
      acq_rep->opcode = ACQ_LOG_TOO_SMALL;
      acq_rep->rmw_id = kv_ptr->last_registered_rmw_id.id;
      acq_rep->glob_sess_id = kv_ptr->last_committed_rmw_id.glob_sess_id;
      memcpy(acq_rep->value, kv_ptr->value, (size_t) RMW_VALUE_SIZE);
      acq_rep->log_no = kv_ptr->last_committed_log_no;
      acq_rep->ts.version = kv_ptr->ts.version;
      acq_rep->ts.m_id = kv_ptr->ts.m_id;
    }
    else if (kv_ptr->last_committed_log_no < acq_log_no) {
      acq_rep->opcode = ACQ_LOG_TOO_HIGH;
    }
    else acq_rep->opcode = ACQ_LOG_EQUAL;

  }
  unlock_seqlock(&kv_ptr->seqlock);
  set_up_rmw_acq_rep_message_size(p_ops, acq_rep->opcode, t_id);
  finish_r_rep_bookkeeping(p_ops, (struct r_rep_big *) acq_rep,
                           read->opcode == OP_ACQUIRE_FP, rem_m_id, t_id);
}


/*-----------------------------READ-COMMITTING---------------------------------------------*/
// On a read reply, we may want to write the KVS, if the TS has not been seen
static inline void KVS_out_of_epoch_writes(struct read_info *op, mica_op_t *kv_ptr,
                                           struct pending_ops *p_ops, uint16_t t_id)
{
  uint32_t r_info_version =  op->ts_to_read.version;
  lock_seqlock(&kv_ptr->seqlock);
  rectify_key_epoch_id(op->epoch_id, kv_ptr, t_id);
  // find the the max ts and write it in the kvs
  if (kv_ptr->ts.version > op->ts_to_read.version)
    op->ts_to_read.version = kv_ptr->ts.version;
  memcpy(kv_ptr->value, op->value, op->val_len);
  kv_ptr->ts.m_id = op->ts_to_read.m_id;
  kv_ptr->ts.version = op->ts_to_read.version;
  unlock_seqlock(&kv_ptr->seqlock);
  if (ENABLE_ASSERTIONS) {
    assert(op->ts_to_read.m_id == machine_id);
    assert(r_info_version <= op->ts_to_read.version);
  }
  // rectifying is not needed!
  //if (r_info_version < op->ts_to_read.version)
  // rectify_version_of_w_mes(p_ops, op, r_info_version, t_id);
  // remove the write from the pending out-of-epoch writes
  p_ops->p_ooe_writes->size--;
  MOD_ADD(p_ops->p_ooe_writes->pull_ptr, PENDING_READS);
}

// Handle acquires/out-of-epoch-reads that have received a bigger version than locally stored, and need to apply the data
static inline void KVS_acquires_and_out_of_epoch_reads(struct read_info *op, mica_op_t *kv_ptr,
                                                       uint16_t t_id)
{
  lock_seqlock(&kv_ptr->seqlock);

  if (compare_ts(&kv_ptr->ts, &op->ts_to_read) == SMALLER) {
    rectify_key_epoch_id(op->epoch_id, kv_ptr, t_id);
    memcpy(kv_ptr->value, op->value, op->val_len);
    kv_ptr->ts.m_id =  op->ts_to_read.m_id;
    kv_ptr->ts.version = op->ts_to_read.version;
  }
  else if (ENABLE_STAT_COUNTING) t_stats[t_id].failed_rem_writes++;
  unlock_seqlock(&kv_ptr->seqlock);
}


// Handle committing an RMW from a response to an rmw acquire
static inline void KVS_rmw_acquire_commits(struct read_info *op, mica_op_t *kv_ptr,
                                           uint16_t op_i, uint16_t t_id)
{
  if (ENABLE_ASSERTIONS) assert(op->ts_to_read.version > 0);
  if (DEBUG_RMW)
    my_printf(green, "Worker %u is handling a remote RMW commit on op %u, "
                "rmw_l_id %u, glob_ses_id %u, log_no %u, version %u  \n",
              t_id, op_i, op->rmw_id.id, op->rmw_id.glob_sess_id,
              op->log_no, op->ts_to_read.version);
  uint64_t number_of_reqs;
  number_of_reqs = handle_remote_commit_message(kv_ptr, (void*) op, false, t_id);
  if (PRINT_LOGS) {
    fprintf(rmw_verify_fp[t_id], "Key: %u, log %u: Req %lu, Acq-RMW: rmw_id %lu, glob_sess id: %u, "
              "version %u, m_id: %u \n",
            kv_ptr->key.bkt, op->log_no, number_of_reqs,  op->rmw_id.id, op->rmw_id.glob_sess_id,
            op->ts_to_read.version, op->ts_to_read.m_id);
  }
}


/* ---------------------------------------------------------------------------
//------------------------------ KVS UTILITY SPECIFIC -----------------------------
//---------------------------------------------------------------------------*/


/* The worker sends its local requests to this, reads check the ts_tuple and copy it to the op to get broadcast
 * Writes do not get served either, writes are only propagated here to see whether their keys exist */
static inline void KVS_batch_op_trace(uint16_t op_num, uint16_t t_id, struct trace_op *op,
                                      struct kvs_resp *resp,
                                      struct pending_ops *p_ops)
{
  uint16_t op_i;
  if (ENABLE_ASSERTIONS) assert (op_num <= MAX_OP_BATCH);
  unsigned int bkt[MAX_OP_BATCH];
  struct mica_bkt *bkt_ptr[MAX_OP_BATCH];
  unsigned int tag[MAX_OP_BATCH];
  uint8_t key_in_store[MAX_OP_BATCH];	/* Is this key in the datastore? */
  mica_op_t *kv_ptr[MAX_OP_BATCH];	/* Ptr to KV item in log */
  /*
   * We first lookup the key in the datastore. The first two @I loops work
   * for both GETs and PUTs.
   */
  for(op_i = 0; op_i < op_num; op_i++) {
    KVS_locate_one_bucket(op_i, bkt, op[op_i].key, bkt_ptr, tag, kv_ptr,
                          key_in_store, KVS);
  }
  KVS_locate_all_kv_pairs(op_num, tag, bkt_ptr, kv_ptr, KVS);

  uint64_t rmw_l_id = p_ops->prop_info->l_id;
  uint32_t r_push_ptr = p_ops->r_push_ptr;
  for(op_i = 0; op_i < op_num; op_i++) {
    if (kv_ptr[op_i] == NULL) assert(false);
    if(kv_ptr[op_i] != NULL) {
      /* We had a tag match earlier. Now compare log entry. */
      bool key_found = memcmp(&kv_ptr[op_i]->key, &op[op_i].key, TRUE_KEY_SIZE) == 0;
      if(key_found) { // Hit
//        my_printf(red, "Hit %u : bkt %u/%u, server %u/%u, tag %u/%u \n",
//                   op_i, op[op_i].key.bkt, kv_ptr[op_i]->key.bkt ,op[op_i].key.server,
//                   kv_ptr[op_i]->key.server, op[op_i].key.tag, kv_ptr[op_i]->key.tag);
        key_in_store[op_i] = 1;
        if (op[op_i].opcode == KVS_OP_GET || op[op_i].opcode == OP_ACQUIRE) {
          KVS_from_trace_reads_and_acquires(&op[op_i], kv_ptr[op_i], &resp[op_i],
                                            p_ops, &r_push_ptr, t_id);
        }
          // Put has to be 2 rounds (readTS + write) if it is out-of-epoch
        else if (op[op_i].opcode == KVS_OP_PUT) {
          KVS_from_trace_writes(&op[op_i], kv_ptr[op_i], &resp[op_i],
                                p_ops, &r_push_ptr, t_id);
        }
        else if (op[op_i].opcode == OP_RELEASE) { // read the timestamp
          KVS_from_trace_releases(&op[op_i], kv_ptr[op_i], &resp[op_i],
                                  p_ops, &r_push_ptr, t_id);
        }
        else if (ENABLE_RMWS) {
          if (opcode_is_rmw(op[op_i].opcode)) {
            KVS_from_trace_rmw(&op[op_i], kv_ptr[op_i], &resp[op_i],
                               p_ops, &rmw_l_id, op_i, t_id);
          }
          else if (ENABLE_RMW_ACQUIRES && op[op_i].opcode == OP_ACQUIRE) {
            KVS_from_trace_rmw_acquire(&op[op_i], kv_ptr[op_i], &resp[op_i],
                                       p_ops, &r_push_ptr, t_id);
          }
          else if (op[op_i].opcode == KVS_OP_GET) {
            KVS_from_trace_rmw_rlxd_read(&op[op_i], kv_ptr[op_i], &resp[op_i],
                                         p_ops, &r_push_ptr, t_id);
          }
          else if (ENABLE_ASSERTIONS) {
            my_printf(red, "Wrkr %u: KVS_batch_op_trace wrong opcode in KVS: %d, req %d \n",
                      t_id, op[op_i].opcode, op_i);
            assert(0);
          }
        }
        else if (ENABLE_ASSERTIONS) assert(false);
      }
      else {
        my_printf(red, "Cache_miss %u : bkt %u/%u, server %u/%u, tag %u/%u \n",
                  op_i, op[op_i].key.bkt, kv_ptr[op_i]->key.bkt ,op[op_i].key.server,
                  kv_ptr[op_i]->key.server, op[op_i].key.tag, kv_ptr[op_i]->key.tag);
      }
    }
    if(key_in_store[op_i] == 0) {  //Cache miss --> We get here if either tag or log key match failed
//      my_printf(red, "miss\n");
//      my_printf(red, "Cache_miss %u : bkt %u/%u, server %u/%u, tag %u/%u \n",
//                op_i, op[op_i].key.bkt, kv_ptr[op_i]->key.bkt ,op[op_i].key.server,
//                kv_ptr[op_i]->key.server, op[op_i].key.tag, kv_ptr[op_i]->key.tag);
      resp[op_i].type = KVS_MISS;
    }
  }
}

/* The worker sends the remote writes to be committed with this function*/
static inline void KVS_batch_op_updates(uint16_t op_num, uint16_t t_id, struct write **writes,
                                        struct pending_ops *p_ops,
                                        uint32_t pull_ptr, uint32_t max_op_size, bool zero_ops)
{
  uint16_t op_i;	/* I is batch index */

  if (ENABLE_ASSERTIONS) assert(op_num <= MAX_INCOMING_W);
  unsigned int bkt[MAX_INCOMING_W];
  struct mica_bkt *bkt_ptr[MAX_INCOMING_W];
  unsigned int tag[MAX_INCOMING_W];
  uint8_t key_in_store[MAX_INCOMING_W];	/* Is this key in the datastore? */
  mica_op_t *kv_ptr[MAX_INCOMING_W];	/* Ptr to KV item in log */
  /*
     * We first lookup the key in the datastore. The first two @I loops work
     * for both GETs and PUTs.
     */
  for(op_i = 0; op_i < op_num; op_i++) {
    struct write *op = writes[(pull_ptr + op_i) % max_op_size];
    KVS_locate_one_bucket(op_i, bkt, op->key, bkt_ptr, tag, kv_ptr,
                          key_in_store, KVS);
  }
  KVS_locate_all_kv_pairs(op_num, tag, bkt_ptr, kv_ptr, KVS);

  // the following variables used to validate atomicity between a lock-free r_rep of an object
  for(op_i = 0; op_i < op_num; op_i++) {
    struct write *write =  writes[(pull_ptr + op_i) % max_op_size];
    if (unlikely (write->opcode == OP_RELEASE_BIT_VECTOR)) continue;
    if (kv_ptr[op_i] != NULL) {
      /* We had a tag match earlier. Now compare log entry. */
      bool key_found = memcmp(&kv_ptr[op_i]->key, &write->key, TRUE_KEY_SIZE) == 0;
      if (key_found) { //Cache Hit
        key_in_store[op_i] = 1;

        if (write->opcode == KVS_OP_PUT || write->opcode == OP_RELEASE ||
            write->opcode == OP_ACQUIRE) {
          KVS_updates_writes_or_releases_or_acquires(write, kv_ptr[op_i], t_id);
        }
        else if (ENABLE_RMWS) {
          if (write->opcode == ACCEPT_OP) {
            KVS_updates_accepts((struct accept*) write, kv_ptr[op_i], p_ops, op_i, t_id);
          }
          else if (write->opcode == COMMIT_OP) {
            KVS_updates_commits((struct commit*) write, kv_ptr[op_i], p_ops, op_i, t_id);
          }
          else if (ENABLE_ASSERTIONS) {
            my_printf(red, "Wrkr %u, kvs batch update: wrong opcode in kvs: %d, req %d, "
                        "m_id %u, val_len %u, version %u , \n",
                      t_id, write->opcode, op_i, write->m_id,
                      write->val_len, write->version);
            assert(0);
          }
        }
        else if (ENABLE_ASSERTIONS) assert(false);
      }
      if (key_in_store[op_i] == 0) {  //Cache miss --> We get here if either tag or log key match failed
        if (ENABLE_ASSERTIONS) assert(false);
      }
      if (zero_ops) {
        //printf("Zero out %d at address %lu \n", write->opcode, &write->opcode);
        write->opcode = 5;
      }
    }
  }
}

// The worker send here the incoming reads, the reads check the incoming ts if it is  bigger/equal to the local
// the just ack it, otherwise they send the value back
static inline void KVS_batch_op_reads(uint32_t op_num, uint16_t t_id, struct pending_ops *p_ops,
                                      uint32_t pull_ptr, uint32_t max_op_size, bool zero_ops)
{
  uint16_t op_i;	/* I is batch index */
  struct read **reads = (struct read **) p_ops->ptrs_to_mes_ops;

  if (ENABLE_ASSERTIONS) assert(op_num <= MAX_INCOMING_R);
  unsigned int bkt[MAX_INCOMING_R];
  struct mica_bkt *bkt_ptr[MAX_INCOMING_R];
  unsigned int tag[MAX_INCOMING_R];
  uint8_t key_in_store[MAX_INCOMING_R];	/* Is this key in the datastore? */
  mica_op_t *kv_ptr[MAX_INCOMING_R];	/* Ptr to KV item in log */
  /*
     * We first lookup the key in the datastore. The first two @I loops work
     * for both GETs and PUTs.
     */
  for(op_i = 0; op_i < op_num; op_i++) {
    struct read *read = reads[(pull_ptr + op_i) % max_op_size];
    if (unlikely(read->opcode == OP_ACQUIRE_FLIP_BIT)) continue; // This message is only meant to flip a bit and is thus a NO-OP
    KVS_locate_one_bucket(op_i, bkt, read->key , bkt_ptr, tag, kv_ptr,
                          key_in_store, KVS);
  }
  for(op_i = 0; op_i < op_num; op_i++) {
    struct read *read = reads[(pull_ptr + op_i) % max_op_size];
    if (unlikely(read->opcode == OP_ACQUIRE_FLIP_BIT)) continue;
    KVS_locate_one_kv_pair(op_i, tag, bkt_ptr, kv_ptr, KVS);
  }

  for(op_i = 0; op_i < op_num; op_i++) {
    struct read *read = reads[(pull_ptr + op_i) % max_op_size];
    if (read->opcode == OP_ACQUIRE_FLIP_BIT) {
      insert_r_rep(p_ops, p_ops->ptrs_to_mes_headers[op_i]->l_id, t_id,
                   p_ops->ptrs_to_mes_headers[op_i]->m_id,
                   p_ops->coalesce_r_rep[op_i], read->opcode);
      continue;
    }
    if(kv_ptr[op_i] != NULL) {
      /* We had a tag match earlier. Now compare log entry. */
      bool key_found = memcmp(&kv_ptr[op_i]->key, &read->key, TRUE_KEY_SIZE) == 0;
      if(key_found) { //Cache Hit
        key_in_store[op_i] = 1;

        check_state_with_allowed_flags(6, read->opcode, KVS_OP_GET, OP_ACQUIRE, OP_ACQUIRE_FP,
                                       CACHE_OP_GET_TS, PROPOSE_OP);
        if (read->opcode == KVS_OP_GET || read->opcode == OP_ACQUIRE ||
          read->opcode == OP_ACQUIRE_FP) {
          KVS_reads_gets_or_acquires_or_acquires_fp(read, kv_ptr[op_i], p_ops, op_i, t_id);
        }
        else if (read->opcode == CACHE_OP_GET_TS) {
          KVS_reads_get_TS(read, kv_ptr[op_i], p_ops, op_i, t_id);
        }
        else if (ENABLE_RMWS) {
          if (read->opcode == PROPOSE_OP) {
            KVS_reads_proposes(read, kv_ptr[op_i], p_ops, op_i, t_id);
          }
          else if (read->opcode == OP_ACQUIRE || read->opcode == OP_ACQUIRE_FP) {
            assert(ENABLE_RMW_ACQUIRES);
            KVS_reads_rmw_acquires(read, kv_ptr[op_i], p_ops, op_i, t_id);
          }
          else if (ENABLE_ASSERTIONS){
            //my_printf(red, "wrong Opcode in KVS: %d, req %d, m_id %u, val_len %u, version %u , \n",
            //           op->opcode, I, reads[(pull_ptr + I) % max_op_size]->m_id,
            //           reads[(pull_ptr + I) % max_op_size]->val_len,
            //          reads[(pull_ptr + I) % max_op_size]->version);
            assert(false);
          }
        }
        else if (ENABLE_ASSERTIONS) assert(false);
      }
    }
    if(key_in_store[op_i] == 0) {  //Cache miss --> We get here if either tag or log key match failed
      my_printf(red, "Opcode %u Cache_miss: bkt %u, server %u, tag %u \n",
                read->opcode, read->key.bkt, read->key.server, read->key.tag);
      assert(false); // cant have a miss since, it hit in the source's kvs
    }
    if (zero_ops) {
//      printf("Zero out %d at address %lu \n", op->opcode, &op->opcode);
      read->opcode = 5;
    } // TODO is this needed?
  }
}

// The  worker sends (out-of-epoch) reads that received a higher timestamp and thus have to be applied as writes
// Could also be that the first round of an out-of-epoch write received a high TS
// All out of epoch reads/writes must come in to update the epoch
static inline void KVS_batch_op_first_read_round(uint16_t op_num, uint16_t t_id, struct read_info **writes,
                                                 struct pending_ops *p_ops,
                                                 uint32_t pull_ptr, uint32_t max_op_size, bool zero_ops)
{
  uint16_t op_i;
  if (ENABLE_ASSERTIONS) assert(op_num <= MAX_INCOMING_R);
  unsigned int bkt[MAX_INCOMING_R];
  struct mica_bkt *bkt_ptr[MAX_INCOMING_R];
  unsigned int tag[MAX_INCOMING_R];
  uint8_t key_in_store[MAX_INCOMING_R];	/* Is this key in the datastore? */
  mica_op_t *kv_ptr[MAX_INCOMING_R];	/* Ptr to KV item in log */
  /*
     * We first lookup the key in the datastore. The first two @I loops work
     * for both GETs and PUTs.
     */
  for(op_i = 0; op_i < op_num; op_i++) {
    struct key *op_key = &writes[(pull_ptr + op_i) % max_op_size]->key;
    KVS_locate_one_bucket_with_key(op_i, bkt, op_key, bkt_ptr, tag, kv_ptr,
                                   key_in_store, KVS);
  }
  KVS_locate_all_kv_pairs(op_num, tag, bkt_ptr, kv_ptr, KVS);


  // the following variables used to validate atomicity between a lock-free r_rep of an object
  for(op_i = 0; op_i < op_num; op_i++) {
    struct read_info *op = writes[(pull_ptr + op_i) % max_op_size];
    if(kv_ptr[op_i] != NULL) {
      /* We had a tag match earlier. Now compare log entry. */
      bool key_found = memcmp(&kv_ptr[op_i]->key, &op->key, TRUE_KEY_SIZE) == 0;
      if(key_found) { //Cache Hit
        key_in_store[op_i] = 1;
        // The write must be performed with the max TS out of the one stored in the KV and read_info
        if (op->opcode == KVS_OP_PUT) {
          KVS_out_of_epoch_writes(op, kv_ptr[op_i], p_ops, t_id);
        } else if (op->opcode == OP_ACQUIRE ||
                   op->opcode == KVS_OP_GET) { // a read resulted on receiving a higher timestamp than expected
          KVS_acquires_and_out_of_epoch_reads(op, kv_ptr[op_i], t_id);
        } else if (op->opcode == UPDATE_EPOCH_OP_GET) {
          if (!MEASURE_SLOW_PATH && op->epoch_id > kv_ptr[op_i]->epoch_id) {
            lock_seqlock(&kv_ptr[op_i]->seqlock);
            kv_ptr[op_i]->epoch_id = op->epoch_id;
            unlock_seqlock(&kv_ptr[op_i]->seqlock);
            if (ENABLE_STAT_COUNTING) t_stats[t_id].rectified_keys++;
          }
        }
        else if (ENABLE_RMWS) {
          if (op->opcode == OP_ACQUIRE) {
            KVS_rmw_acquire_commits(op, kv_ptr[op_i], op_i, t_id);
          }
          else if (ENABLE_ASSERTIONS) assert(false);
        }
        else if (ENABLE_ASSERTIONS) assert(false);
      }
    }
    if(key_in_store[op_i] == 0) {  //Cache miss --> We get here if either tag or log key match failed
      if (ENABLE_ASSERTIONS) assert(false);
    }
    if (zero_ops) {
      // printf("Zero out %d at address %lu \n", op->opcode, &op->opcode);
      op->opcode = 5;
    }
    if (op->complete_flag) {
      if (ENABLE_ASSERTIONS) assert(&p_ops->read_info[op->r_ptr] == op);
      if (op->opcode == OP_ACQUIRE || op->opcode == KVS_OP_GET)
        memcpy(op->value_to_read, op->value, op->val_len);
      signal_completion_to_client(p_ops->r_session_id[op->r_ptr],
                                  p_ops->r_index_to_req_array[op->r_ptr], t_id);
      op->complete_flag = false;
    }
    else if (ENABLE_ASSERTIONS)
      check_state_with_allowed_flags(3, op->opcode, UPDATE_EPOCH_OP_GET,
                                     OP_ACQUIRE, OP_RELEASE);
  }

}


// Send an isolated write to the kvs-no batching
static inline void KVS_isolated_op(int t_id, struct write *write)
{
  uint32_t op_num = 1;
  int j;	/* I is batch index */

  unsigned int bkt;
  struct mica_bkt *bkt_ptr;
  unsigned int tag;
  int key_in_store;	/* Is this key in the datastore? */
  mica_op_t *kv_ptr;	/* Ptr to KV item in log */
  /*
   * We first lookup the key in the datastore. The first two @I loops work
   * for both GETs and PUTs.
   */
//  struct trace_op *op = (struct trace_op*) (((void *) write) - 3);
  //print_true_key((struct key *) write->key);
  //printf("op bkt %u\n", op->key.bkt);
  bkt = write->key.bkt & KVS->bkt_mask;
  bkt_ptr = &KVS->ht_index[bkt];
  //__builtin_prefetch(bkt_ptr, 0, 0);
  tag = write->key.tag;

  key_in_store = 0;
  kv_ptr = NULL;


  for(j = 0; j < 8; j++) {
    if(bkt_ptr->slots[j].in_use == 1 &&
       bkt_ptr->slots[j].tag == tag) {
      uint64_t log_offset = bkt_ptr->slots[j].offset &
                            KVS->log_mask;
      /*
               * We can interpret the log entry as mica_op, even though it
               * may not contain the full MICA_MAX_VALUE value.
               */
      kv_ptr = (mica_op_t *) &KVS->ht_log[log_offset];
      /* Detect if the head has wrapped around for this index entry */
      if(KVS->log_head - bkt_ptr->slots[j].offset >= KVS->log_cap) {
        kv_ptr = NULL;	/* If so, we mark it "not found" */
      }
      break;
    }
  }

  // the following variables used to validate atomicity between a lock-free r_rep of an object
  if(kv_ptr != NULL) {
    /* We had a tag match earlier. Now compare log entry. */
    bool key_found = memcmp(&kv_ptr->key, &write->key, TRUE_KEY_SIZE) == 0;
    if(key_found) { //Cache Hit
      key_in_store = 1;
      if (ENABLE_ASSERTIONS) {
        if (write->opcode != OP_RELEASE) {
          my_printf(red, "Wrkr %u: KVS_isolated_op: wrong opcode : %d, m_id %u, val_len %u, version %u , \n",
                    t_id, write->opcode,  write->m_id,
                    write->val_len, write->version);
          assert(false);
        }
      }
      //my_printf(red, "op val len %d in ptr %d, total ops %d \n", op->val_len, (pull_ptr + I) % max_op_size, op_num );
      lock_seqlock(&kv_ptr->seqlock);
      if (compare_netw_ts_with_ts( (struct network_ts_tuple *) &write->m_id,
                                   &kv_ptr->ts) == GREATER) {
        memcpy(kv_ptr->value, write->value, VALUE_SIZE);
        kv_ptr->ts.m_id = write->m_id;
        kv_ptr->ts.version = write->version;
      }
      unlock_seqlock(&kv_ptr->seqlock);
    }
  }
  if(key_in_store == 0) {  //Cache miss --> We get here if either tag or log key match failed
    if (ENABLE_ASSERTIONS) assert(false);
  }



}

#endif //KITE_KVS_UTILITY_H