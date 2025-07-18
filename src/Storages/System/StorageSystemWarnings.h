#pragma once

#include <Storages/System/IStorageSystemOneBlock.h>


namespace DB
{

class Context;

/** Implements system.warnings table that contains warnings about server configuration
  * to be displayed in clickhouse-client.
  */
class StorageSystemWarnings final : public IStorageSystemOneBlock
{
public:
    std::string getName() const override { return "SystemWarnings"; }

    static ColumnsDescription getColumnsDescription();

    void truncate(const ASTPtr & query_ast, const StorageMetadataPtr & metadata_snapshot, ContextPtr context, TableExclusiveLockHolder & lock_holder) override;

protected:
    using IStorageSystemOneBlock::IStorageSystemOneBlock;

    void fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const override;
};
}
