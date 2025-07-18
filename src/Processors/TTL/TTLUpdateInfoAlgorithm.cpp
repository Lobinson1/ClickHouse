#include <Processors/TTL/TTLUpdateInfoAlgorithm.h>

namespace DB
{

TTLUpdateInfoAlgorithm::TTLUpdateInfoAlgorithm(
    const TTLExpressions & ttl_expressions_,
    const TTLDescription & description_,
    TTLUpdateField ttl_update_field_,
    String ttl_update_key_,
    const TTLInfo & old_ttl_info_,
    time_t current_time_,
    bool force_)
    : ITTLAlgorithm(ttl_expressions_, description_, old_ttl_info_, current_time_, force_)
    , ttl_update_field(ttl_update_field_)
    , ttl_update_key(ttl_update_key_)
{
}

void TTLUpdateInfoAlgorithm::execute(Block & block)
{
    if (block.empty())
        return;

    auto ttl_column = executeExpressionAndGetColumn(ttl_expressions.expression, block, description.result_column);
    for (size_t i = 0; i < block.rows(); ++i)
    {
        Int64 cur_ttl = ITTLAlgorithm::getTimestampByIndex(ttl_column.get(), i);
        new_ttl_info.update(cur_ttl);
    }
}

void TTLUpdateInfoAlgorithm::finalize(const MutableDataPartPtr & data_part) const
{
    if (ttl_update_field == TTLUpdateField::RECOMPRESSION_TTL)
    {
        data_part->ttl_infos.recompression_ttl[ttl_update_key] = new_ttl_info;
    }
    else if (ttl_update_field == TTLUpdateField::MOVES_TTL)
    {
        data_part->ttl_infos.moves_ttl[ttl_update_key] = new_ttl_info;
    }
    else if (ttl_update_field == TTLUpdateField::GROUP_BY_TTL)
    {
        data_part->ttl_infos.group_by_ttl[ttl_update_key] = new_ttl_info;
        data_part->ttl_infos.updatePartMinMaxTTL(new_ttl_info.min, new_ttl_info.max);
    }
    else if (ttl_update_field == TTLUpdateField::ROWS_WHERE_TTL)
    {
        data_part->ttl_infos.rows_where_ttl[ttl_update_key] = new_ttl_info;
        data_part->ttl_infos.updatePartMinMaxTTL(new_ttl_info.min, new_ttl_info.max);
    }
    else if (ttl_update_field == TTLUpdateField::TABLE_TTL)
    {
        data_part->ttl_infos.table_ttl = new_ttl_info;
        data_part->ttl_infos.updatePartMinMaxTTL(new_ttl_info.min, new_ttl_info.max);
    }
    else if (ttl_update_field == TTLUpdateField::COLUMNS_TTL)
    {
        data_part->ttl_infos.columns_ttl[ttl_update_key] = new_ttl_info;
        data_part->ttl_infos.updatePartMinMaxTTL(new_ttl_info.min, new_ttl_info.max);
    }

}

}
