#include <Backups/RestorerFromBackup.h>
#include <Backups/IRestoreCoordination.h>
#include <Backups/BackupCoordinationStage.h>
#include <Backups/BackupSettings.h>
#include <Backups/IBackup.h>
#include <Backups/IBackupEntry.h>
#include <Backups/BackupUtils.h>
#include <Backups/DDLAdjustingForBackupVisitor.h>
#include <Access/AccessBackup.h>
#include <Access/AccessRights.h>
#include <Access/ContextAccess.h>
#include <Parsers/ParserCreateQuery.h>
#include <Parsers/parseQuery.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/Context.h>
#include <Interpreters/InterpreterCreateQuery.h>
#include <Interpreters/ProcessList.h>
#include <Databases/IDatabase.h>
#include <Databases/DDLDependencyVisitor.h>
#include <Storages/IStorage.h>
#include <Common/ZooKeeper/ZooKeeperRetries.h>
#include <Common/quoteString.h>
#include <Common/escapeForFileName.h>
#include <Common/threadPoolCallbackRunner.h>
#include <base/insertAtEnd.h>
#include <Core/Settings.h>

#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/map.hpp>

#include <filesystem>
#include <future>
#include <ranges>

#include <boost/algorithm/string.hpp>

#if CLICKHOUSE_CLOUD
#include <Interpreters/SharedDatabaseCatalog.h>
#endif

namespace fs = std::filesystem;


namespace DB
{
namespace Setting
{
    extern const SettingsUInt64 backup_restore_keeper_retry_initial_backoff_ms;
    extern const SettingsUInt64 backup_restore_keeper_retry_max_backoff_ms;
    extern const SettingsUInt64 backup_restore_keeper_max_retries;
    extern const SettingsSeconds lock_acquire_timeout;
    extern const SettingsBool restore_replicated_merge_tree_to_shared_merge_tree;
}

namespace ErrorCodes
{
    extern const int BACKUP_ENTRY_NOT_FOUND;
    extern const int CANNOT_RESTORE_TABLE;
    extern const int CANNOT_RESTORE_DATABASE;
    extern const int LOGICAL_ERROR;
}


namespace Stage = BackupCoordinationStage;

namespace
{
    /// Outputs "table <name>" or "temporary table <name>"
    String tableNameWithTypeToString(const String & database_name, const String & table_name, bool first_upper)
    {
        String str;
        if (database_name == DatabaseCatalog::TEMPORARY_DATABASE)
            str = fmt::format("temporary table {}", backQuoteIfNeed(table_name));
        else
            str = fmt::format("table {}.{}", backQuoteIfNeed(database_name), backQuoteIfNeed(table_name));
        if (first_upper)
            str[0] = std::toupper(str[0]);
        return str;
    }

    /// Whether a specified name corresponds one of the tables backuping ACL.
    bool isSystemAccessTableName(const QualifiedTableName & table_name)
    {
        if (table_name.database != DatabaseCatalog::SYSTEM_DATABASE)
            return false;

        return (table_name.table == "users") || (table_name.table == "roles") || (table_name.table == "settings_profiles")
            || (table_name.table == "row_policies") || (table_name.table == "quotas");
    }

    /// Whether a specified name corresponds one of the tables backuping ACL.
    bool isSystemFunctionsTableName(const QualifiedTableName & table_name)
    {
        return (table_name.database == DatabaseCatalog::SYSTEM_DATABASE) && (table_name.table == "functions");
    }
 }


RestorerFromBackup::RestorerFromBackup(
    const ASTBackupQuery::Elements & restore_query_elements_,
    const RestoreSettings & restore_settings_,
    std::shared_ptr<IRestoreCoordination> restore_coordination_,
    const BackupPtr & backup_,
    const ContextMutablePtr & context_,
    const ContextPtr & query_context_,
    ThreadPool & thread_pool_,
    const std::function<void()> & after_task_callback_)
    : restore_query_elements(restore_query_elements_)
    , restore_settings(restore_settings_)
    , restore_coordination(restore_coordination_)
    , backup(backup_)
    , context(context_)
    , query_context(query_context_)
    , process_list_element(context->getProcessListElement())
    , after_task_callback(after_task_callback_)
    , create_table_timeout(context->getConfigRef().getUInt64("backups.create_table_timeout", 300000))
    , log(getLogger("RestorerFromBackup"))
    , zookeeper_retries_info(
          context->getSettingsRef()[Setting::backup_restore_keeper_max_retries],
          context->getSettingsRef()[Setting::backup_restore_keeper_retry_initial_backoff_ms],
          context->getSettingsRef()[Setting::backup_restore_keeper_retry_max_backoff_ms],
          context->getProcessListElementSafe())
    , tables_dependencies("RestorerFromBackup")
    , thread_pool(thread_pool_)
{
}

RestorerFromBackup::~RestorerFromBackup()
{
    /// If an exception occurs we can come here to the destructor having some tasks still unfinished.
    /// We have to wait until they finish.
    if (getNumFutures() > 0)
    {
        LOG_INFO(log, "Waiting for {} tasks to finish", getNumFutures());
        waitFutures(/* throw_if_error= */ false);
    }
}

void RestorerFromBackup::run(Mode mode_)
{
    /// run() can be called onle once.
    if (!current_stage.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Already restoring");

    mode = mode_;

    /// Find other hosts working along with us to execute this ON CLUSTER query.
    all_hosts = BackupSettings::Util::filterHostIDs(
        restore_settings.cluster_host_ids, restore_settings.shard_num, restore_settings.replica_num);

    /// Do renaming in the create queries according to the renaming config.
    renaming_map = BackupUtils::makeRenamingMap(restore_query_elements);

    /// Calculate the root path in the backup for restoring, it's either empty or has the format "shards/<shard_num>/replicas/<replica_num>/".
    findRootPathsInBackup();

    /// Find all the databases and tables which we will read from the backup.
    setStage(Stage::FINDING_TABLES_IN_BACKUP);
    findDatabasesAndTablesInBackup();
    logNumberOfDatabasesAndTablesToRestore();

    /// Check access rights.
    setStage(Stage::CHECKING_ACCESS_RIGHTS);
    loadSystemAccessTables();
    checkAccessForObjectsFoundInBackup();

    if (mode == Mode::CHECK_ACCESS_ONLY)
        return;

    /// Create databases using the create queries read from the backup.
    setStage(Stage::CREATING_DATABASES);
    createAndCheckDatabases();

    /// Create tables using the create queries read from the backup.
    setStage(Stage::CREATING_TABLES);
    removeUnresolvedDependencies();
    createAndCheckTables();

    /// All what's left is to insert data to tables.
    setStage(Stage::INSERTING_DATA_TO_TABLES);
    insertDataToTables();
    runDataRestoreTasks();

    setStage(Stage::FINALIZING_TABLES);
    finalizeTables();

    /// Restored successfully!
    setStage(Stage::COMPLETED);
}

void RestorerFromBackup::waitFutures(bool throw_if_error)
{
    std::exception_ptr error;

    for (;;)
    {
        std::vector<std::future<void>> futures_to_wait;
        {
            std::lock_guard lock{mutex};
            std::swap(futures_to_wait, futures);
        }

        if (futures_to_wait.empty())
            break;

        /// Wait for all tasks to finish.
        for (auto & future : futures_to_wait)
        {
            try
            {
                future.get();
            }
            catch (...)
            {
                if (!error)
                    error = std::current_exception();
                exception_caught = true;
            }
        }
    }

    if (error)
    {
        if (throw_if_error)
            std::rethrow_exception(error);
        else
            tryLogException(error, log);
    }
}

size_t RestorerFromBackup::getNumFutures() const
{
    std::lock_guard lock{mutex};
    return futures.size();
}

void RestorerFromBackup::setStage(const String & new_stage, const String & message)
{
    LOG_TRACE(log, "Setting stage: {}", new_stage);

    if (getNumFutures() != 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Cannot change the stage while some tasks ({}) are still running", getNumFutures());

    checkIsQueryCancelled();

    current_stage = new_stage;

    if (restore_coordination)
    {
        /// There is no need to sync Stage::COMPLETED with other hosts because it's the last stage.
        restore_coordination->setStage(new_stage, message, /* sync = */ (new_stage != Stage::COMPLETED));
    }
}

void RestorerFromBackup::schedule(std::function<void()> && task_, const char * thread_name_)
{
    if (exception_caught)
        return;

    checkIsQueryCancelled();

    auto future = scheduleFromThreadPoolUnsafe<void>(
        [this, task = std::move(task_)]() mutable
        {
            if (exception_caught)
                return;
            checkIsQueryCancelled();

            std::move(task)();

            if (after_task_callback)
                after_task_callback();
        },
        thread_pool,
        thread_name_);

    std::lock_guard lock{mutex};
    futures.push_back(std::move(future));
}

void RestorerFromBackup::checkIsQueryCancelled() const
{
    if (process_list_element)
        process_list_element->checkTimeLimit();
}

void RestorerFromBackup::findRootPathsInBackup()
{
    size_t shard_num = 1;
    size_t replica_num = 1;
    if (!restore_settings.host_id.empty())
    {
        std::tie(shard_num, replica_num)
            = BackupSettings::Util::findShardNumAndReplicaNum(restore_settings.cluster_host_ids, restore_settings.host_id);
    }

    root_paths_in_backup.clear();

    /// Start with "" as the root path and then we will add shard- and replica-related part to it.
    fs::path root_path = "/";
    root_paths_in_backup.push_back(root_path);

    /// Add shard-related part to the root path.
    Strings shards_in_backup = backup->listFiles(root_path / "shards", /*recursive*/ false);
    if (shards_in_backup.empty())
    {
        if (restore_settings.shard_num_in_backup > 1)
            throw Exception(ErrorCodes::BACKUP_ENTRY_NOT_FOUND, "No shard #{} in backup", restore_settings.shard_num_in_backup);
    }
    else
    {
        String shard_name;
        if (restore_settings.shard_num_in_backup)
            shard_name = std::to_string(restore_settings.shard_num_in_backup);
        else if (shards_in_backup.size() == 1)
            shard_name = shards_in_backup.front();
        else
            shard_name = std::to_string(shard_num);
        if (std::find(shards_in_backup.begin(), shards_in_backup.end(), shard_name) == shards_in_backup.end())
            throw Exception(ErrorCodes::BACKUP_ENTRY_NOT_FOUND, "No shard #{} in backup", shard_name);
        root_path = root_path / "shards" / shard_name;
        root_paths_in_backup.push_back(root_path);
    }

    /// Add replica-related part to the root path.
    Strings replicas_in_backup = backup->listFiles(root_path / "replicas", /*recursive*/ false);
    if (replicas_in_backup.empty())
    {
        if (restore_settings.replica_num_in_backup > 1)
            throw Exception(ErrorCodes::BACKUP_ENTRY_NOT_FOUND, "No replica #{} in backup", restore_settings.replica_num_in_backup);
    }
    else
    {
        String replica_name;
        if (restore_settings.replica_num_in_backup)
        {
            replica_name = std::to_string(restore_settings.replica_num_in_backup);
            if (std::find(replicas_in_backup.begin(), replicas_in_backup.end(), replica_name) == replicas_in_backup.end())
                throw Exception(ErrorCodes::BACKUP_ENTRY_NOT_FOUND, "No replica #{} in backup", replica_name);
        }
        else
        {
            replica_name = std::to_string(replica_num);
            if (std::find(replicas_in_backup.begin(), replicas_in_backup.end(), replica_name) == replicas_in_backup.end())
                replica_name = replicas_in_backup.front();
        }
        root_path = root_path / "replicas" / replica_name;
        root_paths_in_backup.push_back(root_path);
    }

    /// Revert the list of root paths, because we need it in the following order:
    /// "/shards/<shard_num>/replicas/<replica_num>/" (first we search tables here)
    /// "/shards/<shard_num>/" (then here)
    /// "/" (and finally here)
    std::reverse(root_paths_in_backup.begin(), root_paths_in_backup.end());

    LOG_TRACE(
        log,
        "Will use paths in backup: {}",
        boost::algorithm::join(
            root_paths_in_backup
                | boost::adaptors::transformed([](const fs::path & path) -> String { return doubleQuoteString(String{path}); }),
            ", "));
}

void RestorerFromBackup::findDatabasesAndTablesInBackup()
{
    for (const auto & element : restore_query_elements)
    {
        switch (element.type)
        {
            case ASTBackupQuery::ElementType::TABLE:
            {
                findTableInBackup({element.database_name, element.table_name}, /* skip_if_inner_table= */ false, element.partitions);
                break;
            }
            case ASTBackupQuery::ElementType::TEMPORARY_TABLE:
            {
                findTableInBackup({DatabaseCatalog::TEMPORARY_DATABASE, element.table_name}, /* skip_if_inner_table= */ false, element.partitions);
                break;
            }
            case ASTBackupQuery::ElementType::DATABASE:
            {
                findDatabaseInBackup(element.database_name, element.except_tables);
                break;
            }
            case ASTBackupQuery::ElementType::ALL:
            {
                findEverythingInBackup(element.except_databases, element.except_tables);
                break;
            }
        }
    }
    waitFutures();
}

void RestorerFromBackup::logNumberOfDatabasesAndTablesToRestore() const
{
    std::string_view action = (mode == CHECK_ACCESS_ONLY) ? "check access rights for restoring" : "restore";
    LOG_INFO(log, "Will {} {} databases and {} tables", action, getNumDatabases(), getNumTables());
}

void RestorerFromBackup::findTableInBackup(const QualifiedTableName & table_name_in_backup, bool skip_if_inner_table, const std::optional<ASTs> & partitions)
{
    schedule(
        [this, table_name_in_backup, skip_if_inner_table, partitions]() { findTableInBackupImpl(table_name_in_backup, skip_if_inner_table, partitions); },
        "Restore_FindTbl");
}

void RestorerFromBackup::findTableInBackupImpl(const QualifiedTableName & table_name_in_backup, bool skip_if_inner_table, const std::optional<ASTs> & partitions)
{
    bool is_temporary_table = (table_name_in_backup.database == DatabaseCatalog::TEMPORARY_DATABASE);

    std::optional<fs::path> metadata_path;
    std::optional<fs::path> root_path_in_use;
    for (const auto & root_path_in_backup : root_paths_in_backup)
    {
        fs::path try_metadata_path;
        if (is_temporary_table)
        {
            try_metadata_path
                = root_path_in_backup / "temporary_tables" / "metadata" / (escapeForFileName(table_name_in_backup.table) + ".sql");
        }
        else
        {
            try_metadata_path = root_path_in_backup / "metadata" / escapeForFileName(table_name_in_backup.database)
                / (escapeForFileName(table_name_in_backup.table) + ".sql");
        }

        if (backup->fileExists(try_metadata_path))
        {
            metadata_path = try_metadata_path;
            root_path_in_use = root_path_in_backup;
            break;
        }
    }

    if (!metadata_path)
        throw Exception(
            ErrorCodes::BACKUP_ENTRY_NOT_FOUND,
            "{} not found in backup",
            tableNameWithTypeToString(table_name_in_backup.database, table_name_in_backup.table, true));

    fs::path data_path_in_backup;
    if (is_temporary_table)
    {
        data_path_in_backup = *root_path_in_use / "temporary_tables" / "data" / escapeForFileName(table_name_in_backup.table);
    }
    else
    {
        data_path_in_backup
            = *root_path_in_use / "data" / escapeForFileName(table_name_in_backup.database) / escapeForFileName(table_name_in_backup.table);
    }

    QualifiedTableName table_name = renaming_map.getNewTableName(table_name_in_backup);
    if (skip_if_inner_table && BackupUtils::isInnerTable(table_name))
        return;

    auto read_buffer = backup->readFile(*metadata_path);
    String create_query_str;
    readStringUntilEOF(create_query_str, *read_buffer);
    read_buffer.reset();
    ParserCreateQuery create_parser;
    ASTPtr create_table_query = parseQuery(create_parser, create_query_str, 0, DBMS_DEFAULT_MAX_PARSER_DEPTH, DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);
    applyCustomStoragePolicy(create_table_query);
    renameDatabaseAndTableNameInCreateQuery(create_table_query, renaming_map, context->getGlobalContext());
    String create_table_query_str = create_table_query->formatWithSecretsOneLine();

    bool is_predefined_table = DatabaseCatalog::instance().isPredefinedTable(StorageID{table_name.database, table_name.table});
    auto table_dependencies = getDependenciesFromCreateQuery(context, table_name, create_table_query, context->getCurrentDatabase());
    bool table_has_data = backup->hasFiles(data_path_in_backup);

    std::lock_guard lock{mutex};

    if (auto it = table_infos.find(table_name); it != table_infos.end())
    {
        const TableInfo & table_info = it->second;
        if (table_info.create_table_query && (table_info.create_table_query_str != create_table_query_str))
        {
            throw Exception(
                ErrorCodes::CANNOT_RESTORE_TABLE,
                "Extracted two different create queries for the same {}: {} and {}",
                tableNameWithTypeToString(table_name.database, table_name.table, false),
                table_info.create_table_query_str,
                create_table_query_str);
        }
    }

    TableInfo & res_table_info = table_infos[table_name];
    res_table_info.create_table_query = create_table_query;
    res_table_info.create_table_query_str = create_table_query_str;
    res_table_info.is_predefined_table = is_predefined_table;
    res_table_info.has_data = table_has_data;
    res_table_info.data_path_in_backup = data_path_in_backup;

    tables_dependencies.addDependencies(table_name, table_dependencies.dependencies);

    if (partitions)
    {
        if (!res_table_info.partitions)
            res_table_info.partitions.emplace();
        insertAtEnd(*res_table_info.partitions, *partitions);
    }
}

void RestorerFromBackup::findDatabaseInBackup(const String & database_name_in_backup, const std::set<DatabaseAndTableName> & except_table_names)
{
    schedule(
        [this, database_name_in_backup, except_table_names]() { findDatabaseInBackupImpl(database_name_in_backup, except_table_names); },
        "Restore_FindDB");
}

void RestorerFromBackup::findDatabaseInBackupImpl(const String & database_name_in_backup, const std::set<DatabaseAndTableName> & except_table_names)
{
    std::optional<fs::path> metadata_path;
    std::unordered_set<String> table_names_in_backup;
    for (const auto & root_path_in_backup : root_paths_in_backup)
    {
        fs::path try_metadata_path;
        fs::path try_tables_metadata_path;
        if (database_name_in_backup == DatabaseCatalog::TEMPORARY_DATABASE)
        {
            try_tables_metadata_path = root_path_in_backup / "temporary_tables" / "metadata";
        }
        else
        {
            try_metadata_path = root_path_in_backup / "metadata" / (escapeForFileName(database_name_in_backup) + ".sql");
            try_tables_metadata_path = root_path_in_backup / "metadata" / escapeForFileName(database_name_in_backup);
        }

        if (!metadata_path && !try_metadata_path.empty() && backup->fileExists(try_metadata_path))
            metadata_path = try_metadata_path;

        Strings file_names = backup->listFiles(try_tables_metadata_path, /*recursive*/ false);
        for (const String & file_name : file_names)
        {
            if (!file_name.ends_with(".sql"))
                continue;
            String file_name_without_ext = file_name.substr(0, file_name.length() - strlen(".sql"));
            table_names_in_backup.insert(unescapeForFileName(file_name_without_ext));
        }
    }

    if (!metadata_path && table_names_in_backup.empty())
        throw Exception(ErrorCodes::BACKUP_ENTRY_NOT_FOUND, "Database {} not found in backup", backQuoteIfNeed(database_name_in_backup));

    if (metadata_path)
    {
        auto read_buffer = backup->readFile(*metadata_path);
        String create_query_str;
        readStringUntilEOF(create_query_str, *read_buffer);
        read_buffer.reset();
        ParserCreateQuery create_parser;
        ASTPtr create_database_query = parseQuery(create_parser, create_query_str, 0, DBMS_DEFAULT_MAX_PARSER_DEPTH, DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);
        renameDatabaseAndTableNameInCreateQuery(create_database_query, renaming_map, context->getGlobalContext());
        String create_database_query_str = create_database_query->formatWithSecretsOneLine();

        String database_name = renaming_map.getNewDatabaseName(database_name_in_backup);
        bool is_predefined_database = DatabaseCatalog::isPredefinedDatabase(database_name);

        std::lock_guard lock{mutex};

        DatabaseInfo & database_info = database_infos[database_name];

        if (database_info.create_database_query && (database_info.create_database_query_str != create_database_query_str))
        {
            throw Exception(
                ErrorCodes::CANNOT_RESTORE_DATABASE,
                "Extracted two different create queries for the same database {}: {} and {}",
                backQuoteIfNeed(database_name),
                database_info.create_database_query_str,
                create_database_query_str);
        }

        database_info.create_database_query = create_database_query;
        database_info.create_database_query_str = create_database_query_str;
        database_info.is_predefined_database = is_predefined_database;
    }

    for (const String & table_name_in_backup : table_names_in_backup)
    {
        if (except_table_names.contains({database_name_in_backup, table_name_in_backup}))
            continue;

        findTableInBackup({database_name_in_backup, table_name_in_backup}, /* skip_if_inner_table= */ true, /* partitions= */ {});
    }
}

void RestorerFromBackup::findEverythingInBackup(const std::set<String> & except_database_names, const std::set<DatabaseAndTableName> & except_table_names)
{
    std::unordered_set<String> database_names_in_backup;

    for (const auto & root_path_in_backup : root_paths_in_backup)
    {
        Strings file_names = backup->listFiles(root_path_in_backup / "metadata", /*recursive*/ false);
        for (String & file_name : file_names)
        {
            if (file_name.ends_with(".sql"))
                file_name.resize(file_name.length() - strlen(".sql"));
            database_names_in_backup.emplace(unescapeForFileName(file_name));
        }

        if (backup->hasFiles(root_path_in_backup / "temporary_tables" / "metadata"))
            database_names_in_backup.emplace(DatabaseCatalog::TEMPORARY_DATABASE);
    }

    for (const String & database_name_in_backup : database_names_in_backup)
    {
        if (except_database_names.contains(database_name_in_backup))
            continue;

        findDatabaseInBackup(database_name_in_backup, except_table_names);
    }
}

size_t RestorerFromBackup::getNumDatabases() const
{
    std::lock_guard lock{mutex};
    return database_infos.size();
}

size_t RestorerFromBackup::getNumTables() const
{
    std::lock_guard lock{mutex};
    return table_infos.size();
}

void RestorerFromBackup::loadSystemAccessTables()
{
    if (restore_settings.structure_only)
        return;

    /// Special handling for ACL-related system tables.
    std::lock_guard lock{mutex};
    for (const auto & [table_name, table_info] : table_infos)
    {
        if (isSystemAccessTableName(table_name))
        {
            if (!access_restorer)
                access_restorer = std::make_unique<AccessRestorerFromBackup>(backup, restore_settings);
            access_restorer->addDataPath(table_info.data_path_in_backup);
        }
    }

    if (access_restorer)
        access_restorer->loadFromBackup();
}

void RestorerFromBackup::checkAccessForObjectsFoundInBackup() const
{
    AccessRightsElements required_access;

    {
        std::lock_guard lock{mutex};
        for (const auto & [database_name, database_info] : database_infos)
        {
            if (database_info.is_predefined_database)
                continue;

            AccessFlags flags;

            if (restore_settings.create_database != RestoreDatabaseCreationMode::kMustExist)
                flags |= AccessType::CREATE_DATABASE;

            if (!flags)
                flags = AccessType::SHOW_DATABASES;

            required_access.emplace_back(flags, database_name);
        }

        for (const auto & [table_name, table_info] : table_infos)
        {
            if (table_info.is_predefined_table)
            {
                if (isSystemFunctionsTableName(table_name))
                {
                    /// CREATE_FUNCTION privilege is required to restore the "system.functions" table.
                    if (!restore_settings.structure_only && table_info.has_data)
                        required_access.emplace_back(AccessType::CREATE_FUNCTION);
                }
                /// Privileges required to restore ACL system tables are checked separately
                /// (see access_restore_task->getRequiredAccess() below).
                continue;
            }

            if (table_name.database == DatabaseCatalog::TEMPORARY_DATABASE)
            {
                if (restore_settings.create_table != RestoreTableCreationMode::kMustExist)
                    required_access.emplace_back(AccessType::CREATE_TEMPORARY_TABLE);
                continue;
            }

            AccessFlags flags;
            const ASTCreateQuery & create = table_info.create_table_query->as<const ASTCreateQuery &>();

            if (restore_settings.create_table != RestoreTableCreationMode::kMustExist)
            {
                if (create.is_dictionary)
                    flags |= AccessType::CREATE_DICTIONARY;
                else if (create.is_ordinary_view || create.is_materialized_view || create.is_live_view)
                    flags |= AccessType::CREATE_VIEW;
                else
                    flags |= AccessType::CREATE_TABLE;
            }

            if (!restore_settings.structure_only && table_info.has_data)
            {
                flags |= AccessType::INSERT;
            }

            if (!flags)
            {
                if (create.is_dictionary)
                    flags = AccessType::SHOW_DICTIONARIES;
                else
                    flags = AccessType::SHOW_TABLES;
            }

            required_access.emplace_back(flags, table_name.database, table_name.table);
        }

        if (access_restorer)
            insertAtEnd(required_access, access_restorer->getRequiredAccess());
    }

    /// We convert to AccessRights to check if we have the rights contained in our current access.
    /// This handles the situation when restoring partial revoked grants:
    /// GRANT SELECT ON *.*
    /// REVOKE SELECT FROM systems.*
    const auto & required_access_rights = AccessRights{required_access};
    const auto & current_user_access_rights = context->getAccess()->getAccessRightsWithImplicit();
    if (current_user_access_rights->contains(required_access_rights))
        return;

    query_context->checkAccess(required_access_rights.getElements());
}

AccessEntitiesToRestore RestorerFromBackup::getAccessEntitiesToRestore(const String & data_path_in_backup) const
{
    std::lock_guard lock{mutex};
    if (!access_restorer)
        return {};
    access_restorer->generateRandomIDsAndResolveDependencies(context->getAccessControl());
    return access_restorer->getEntitiesToRestore(data_path_in_backup);
}

void RestorerFromBackup::createAndCheckDatabases()
{
    Strings database_names;
    {
        std::lock_guard lock{mutex};
        database_names.reserve(database_infos.size());
        std::ranges::copy(database_infos | boost::adaptors::map_keys, std::back_inserter(database_names));
    }

    for (const auto & database_name : database_names)
        createAndCheckDatabase(database_name);

    waitFutures();
}

void RestorerFromBackup::createAndCheckDatabase(const String & database_name)
{
    schedule(
        [this, database_name]() { createAndCheckDatabaseImpl(database_name); },
        "Restore_MakeDB");
}

void RestorerFromBackup::createAndCheckDatabaseImpl(const String & database_name)
{
    checkIsQueryCancelled();
    createDatabase(database_name);
    checkDatabase(database_name);
}

void RestorerFromBackup::createDatabase(const String & database_name) const
{
    try
    {
        if (restore_settings.create_database == RestoreDatabaseCreationMode::kMustExist)
            return;

        std::shared_ptr<ASTCreateQuery> create_database_query;
        {
            std::lock_guard lock{mutex};
            const auto & database_info = database_infos.at(database_name);

            /// Predefined databases always exist, we don't need to create them while restoring.
            if (database_info.is_predefined_database)
                return;

            create_database_query = typeid_cast<std::shared_ptr<ASTCreateQuery>>(database_info.create_database_query->clone());
        }

        /// Generate a new UUID for a database.
        /// The generated UUID will be ignored if the database does not support UUIDs.
        restore_coordination->generateUUIDForTable(*create_database_query);

        /// Add the clause `IF NOT EXISTS` if that is specified in the restore settings.
        create_database_query->if_not_exists = (restore_settings.create_database == RestoreTableCreationMode::kCreateIfNotExists);

#if CLICKHOUSE_CLOUD
        bool shared_catalog  = SharedDatabaseCatalog::initialized();
        auto & create = create_database_query->as<ASTCreateQuery &>();
        auto engine_name = create.storage != nullptr && create.storage->engine != nullptr ? create.storage->engine->name : "";

        if (shared_catalog && engine_name == "Replicated")
        {
            auto engine = std::make_shared<ASTFunction>();

            engine->name = "Shared";
            engine->no_empty_args = true;

            create.storage->set(create.storage->engine, engine);
        }
        else if (!shared_catalog && engine_name == "Shared")
        {
            // Change engine to Replicated
            auto engine = makeASTFunction("Replicated",
                    std::make_shared<ASTLiteral>("/clickhouse/databases/{uuid}"),
                    std::make_shared<ASTLiteral>("{shard}"),
                    std::make_shared<ASTLiteral>("{replica}")
                );

            create.storage->set(create.storage->engine, engine);
        }
#endif

        LOG_TRACE(log, "Creating database {}: {}", backQuoteIfNeed(database_name), create_database_query->formatForLogging());

        auto create_query_context = Context::createCopy(query_context);
        create_query_context->setSetting("allow_deprecated_database_ordinary", 1);

#if CLICKHOUSE_CLOUD
        if (shared_catalog && SharedDatabaseCatalog::instance().shouldRestoreDatabase(create_database_query))
        {
            SharedDatabaseCatalog::instance().createDatabaseRestoredFromBackup(
                database_name,
                create_database_query,
                create_query_context,
                restore_coordination,
                std::chrono::duration_cast<std::chrono::milliseconds>(create_table_timeout).count());

            return;
        }
#endif

        /// Execute CREATE DATABASE query.
        InterpreterCreateQuery interpreter{create_database_query, create_query_context};
        interpreter.setInternal(true);
        interpreter.execute();
    }
    catch (Exception & e)
    {
        e.addMessage("While creating database {}", backQuoteIfNeed(database_name));
        throw;
    }
}

void RestorerFromBackup::checkDatabase(const String & database_name)
{
    try
    {
        /// We must be able to find the database now (either because it has been just created or because it existed before this RESTORE).
        DatabasePtr database = DatabaseCatalog::instance().getDatabase(database_name);

        ASTPtr database_def_from_backup;
        bool is_predefined_database;

        {
            std::lock_guard lock{mutex};
            auto & database_info = database_infos.at(database_name);
            database_info.database = database;
            database_def_from_backup = database_info.create_database_query;
            is_predefined_database = database_info.is_predefined_database;
        }

        auto is_shared = [](ASTPtr ast) -> bool
        {
            auto create = ast->as<ASTCreateQuery &>();
            return create.storage && create.storage->engine && create.storage->engine->name == "Shared";
        };
        bool shared_migration = is_shared(database_def_from_backup) != is_shared(database->getCreateDatabaseQuery());

        /// Check that the database's definition is the same as expected.
        if (!restore_settings.allow_different_database_def && !is_predefined_database && !shared_migration)
        {
            ASTPtr existing_database_def = database->getCreateDatabaseQuery();
            if (!BackupUtils::compareRestoredDatabaseDef(*existing_database_def, *database_def_from_backup, context->getGlobalContext()))
            {
                throw Exception(
                    ErrorCodes::CANNOT_RESTORE_DATABASE,
                    "The database has a different definition: {} "
                    "comparing to its definition in the backup: {}",
                    existing_database_def->formatForErrorMessage(),
                    database_def_from_backup->formatForErrorMessage());
            }
        }
    }
    catch (Exception & e)
    {
        e.addMessage("While checking database {}", backQuoteIfNeed(database_name));
        throw;
    }
}

void RestorerFromBackup::applyCustomStoragePolicy(ASTPtr query_ptr)
{
    constexpr auto setting_name = "storage_policy";
    if (query_ptr && restore_settings.storage_policy.has_value())
    {
        ASTStorage * storage = query_ptr->as<ASTCreateQuery &>().storage;
        if (storage && storage->settings)
        {
            if (restore_settings.storage_policy.value().empty())
                /// it has been set to "" deliberately, so the source storage policy is erased
                storage->settings->changes.removeSetting(setting_name); // NOLINT
            else
                /// it has been set to a custom value, so it either overwrites the existing value or is added as a new one
                storage->settings->changes.setSetting(setting_name, restore_settings.storage_policy.value());
        }
    }
}

void RestorerFromBackup::removeUnresolvedDependencies()
{
    std::lock_guard lock{mutex};

    auto need_exclude_dependency = [&](const StorageID & table_id) TSA_REQUIRES(mutex) -> bool {
        /// Table will be restored.
        if (table_infos.contains(table_id.getQualifiedName()))
            return false;

        /// Table exists and it already exists
        if (!DatabaseCatalog::instance().isTableExist(table_id, context))
        {
            LOG_WARNING(
                log,
                "Tables {} in backup depend on {}, but seems like {} is not in the backup and does not exist. "
                "Will try to ignore that and restore tables",
                fmt::join(tables_dependencies.getDependents(table_id), ", "),
                table_id,
                table_id);
        }

        size_t num_dependencies;
        size_t num_dependents;
        tables_dependencies.getNumberOfAdjacents(table_id, num_dependencies, num_dependents);
        if (num_dependencies || !num_dependents)
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Table {} in backup doesn't have dependencies and dependent tables as it expected to. It's a bug",
                table_id);

        return true; /// Exclude this dependency.
    };

    tables_dependencies.removeTablesIf(need_exclude_dependency); // NOLINT

    if (tables_dependencies.getNumberOfTables() != table_infos.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Number of tables to be restored is not as expected. It's a bug");

    if (tables_dependencies.hasCyclicDependencies())
    {
        LOG_WARNING(
            log,
            "Tables {} in backup have cyclic dependencies: {}. Will try to ignore that and restore tables",
            fmt::join(tables_dependencies.getTablesWithCyclicDependencies(), ", "),
            tables_dependencies.describeCyclicDependencies());
    }
}

void RestorerFromBackup::createAndCheckTables()
{
    /// We need to create tables considering their dependencies.
    std::vector<std::vector<StorageID>> tables_to_create;
    {
        std::lock_guard lock{mutex};
        tables_dependencies.log();
        tables_to_create = tables_dependencies.getTablesSplitByDependencyLevel();
    }

    for (const auto & table_ids : tables_to_create)
        createAndCheckTablesWithSameDependencyLevel(table_ids);
}

void RestorerFromBackup::createAndCheckTablesWithSameDependencyLevel(const std::vector<StorageID> & table_ids)
{
    for (const auto & table_id : table_ids)
        createAndCheckTable(table_id.getQualifiedName());
    waitFutures();
}

void RestorerFromBackup::createAndCheckTable(const QualifiedTableName & table_name)
{
    schedule(
        [this, table_name]() { createAndCheckTableImpl(table_name); },
        "Restore_MakeTbl");
}

void RestorerFromBackup::createAndCheckTableImpl(const QualifiedTableName & table_name)
{
    checkIsQueryCancelled();
    createTable(table_name);
    checkTable(table_name);
}

void RestorerFromBackup::createTable(const QualifiedTableName & table_name)
{
    try
    {
        if (restore_settings.create_table == RestoreTableCreationMode::kMustExist)
            return;

        std::shared_ptr<ASTCreateQuery> create_table_query;
        DatabasePtr database;

        {
            std::lock_guard lock{mutex};
            const auto & table_info = table_infos.at(table_name);

            /// Predefined tables always exist, we don't need to create them while restoring.
            if (table_info.is_predefined_table)
                return;

            create_table_query = typeid_cast<std::shared_ptr<ASTCreateQuery>>(table_info.create_table_query->clone());
            database = table_info.database;
        }

        /// Generate a new UUID for a table (the same table on different hosts must use the same UUID, `restore_coordination` will make it so).
        /// The generated UUID will be ignored if the database does not support UUIDs.
        restore_coordination->generateUUIDForTable(*create_table_query);

        /// Add the clause `IF NOT EXISTS` if that is specified in the restore settings.
        create_table_query->if_not_exists = (restore_settings.create_table == RestoreTableCreationMode::kCreateIfNotExists);

        if (query_context->getSettingsRef()[Setting::restore_replicated_merge_tree_to_shared_merge_tree])
        {
            LOG_INFO(log, "`restore_replicated_merge_tree_to_shared_merge_tree` enabled, will try to replace Replicated engine with Shared");
            ASTStorage * storage = create_table_query->storage;
            if (storage != nullptr && storage->engine != nullptr)
                boost::replace_first(storage->engine->name, "Replicated", "Shared");
            else if (create_table_query->is_materialized_view_with_inner_table())
            {
                storage = create_table_query->targets->getInnerEngine(ViewTarget::To).get();
                if (storage != nullptr && storage->engine != nullptr)
                    boost::replace_first(storage->engine->name, "Replicated", "Shared");
            }
        }

        LOG_TRACE(log, "Creating {}: {}",
                  tableNameWithTypeToString(table_name.database, table_name.table, false), create_table_query->formatForLogging());

        if (!database)
        {
            /// The database containing this table is expected to exist already
            /// (either because it was created by the same RESTORE or because it existed before this RESTORE).
            database = DatabaseCatalog::instance().getDatabase(table_name.database);
            std::lock_guard lock{mutex};
            auto & table_info = table_infos.at(table_name);
            if (!table_info.database)
                table_info.database = database;
        }

        auto create_query_context = Context::createCopy(query_context);
        create_query_context->setSetting("database_replicated_allow_explicit_uuid", 3);
        create_query_context->setSetting("database_replicated_allow_replicated_engine_arguments", 3);

        /// Creating of replicated tables may need retries.
        create_query_context->setSetting("keeper_max_retries", zookeeper_retries_info.max_retries);
        create_query_context->setSetting("keeper_initial_backoff_ms", zookeeper_retries_info.initial_backoff_ms);
        create_query_context->setSetting("keeper_max_backoff_ms", zookeeper_retries_info.max_backoff_ms);

        create_query_context->setUnderRestore(true);

        /// Execute CREATE TABLE query (we call IDatabase::createTableRestoredFromBackup() to allow the database to do some
        /// database-specific things).
        database->createTableRestoredFromBackup(
            create_table_query,
            create_query_context,
            restore_coordination,
            std::chrono::duration_cast<std::chrono::milliseconds>(create_table_timeout).count());
    }
    catch (Exception & e)
    {
        e.addMessage("While creating {}", tableNameWithTypeToString(table_name.database, table_name.table, false));
        throw;
    }
}

void RestorerFromBackup::checkTable(const QualifiedTableName & table_name)
{
    try
    {
        auto resolved_id = (table_name.database == DatabaseCatalog::TEMPORARY_DATABASE)
            ? context->resolveStorageID(StorageID{"", table_name.table}, Context::ResolveExternal)
            : context->resolveStorageID(StorageID{table_name.database, table_name.table}, Context::ResolveGlobal);

        DatabasePtr database;

        {
            std::lock_guard lock{mutex};
            const auto & table_info = table_infos.at(table_name);
            database = table_info.database;
        }

        if (!database)
            database = DatabaseCatalog::instance().getDatabase(table_name.database);

        /// We must be able to find the database now (either because it has been just created or because it existed before this RESTORE).
        StoragePtr storage = database->getTable(resolved_id.table_name, context);

        /// We will keep the table lock until this RESTORE finishes.
        auto table_lock = storage->lockForShare(context->getInitialQueryId(), context->getSettingsRef()[Setting::lock_acquire_timeout]);

        ASTPtr table_def_from_backup;
        bool is_predefined_table;

        {
            std::lock_guard lock{mutex};
            auto & table_info = table_infos.at(table_name);
            if (!table_info.database)
                table_info.database = database;
            table_info.storage = storage;
            table_info.table_lock = table_lock;
            table_def_from_backup = table_info.create_table_query;
            is_predefined_table = table_info.is_predefined_table;
        }

        if (!restore_settings.allow_different_table_def && !is_predefined_table &&
            !query_context->getSettingsRef()[Setting::restore_replicated_merge_tree_to_shared_merge_tree])
        {
            ASTPtr existing_table_def = database->getCreateTableQuery(resolved_id.table_name, context);
            if (!BackupUtils::compareRestoredTableDef(*existing_table_def, *table_def_from_backup, context->getGlobalContext()))
            {
                throw Exception(
                    ErrorCodes::CANNOT_RESTORE_TABLE,
                    "The table has a different definition: {} "
                    "comparing to its definition in the backup: {}",
                    existing_table_def->formatForErrorMessage(),
                    table_def_from_backup->formatForErrorMessage());
            }
        }
    }
    catch (Exception & e)
    {
        e.addMessage("While checking {}", tableNameWithTypeToString(table_name.database, table_name.table, false));
        throw;
    }
}

void RestorerFromBackup::insertDataToTables()
{
    std::vector<QualifiedTableName> table_names;
    {
        std::lock_guard lock{mutex};
        table_names.reserve(table_infos.size());
        std::ranges::copy(table_infos | boost::adaptors::map_keys, std::back_inserter(table_names));
    }

    for (const auto & table_name : table_names)
        insertDataToTable(table_name);

    waitFutures();
}

void RestorerFromBackup::insertDataToTable(const QualifiedTableName & table_name)
{
    if (restore_settings.structure_only)
        return;

    StoragePtr storage;
    String data_path_in_backup;
    std::optional<ASTs> partitions;
    {
        std::lock_guard lock{mutex};
        auto & table_info = table_infos.at(table_name);
        storage = table_info.storage;
        data_path_in_backup = table_info.data_path_in_backup;
        partitions = table_info.partitions;
    }

    schedule(
        [this, table_name, storage, data_path_in_backup, partitions]() { insertDataToTableImpl(table_name, storage, data_path_in_backup, partitions); },
        "Restore_TblData");
}

void RestorerFromBackup::insertDataToTableImpl(const QualifiedTableName & table_name, StoragePtr storage, const String & data_path_in_backup, const std::optional<ASTs> & partitions)
{
    try
    {
        if (partitions && !storage->supportsBackupPartition())
        {
            throw Exception(
                ErrorCodes::CANNOT_RESTORE_TABLE,
                "Table engine {} doesn't support partitions",
                storage->getName());
        }
        storage->restoreDataFromBackup(*this, data_path_in_backup, partitions);
    }
    catch (Exception & e)
    {
        e.addMessage("While restoring data of {}", tableNameWithTypeToString(table_name.database, table_name.table, false));
        throw;
    }
}

void RestorerFromBackup::addDataRestoreTask(DataRestoreTask && new_task)
{
    if (current_stage != Stage::INSERTING_DATA_TO_TABLES)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding of data-restoring tasks is not allowed");

    std::lock_guard lock{mutex};
    data_restore_tasks.push_back(std::move(new_task));
}

void RestorerFromBackup::addDataRestoreTasks(DataRestoreTasks && new_tasks)
{
    if (current_stage != Stage::INSERTING_DATA_TO_TABLES)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Adding of data-restoring tasks is not allowed");

    std::lock_guard lock{mutex};
    insertAtEnd(data_restore_tasks, std::move(new_tasks));
}

void RestorerFromBackup::runDataRestoreTasks()
{
    /// Iterations are required here because data restore tasks are allowed to call addDataRestoreTask() and add other data restore tasks.
    for (;;)
    {
        std::vector<DataRestoreTask> tasks_to_run;
        {
            std::lock_guard lock{mutex};
            std::swap(tasks_to_run, data_restore_tasks);
        }

        if (tasks_to_run.empty())
            break;

        for (auto & task : tasks_to_run)
            schedule(std::move(task), "Restore_TblTask");

        waitFutures();
    }
}

void RestorerFromBackup::finalizeTables()
{
    std::vector<StoragePtr> tables;
    {
        std::lock_guard lock{mutex};
        tables.reserve(table_infos.size());
        for (const auto & [_, info] : table_infos)
            tables.push_back(info.storage);
    }

    for (const auto & storage : tables)
        storage->finalizeRestoreFromBackup();
}

void RestorerFromBackup::throwTableIsNotEmpty(const StorageID & storage_id)
{
    throw Exception(
        ErrorCodes::CANNOT_RESTORE_TABLE,
        "Cannot restore the table {} because it already contains some data. You can set structure_only=true or "
        "allow_non_empty_tables=true to overcome that in the way you want",
        storage_id.getFullTableName());
}
}
