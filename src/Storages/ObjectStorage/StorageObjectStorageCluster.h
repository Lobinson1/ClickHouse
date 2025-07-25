#pragma once
#include <Storages/IStorageCluster.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Interpreters/Context_fwd.h>

namespace DB
{

class StorageObjectStorageCluster : public IStorageCluster
{
public:
    StorageObjectStorageCluster(
        const String & cluster_name_,
        StorageObjectStorageConfigurationPtr configuration_,
        ObjectStoragePtr object_storage_,
        const StorageID & table_id_,
        const ColumnsDescription & columns_,
        const ConstraintsDescription & constraints_,
        ContextPtr context_);

    std::string getName() const override;

    RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node * predicate,
        const ActionsDAG * filter,
        const ContextPtr & context,
        size_t number_of_replicas) const override;

    String getPathSample(StorageInMemoryMetadata metadata, ContextPtr context);

    std::optional<UInt64> totalRows(ContextPtr query_context) const override;
    std::optional<UInt64> totalBytes(ContextPtr query_context) const override;

private:
    void updateQueryToSendIfNeeded(
        ASTPtr & query,
        const StorageSnapshotPtr & storage_snapshot,
        const ContextPtr & context) override;

    const String engine_name;
    const StorageObjectStorageConfigurationPtr configuration;
    const ObjectStoragePtr object_storage;
    NamesAndTypesList virtual_columns;
};

}
