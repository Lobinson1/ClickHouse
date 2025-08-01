#pragma once
#include <Processors/ISimpleTransform.h>
#include <Columns/FilterDescription.h>
#include <Storages/MergeTree/MarkRange.h>

namespace DB
{

class ExpressionActions;
using ExpressionActionsPtr = std::shared_ptr<ExpressionActions>;

class ActionsDAG;
class QueryConditionCache;

/** Implements WHERE, HAVING operations.
  * Takes an expression, which adds to the block one ColumnUInt8 column containing the filtering conditions.
  * The expression is evaluated and result chunks contain only the filtered rows.
  * If remove_filter_column is true, remove filter column from block.
  */
class FilterTransform : public ISimpleTransform
{
public:
    FilterTransform(
        SharedHeader header_, ExpressionActionsPtr expression_, String filter_column_name_,
        bool remove_filter_column_, bool on_totals_ = false, std::shared_ptr<std::atomic<size_t>> rows_filtered_ = nullptr,
        std::optional<std::pair<UInt64, String>> condition_ = std::nullopt);

    static Block
    transformHeader(const Block & header, const ActionsDAG * expression, const String & filter_column_name, bool remove_filter_column);

    String getName() const override { return "FilterTransform"; }

    Status prepare() override;

    void transform(Chunk & chunk) override;

    static bool canUseType(const DataTypePtr & type);

private:
    ExpressionActionsPtr expression;
    String filter_column_name;
    bool remove_filter_column;
    bool on_totals;
    bool always_false = false;
    size_t filter_column_position = 0;

    std::shared_ptr<std::atomic<size_t>> rows_filtered;

    /// If set, we need to update the query condition cache at runtime for every processed chunk
    std::optional<std::pair<UInt64, String>> condition;

    std::shared_ptr<QueryConditionCache> query_condition_cache;

    MarkRangesInfoPtr buffered_mark_ranges_info; /// Buffers mark info for chunks from the same table and part.
                                                 /// The goal is to write less often into the query condition cache (reduce lock contention).

    /// Header after expression, but before removing filter column.
    Block transformed_header;

    bool are_prepared_sets_initialized = false;

    void doTransform(Chunk & chunk);
    void removeFilterIfNeed(Columns & columns) const;

    void writeIntoQueryConditionCache(const MarkRangesInfoPtr & mark_ranges_info);
};

}
