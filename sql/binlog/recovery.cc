/* Copyright (c) 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/binlog/recovery.h"

#include "sql/binlog.h"                  // set_valid_pos
#include "sql/binlog/tools/iterators.h"  // binlog::tools::Iterator
#include "sql/raii/sentry.h"             // raii::Sentry<>
#include "sql/xa/xid_extract.h"          // xa::XID_extractor

binlog::Binlog_recovery::Binlog_recovery(Binlog_file_reader &binlog_file_reader)
    : m_reader{binlog_file_reader},
      m_mem_root{key_memory_binlog_recover_exec,
                 static_cast<size_t>(my_getpagesize())},
      m_map_alloc{&m_mem_root},
      m_internal_xids{&m_mem_root},
      m_external_xids{m_map_alloc} {}

my_off_t binlog::Binlog_recovery::get_valid_pos() const {
  return this->m_valid_pos;
}

bool binlog::Binlog_recovery::has_failures() const {
  return this->m_is_malformed || this->m_no_engine_recovery;
}

bool binlog::Binlog_recovery::is_binlog_malformed() const {
  return this->m_is_malformed;
}

bool binlog::Binlog_recovery::has_engine_recovery_failed() const {
  return this->m_no_engine_recovery;
}

std::string const &binlog::Binlog_recovery::get_failure_message() const {
  return this->m_failure_message;
}

binlog::Binlog_recovery &binlog::Binlog_recovery::recover(
    Gtid *binlog_max_gtid, char *engine_binlog_file,
    my_off_t *engine_binlog_pos, const std::string &cur_binlog_file,
    my_off_t *first_gtid_start_pos) {
  /*
    Flag to indicate if we have seen a gtid which is pending i.e the trx
    represented by this gtid has not yet ended
  */
  bool pending_gtid = false;
  m_current_gtid.clear();
  my_off_t first_gtid_start = 0;

  binlog::tools::Iterator it{&this->m_reader};
  it.set_copy_event_buffer();
  this->m_valid_pos = this->m_reader.position();

  for (Log_event *ev = it.begin(); ev != it.end(); ev = it.next()) {
    switch (ev->get_type_code()) {
      case binary_log::QUERY_EVENT: {
        this->process_query_event(dynamic_cast<Query_log_event &>(*ev));
        break;
      }
      case binary_log::XID_EVENT: {
        this->process_xid_event(dynamic_cast<Xid_log_event &>(*ev));
        break;
      }
      case binary_log::XA_PREPARE_LOG_EVENT: {
        this->process_xa_prepare_event(
            dynamic_cast<XA_prepare_log_event &>(*ev));
        break;
      }
      case binary_log::ANONYMOUS_GTID_LOG_EVENT:
      case binary_log::GTID_LOG_EVENT: {
        pending_gtid = true;
        auto gev = static_cast<Gtid_log_event *>(ev);
        if (gev->get_type() != ANONYMOUS_GTID)
          m_current_gtid.set(gev->get_sidno(true), gev->get_gno());
        else
          m_current_gtid.clear();
        if (first_gtid_start == 0) {
          first_gtid_start =
              ev->common_header->log_pos - ev->common_header->data_written;
          *first_gtid_start_pos = first_gtid_start;
        }
      }
      [[fallthrough]];
      default: {
        break;
      }
    }

    // Whenever the current position is at a transaction boundary, save it
    // to m_valid_pos
    if (!(ev->get_type_code() == binary_log::METADATA_EVENT && pending_gtid)) {
      if (!this->m_is_malformed && !this->m_in_transaction &&
          !is_gtid_event(ev) && !is_session_control_event(ev)) {
        this->m_valid_pos = this->m_reader.position();
        pending_gtid = false;
      }
    }

    delete ev;
    ev = nullptr;
    this->m_is_malformed = it.has_error() || this->m_is_malformed;
    if (this->m_is_malformed) break;
  }

  if (!this->m_is_malformed && total_ha_2pc > 1) {
    Xa_state_list xa_list{this->m_external_xids};
    this->m_no_engine_recovery =
        ha_recover(&this->m_internal_xids, &xa_list, binlog_max_gtid,
                   engine_binlog_file, engine_binlog_pos);
    if (this->m_no_engine_recovery) {
      this->m_failure_message.assign("Recovery failed in storage engines");
    } else {
      /*
        If trim binlog on recover option is set, then we essentially trim
        binlog to the position that the engine thinks it has committed. Note
        that if opt_trim_binlog option is set, then engine recovery (called
        through ha_recover() above) ensures that all prepared txns are rolled
        back. There are a few things which need to be kept in mind:
        1. txns never span across two binlogs, hence it is safe to recover only
           the latest binlog file.
        2. A binlog rotation ensures that the previous binlogs and engine's
           transaction logs are flushed and made durable. Hence all previous
           transactions are made durable.

        If enable_raft_plugin is set, then we skip trimming binlog. This is
        because the trim is handled inside the raft plugin when this node
        rejoins the raft ring
      */
      if (opt_trim_binlog && !enable_raft_plugin) {
        set_valid_pos(&this->m_valid_pos, cur_binlog_file, first_gtid_start,
                      engine_binlog_file, *engine_binlog_pos);
      }
    }
  }

  return (*this);
}

void binlog::Binlog_recovery::process_query_event(Query_log_event const &ev) {
  std::string query{ev.query};

  if (query == "BEGIN" || query.find("XA START") == 0)
    this->process_start();

  else if (query == "COMMIT")
    this->process_commit();

  else if (query == "ROLLBACK")
    this->process_rollback();

  else if (is_atomic_ddl_event(&ev))
    this->process_atomic_ddl(ev);

  else if (query.find("XA COMMIT") == 0)
    this->process_xa_commit(query);

  else if (query.find("XA ROLLBACK") == 0)
    this->process_xa_rollback(query);
}

void binlog::Binlog_recovery::process_xid_event(Xid_log_event const &ev) {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Xid_log_event outside the boundary of a sequence of events "
        "representing an active transaction");
    return;
  }
  this->m_in_transaction = false;
  if (!this->m_internal_xids.emplace(ev.xid, m_current_gtid).second) {
    this->m_is_malformed = true;
    this->m_failure_message.assign("Xid_log_event holds an invalid XID");
  }
}

void binlog::Binlog_recovery::process_xa_prepare_event(
    XA_prepare_log_event const &ev) {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "XA_prepare_log_event outside the boundary of a sequence of events "
        "representing an active transaction");
    return;
  }

  this->m_in_transaction = false;

  XID xid;
  xid = ev.get_xid();
  auto found = this->m_external_xids.find(xid);
  if (found != this->m_external_xids.end()) {
    assert(found->second != enum_ha_recover_xa_state::PREPARED_IN_SE);
    if (found->second == enum_ha_recover_xa_state::PREPARED_IN_TC) {
      // If it was found already, must have been committed or rolled back, it
      // can't be in prepared state
      this->m_is_malformed = true;
      this->m_failure_message.assign(
          "XA_prepare_log_event holds an invalid XID");
      return;
    }
  }

  this->m_external_xids[xid] =
      ev.is_one_phase() ? enum_ha_recover_xa_state::COMMITTED_WITH_ONEPHASE
                        : enum_ha_recover_xa_state::PREPARED_IN_TC;
}

void binlog::Binlog_recovery::process_start() {
  this->m_is_malformed = this->m_in_transaction;
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `BEGIN/XA START` inside the boundary of a "
        "sequence of events representing an active transaction");
  this->m_in_transaction = true;
}

void binlog::Binlog_recovery::process_commit() {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `COMMIT` outside the boundary of a "
        "sequence of events representing an active transaction");
  this->m_in_transaction = false;
}

void binlog::Binlog_recovery::process_rollback() {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `ROLLBACK` outside the boundary of a "
        "sequence of events representing an active transaction");
  this->m_in_transaction = false;
}

void binlog::Binlog_recovery::process_atomic_ddl(Query_log_event const &ev) {
  this->m_is_malformed = this->m_in_transaction;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Query_log event containing a DDL inside the boundary of a sequence of "
        "events representing an active transaction");
    return;
  }
  if (!this->m_internal_xids.emplace(ev.ddl_xid, m_current_gtid).second) {
    this->m_is_malformed = true;
    this->m_failure_message.assign(
        "Query_log_event containing a DDL holds an invalid XID");
  }
}

void binlog::Binlog_recovery::process_xa_commit(std::string const &query) {
  this->m_is_malformed = this->m_in_transaction;
  this->m_in_transaction = false;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Query_log_event containing `XA COMMIT` inside the boundary of a "
        "sequence of events representing a transaction not yet in prepared "
        "state");
    return;
  }
  this->add_external_xid(query, enum_ha_recover_xa_state::COMMITTED);
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `XA COMMIT` holds an invalid XID");
}

void binlog::Binlog_recovery::process_xa_rollback(std::string const &query) {
  this->m_is_malformed = this->m_in_transaction;
  this->m_in_transaction = false;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Query_log_event containing `XA ROLLBACK` inside the boundary of a "
        "sequence of events representing a transaction not yet in prepared "
        "state");
    return;
  }
  this->add_external_xid(query, enum_ha_recover_xa_state::ROLLEDBACK);
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `XA ROLLBACK` holds an invalid XID");
}

void binlog::Binlog_recovery::add_external_xid(std::string const &query,
                                               enum_ha_recover_xa_state state) {
  xa::XID_extractor tokenizer{query, 1};
  if (tokenizer.size() == 0) {
    this->m_is_malformed = true;
    return;
  }

  auto found = this->m_external_xids.find(tokenizer[0]);
  if (found != this->m_external_xids.end()) {
    assert(found->second != enum_ha_recover_xa_state::PREPARED_IN_SE);
    if (found->second != enum_ha_recover_xa_state::PREPARED_IN_TC) {
      // If it was found already, it needs to be in prepared in TC state
      this->m_is_malformed = true;
      return;
    }
  }

  this->m_external_xids[tokenizer[0]] = state;
}
