#include "Daemon.h"

#include <sys/types.h>

#include <array>
#include <fstream>
#include <iostream>
#include <list>
#include <queue>
#include <sstream>
#include <stack>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "Command.h"
#include "Database.h"
#include "DatasetBuilder.h"
#include "FeatureFlags.h"
#include "OnDiskDataset.h"
#include "QueryParser.h"
#include "Responses.h"
#include "ResultWriter.h"
#include "Utils.h"
#include "spdlog/spdlog.h"

Response execute_command(const SelectCommand &cmd, Task *task,
                         const DatabaseSnapshot *snap) {
    if (cmd.iterator_requested()) {
        DatabaseName data_filename = snap->allocate_name("iterator");
        FileResultWriter writer(data_filename.get_full_path());

        auto stats = snap->execute(cmd.get_query(), cmd.taints(),
                                   cmd.datasets(), task, &writer);

        // TODO(unknown): DbChange should use DatabaseName type instead.
        DatabaseName meta_filename =
            snap->derive_name(data_filename, "itermeta");
        OnDiskIterator::construct(meta_filename, data_filename,
                                  writer.get_file_count());
        task->change(
            DBChange(DbChangeType::NewIterator, meta_filename.get_filename()));
        return Response::select_iterator(
            meta_filename.get_id(), writer.get_file_count(), stats.counters());
    }
    InMemoryResultWriter writer;
    auto stats = snap->execute(cmd.get_query(), cmd.taints(), cmd.datasets(),
                               task, &writer);
    return Response::select(writer.get(), stats.counters());
}

Response execute_command(const IteratorPopCommand &cmd, Task *task,
                         const DatabaseSnapshot *snap) {
    std::vector<std::string> out;
    uint64_t iterator_position;
    uint64_t total_files;
    snap->read_iterator(task, cmd.get_iterator_id(), cmd.elements_to_pop(),
                        &out, &iterator_position, &total_files);

    return Response::select_from_iterator(out, iterator_position, total_files);
}

Response execute_command(const IndexFromCommand &cmd, Task *task,
                         const DatabaseSnapshot *snap) {
    const auto &path_list_fname = cmd.get_path_list_fname();

    std::vector<std::string> paths;
    std::ifstream inf(path_list_fname, std::ifstream::binary);

    if (!inf) {
        throw std::runtime_error("failed to open file");
    }

    inf.exceptions(std::ifstream::badbit);

    while (!inf.eof()) {
        std::string filename;
        std::getline(inf, filename);

        if (!filename.empty()) {
            paths.push_back(filename);
        }
    }

    if (cmd.ensure_unique()) {
        snap->recursive_index_paths(task, cmd.get_index_types(), cmd.taints(),
                                    paths);
    } else {
        snap->force_recursive_index_paths(task, cmd.get_index_types(),
                                          cmd.taints(), paths);
    }

    return Response::ok();
}

Response execute_command(const IndexCommand &cmd, Task *task,
                         const DatabaseSnapshot *snap) {
    if (cmd.ensure_unique()) {
        snap->recursive_index_paths(task, cmd.get_index_types(), cmd.taints(),
                                    cmd.get_paths());
    } else {
        snap->force_recursive_index_paths(task, cmd.get_index_types(),
                                          cmd.taints(),

                                          cmd.get_paths());
    }

    return Response::ok();
}

Response execute_command(const ConfigGetCommand &cmd,
                         [[maybe_unused]] Task *task,
                         const DatabaseSnapshot *snap) {
    if (cmd.keys().empty()) {
        return Response::config(snap->get_config().get_all());
    }
    std::unordered_map<std::string, uint64_t> vals;
    for (const auto &keyname : cmd.keys()) {
        auto key = ConfigKey::parse(keyname);
        if (key) {
            vals[keyname] = snap->get_config().get(*key);
        }
    }
    return Response::config(vals);
}

Response execute_command(const ConfigSetCommand &cmd, Task *task,
                         [[maybe_unused]] const DatabaseSnapshot *snap) {
    auto key = ConfigKey::parse(cmd.key());
    if (!key) {
        return Response::error("Invalid key name specified");
    }
    if (!snap->get_config().can_set(*key, cmd.value())) {
        return Response::error("Value specified is out of range");
    }
    task->change(DBChange(DbChangeType::ConfigChange, cmd.key(),
                          std::to_string(cmd.value())));
    return Response::ok();
}

Response execute_command(const ReindexCommand &cmd, Task *task,
                         const DatabaseSnapshot *snap) {
    const std::string &dataset_id = cmd.dataset_id();
    snap->reindex_dataset(task, cmd.get_index_types(), dataset_id);

    return Response::ok();
}

Response execute_command([[maybe_unused]] const CompactCommand &cmd, Task *task,
                         const DatabaseSnapshot *snap) {
    snap->compact_locked_datasets(task);
    return Response::ok();
}

Response execute_command([[maybe_unused]] const StatusCommand &cmd,
                         [[maybe_unused]] Task *task,
                         const DatabaseSnapshot *snap) {
    return Response::status(snap->get_tasks());
}

Response execute_command([[maybe_unused]] const TopologyCommand &cmd,
                         [[maybe_unused]] Task *task,
                         const DatabaseSnapshot *snap) {
    std::stringstream ss;
    const std::vector<const OnDiskDataset *> &datasets = snap->get_datasets();

    std::vector<DatasetEntry> result;
    for (const auto *dataset : datasets) {
        DatasetEntry dataset_entry{/*.id:*/ dataset->get_id(),
                                   /*.size:*/ 0,
                                   /*.file_count:*/ dataset->get_file_count(),
                                   /*.taints:*/ dataset->get_taints(),
                                   /*.indexes:*/ {}};

        for (const auto &index : dataset->get_indexes()) {
            IndexEntry index_entry{/*.type:*/ index.index_type(),
                                   /*.size:*/ index.real_size()};
            dataset_entry.indexes.push_back(index_entry);
            dataset_entry.size += index_entry.size;
        }

        result.push_back(dataset_entry);
    }

    return Response::topology(result);
}

Response execute_command([[maybe_unused]] const PingCommand &cmd, Task *task,
                         [[maybe_unused]] const DatabaseSnapshot *snap) {
    return Response::ping(task->spec().hex_conn_id());
}

Response execute_command(const TaintCommand &cmd, Task *task,
                         const DatabaseSnapshot *snap) {
    const OnDiskDataset *ds = snap->find_dataset(cmd.get_dataset());
    if (ds == nullptr) {
        throw std::runtime_error("can't taint non-existent dataset");
    }
    const std::string &taint = cmd.get_taint();
    bool has_taint = ds->get_taints().count(taint) > 0;
    bool should_have_taint = cmd.get_mode() == TaintMode::Add;

    if (has_taint != should_have_taint) {
        task->change(
            DBChange(DbChangeType::ToggleTaint, cmd.get_dataset(), taint));
    }

    return Response::ok();
}

Response execute_command(const DatasetDropCommand &cmd, Task *task,
                         [[maybe_unused]] const DatabaseSnapshot *snap) {
    task->change(DBChange(DbChangeType::Drop, cmd.dataset_id()));

    return Response::ok();
}

Response dispatch_command(const Command &cmd, Task *task,
                          const DatabaseSnapshot *snap) {
    return std::visit(
        [snap, task](const auto &cmd) {
            return execute_command(cmd, task, snap);
        },
        cmd);
}

Response dispatch_command_safe(const std::string &cmd_str, Task *task,
                               const DatabaseSnapshot *snap) {
    try {
        Command cmd = parse_command(cmd_str);
        return dispatch_command(cmd, task, snap);
    } catch (const std::runtime_error &e) {
        spdlog::error("Task {} failed: {}", task->spec().id(), e.what());
        return Response::error(e.what());
    } catch (const std::bad_alloc &e) {
        spdlog::error("Task {} failed: out of memory!", task->spec().id());
        return Response::error("out of memory");
    }
}

std::vector<DatabaseLock> acquire_locks(
    const IteratorPopCommand &cmd,
    [[maybe_unused]] const DatabaseSnapshot *snap) {
    return {IteratorLock(cmd.get_iterator_id())};
}

std::vector<DatabaseLock> acquire_locks(
    const ReindexCommand &cmd, [[maybe_unused]] const DatabaseSnapshot *snap) {
    return {DatasetLock(cmd.dataset_id())};
}

std::vector<DatabaseLock> acquire_locks(const CompactCommand &cmd,
                                        const DatabaseSnapshot *snap) {
    std::vector<std::string> to_lock;
    if (cmd.get_type() == CompactType::Smart) {
        to_lock = snap->compact_smart_candidates();
    } else {
        to_lock = snap->compact_full_candidates();
    }

    std::vector<DatabaseLock> locks;
    locks.reserve(to_lock.size());
    for (const auto &dsid : to_lock) {
        locks.emplace_back(DatasetLock(dsid));
    }

    return locks;
}

std::vector<DatabaseLock> acquire_locks(
    const TaintCommand &cmd, [[maybe_unused]] const DatabaseSnapshot *snap) {
    return {DatasetLock(cmd.get_dataset())};
}

template <typename T>
std::vector<DatabaseLock> acquire_locks(
    [[maybe_unused]] const T &_anything,
    [[maybe_unused]] const DatabaseSnapshot *snap) {
    return {};
}

std::vector<DatabaseLock> dispatch_locks(const Command &cmd,
                                         const DatabaseSnapshot *snap) {
    return std::move(std::visit(
        [snap](const auto &cmd) { return acquire_locks(cmd, snap); }, cmd));
}
