/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright 2015 Cloudius Systems
 *
 * Modified by Cloudius Systems
 */

#pragma once

#include "database.hh"
#include "query-request.hh"
#include "query-result.hh"
#include "query-result-set.hh"
#include "core/distributed.hh"
#include "db/consistency_level.hh"

namespace service {

class abstract_write_response_handler;
class abstract_read_executor;

class storage_proxy /*implements StorageProxyMBean*/ {
    struct rh_entry {
        std::unique_ptr<abstract_write_response_handler> handler;
        timer<> expire_timer;
        rh_entry(std::unique_ptr<abstract_write_response_handler>&& h, std::function<void()>&& cb);
    };

public:
    struct stats {
        uint64_t read_timeouts;
        uint64_t read_unavailables;
        uint64_t range_slice_timeouts;
        uint64_t range_slice_unavailables;
        uint64_t write_timeouts;
        uint64_t write_unavailables;
    };
    using response_id_type = uint64_t;
private:
    distributed<database>& _db;
    response_id_type _next_response_id = 0;
    std::unordered_map<response_id_type, rh_entry> _response_handlers;
    constexpr static size_t _max_hints_in_progress = 128; // origin multiplies by FBUtilities.getAvailableProcessors() but we already sharded
    size_t _total_hints_in_progress = 0;
    std::unordered_map<gms::inet_address, size_t> _hints_in_progress;
    stats _stats;
    static constexpr float CONCURRENT_SUBREQUESTS_MARGIN = 0.10;
private:
    void init_messaging_service();
    future<foreign_ptr<lw_shared_ptr<query::result>>> query_singular(lw_shared_ptr<query::read_command> cmd, std::vector<query::partition_range>&& partition_ranges, db::consistency_level cl);
    response_id_type register_response_handler(std::unique_ptr<abstract_write_response_handler>&& h);
    void remove_response_handler(response_id_type id);
    void got_response(response_id_type id, gms::inet_address from);
    future<> response_wait(response_id_type id);
    abstract_write_response_handler& get_write_response_handler(storage_proxy::response_id_type id);
    response_id_type create_write_response_handler(keyspace& ks, db::consistency_level cl, frozen_mutation&& mutation, std::unordered_set<gms::inet_address> targets, std::vector<gms::inet_address>& pending_endpoints);
    future<> send_to_live_endpoints(response_id_type response_id,  sstring local_data_center);
    template<typename Range>
    size_t hint_to_dead_endpoints(lw_shared_ptr<const frozen_mutation> m, const Range& targets);
    bool cannot_hint(gms::inet_address target);
    size_t get_hints_in_progress_for(gms::inet_address target);
    bool should_hint(gms::inet_address ep);
    bool submit_hint(lw_shared_ptr<const frozen_mutation> m, gms::inet_address target);
    std::vector<gms::inet_address> get_live_sorted_endpoints(keyspace& ks, const dht::token& token);
    ::shared_ptr<abstract_read_executor> get_read_executor(lw_shared_ptr<query::read_command> cmd, query::partition_range pr, db::consistency_level cl);
    future<foreign_ptr<lw_shared_ptr<query::result>>> query_singular_local(lw_shared_ptr<query::read_command> cmd, const query::partition_range& pr);
    future<query::result_digest> query_singular_local_digest(lw_shared_ptr<query::read_command> cmd, const query::partition_range& pr);
    future<foreign_ptr<lw_shared_ptr<query::result>>> query_partition_key_range(lw_shared_ptr<query::read_command> cmd, query::partition_range&& range, db::consistency_level cl);
    std::vector<query::partition_range> get_restricted_ranges(keyspace& ks, const schema& s, query::partition_range range);
    float estimate_result_rows_per_range(lw_shared_ptr<query::read_command> cmd, keyspace& ks);
    static std::vector<gms::inet_address> intersection(const std::vector<gms::inet_address>& l1, const std::vector<gms::inet_address>& l2);
    future<std::vector<foreign_ptr<lw_shared_ptr<query::result>>>> query_partition_key_range_concurrent(std::chrono::high_resolution_clock::time_point timeout,
            std::vector<foreign_ptr<lw_shared_ptr<query::result>>>&& results, lw_shared_ptr<query::read_command> cmd, db::consistency_level cl, std::vector<query::partition_range>::iterator&& i,
            std::vector<query::partition_range>&& ranges, int concurrency_factor);

public:
    storage_proxy(distributed<database>& db);
    ~storage_proxy();
    distributed<database>& get_db() {
        return _db;
    }

    future<> mutate_locally(const mutation& m);
    future<> mutate_locally(const frozen_mutation& m);
    future<> mutate_locally(std::vector<mutation> mutations);

    /**
    * Use this method to have these Mutations applied
    * across all replicas. This method will take care
    * of the possibility of a replica being down and hint
    * the data across to some other replica.
    *
    * @param mutations the mutations to be applied across the replicas
    * @param consistency_level the consistency level for the operation
    */
    future<> mutate(std::vector<mutation> mutations, db::consistency_level cl);

    future<> mutate_with_triggers(std::vector<mutation> mutations, db::consistency_level cl,
        bool should_mutate_atomically);

    /**
    * See mutate. Adds additional steps before and after writing a batch.
    * Before writing the batch (but after doing availability check against the FD for the row replicas):
    *      write the entire batch to a batchlog elsewhere in the cluster.
    * After: remove the batchlog entry (after writing hints for the batch rows, if necessary).
    *
    * @param mutations the Mutations to be applied across the replicas
    * @param consistency_level the consistency level for the operation
    */
    future<> mutate_atomically(std::vector<mutation> mutations, db::consistency_level cl);

    /*
     * Executes data query on the whole cluster.
     *
     * Partitions for each range will be ordered according to decorated_key ordering. Results for
     * each range from "partition_ranges" may appear in any order.
     */
    future<foreign_ptr<lw_shared_ptr<query::result>>> query(schema_ptr,
        lw_shared_ptr<query::read_command> cmd,
        std::vector<query::partition_range>&& partition_ranges,
        db::consistency_level cl);

    future<foreign_ptr<lw_shared_ptr<query::result>>> query_local(lw_shared_ptr<query::read_command> cmd, std::vector<query::partition_range>&& partition_ranges);

    future<foreign_ptr<lw_shared_ptr<reconcilable_result>>> query_mutations_locally(
        lw_shared_ptr<query::read_command> cmd, const query::partition_range&);

    future<> stop() { return make_ready_future<>(); }

    friend class abstract_read_executor;

    const stats& get_stats() const {
        return _stats;
    }
};

extern distributed<storage_proxy> _the_storage_proxy;

inline distributed<storage_proxy>& get_storage_proxy() {
    return _the_storage_proxy;
}

inline storage_proxy& get_local_storage_proxy() {
    return _the_storage_proxy.local();
}


}
