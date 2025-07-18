#pragma once

#include <Interpreters/PreparedSets.h>
#include <Processors/IAccumulatingTransform.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Processors/TTL/ITTLAlgorithm.h>
#include <Processors/TTL/TTLDeleteAlgorithm.h>

namespace DB
{

class Block;

class TTLTransform : public IAccumulatingTransform
{
public:
    TTLTransform(
        const ContextPtr & context,
        SharedHeader header_,
        const MergeTreeData & storage_,
        const StorageMetadataPtr & metadata_snapshot_,
        const MergeTreeData::MutableDataPartPtr & data_part_,
        time_t current_time,
        bool force_
    );

    String getName() const override { return "TTL"; }

    Status prepare() override;

    PreparedSets::Subqueries getSubqueries() { return std::move(subqueries_for_sets); }

protected:
    void consume(Chunk chunk) override;
    Chunk generate() override;

    /// Finalizes ttl infos and updates data part
    void finalize();

private:
    std::vector<TTLAlgorithmPtr> algorithms;
    const TTLDeleteAlgorithm * delete_algorithm = nullptr;
    bool all_data_dropped = false;

    PreparedSets::Subqueries subqueries_for_sets;

    /// ttl_infos and empty_columns are updating while reading
    const MergeTreeData::MutableDataPartPtr & data_part;
    LoggerPtr log;
};

}
