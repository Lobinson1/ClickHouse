#include <optional>
#include <Processors/Transforms/JoiningTransform.h>

#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/GraceHashJoin.h>
#include <Interpreters/JoinUtils.h>
#include <Processors/Port.h>
#include <Common/logger_useful.h>

namespace ProfileEvents
{
    extern const Event JoinBuildTableRowCount;
    extern const Event JoinProbeTableRowCount;
    extern const Event JoinResultRowCount;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

Block JoiningTransform::transformHeader(Block header, const JoinPtr & join)
{
    LOG_TEST(getLogger("JoiningTransform"), "Before join block: '{}'", header.dumpStructure());
    join->checkTypesOfKeys(header);
    join->initialize(header);
    header = join->joinBlock(std::move(header))->next().block;
    materializeBlockInplace(header);
    LOG_TEST(getLogger("JoiningTransform"), "After join block: '{}'", header.dumpStructure());
    return header;
}

JoiningTransform::JoiningTransform(
    SharedHeader input_header,
    SharedHeader output_header,
    JoinPtr join_,
    size_t max_block_size_,
    bool on_totals_,
    bool default_totals_,
    FinishCounterPtr finish_counter_)
    : IProcessor({input_header}, {output_header})
    , join(std::move(join_))
    , on_totals(on_totals_)
    , default_totals(default_totals_)
    , finish_counter(std::move(finish_counter_))
    , max_block_size(max_block_size_)
{
    if (!join->isFilled())
        inputs.emplace_back(Block(), this); // Wait for FillingRightJoinSideTransform
}

JoiningTransform::~JoiningTransform() = default;

OutputPort & JoiningTransform::getFinishedSignal()
{
    assert(outputs.size() == 2);
    return outputs.back();
}

IProcessor::Status JoiningTransform::prepare()
{
    auto & output = outputs.front();
    auto & on_finish_output = outputs.back();

    /// Check can output.
    if (output.isFinished() || stop_reading)
    {
        output.finish();
        on_finish_output.finish();
        for (auto & input : inputs)
            input.close();
        return Status::Finished;
    }

    if (!output.canPush())
    {
        for (auto & input : inputs)
            input.setNotNeeded();
        return Status::PortFull;
    }

    /// Output if has data.
    if (output_chunk)
    {
        output.push(std::move(*(output_chunk)));
        output_chunk.reset();

        return Status::PortFull;
    }

    if (inputs.size() > 1)
    {
        auto & last_in = inputs.back();
        if (!last_in.isFinished())
        {
            last_in.setNeeded();
            if (last_in.hasData())
                throw Exception(ErrorCodes::LOGICAL_ERROR, "No data is expected from second JoiningTransform port");

            return Status::NeedData;
        }
    }

    if (has_input)
        return Status::Ready;

    auto & input = inputs.front();
    if (input.isFinished())
    {
        if (process_non_joined)
            return Status::Ready;

        output.finish();
        on_finish_output.finish();
        return Status::Finished;
    }

    input.setNeeded();

    if (!input.hasData())
        return Status::NeedData;

    input_chunk = input.pull(true);
    has_input = input_chunk.hasRows() || on_totals;
    return Status::Ready;
}

void JoiningTransform::work()
{
    if (has_input)
    {
        chassert(!output_chunk.has_value());
        transform(input_chunk);
        has_input = join_result != nullptr;
    }
    else
    {
        if (!non_joined_blocks)
        {
            if (!finish_counter || !finish_counter->isLast())
            {
                process_non_joined = false;
                return;
            }

            non_joined_blocks = join->getNonJoinedBlocks(
                inputs.front().getHeader(), outputs.front().getHeader(), max_block_size);
            if (!non_joined_blocks)
            {
                process_non_joined = false;
                return;
            }
        }

        Block block = non_joined_blocks->next();
        if (block.empty())
        {
            process_non_joined = false;
            return;
        }

        if (block.rows())
        {
            ProfileEvents::increment(ProfileEvents::JoinResultRowCount, block.rows());
            output_chunk = Chunk(block.getColumns(), block.rows());
        }
    }
}

void JoiningTransform::transform(Chunk & chunk)
{
    if (!initialized)
    {
        initialized = true;

        if (join->alwaysReturnsEmptySet() && !on_totals)
        {
            stop_reading = true;
            chunk.clear();
            return;
        }
    }

    Block res;
    if (on_totals)
    {
        const auto & left_totals = inputs.front().getHeader().cloneWithColumns(chunk.detachColumns());
        const auto & right_totals = join->getTotals();

        /// Drop totals if both out stream and joined stream doesn't have ones.
        /// See comment in ExpressionTransform.h
        if (default_totals && right_totals.empty())
            return;

        res = outputs.front().getHeader().cloneEmpty();
        JoinCommon::joinTotals(left_totals, right_totals, join->getTableJoin(), res);
    }
    else
    {
        res = readExecute(chunk);
    }

    if (res.rows())
    {
        ProfileEvents::increment(ProfileEvents::JoinResultRowCount, res.rows());
        output_chunk = Chunk(res.getColumns(), res.rows());
    }
}

Block JoiningTransform::readExecute(Chunk & chunk)
{
    if (!join_result)
    {
        Block block = inputs.front().getHeader().cloneWithColumns(chunk.detachColumns());
        ProfileEvents::increment(ProfileEvents::JoinProbeTableRowCount, block.rows());
        join_result = join->joinBlock(std::move(block));
    }

    auto data = join_result->next();
    if (data.is_last)
        join_result.reset();

    return std::move(data.block);
}

FillingRightJoinSideTransform::FillingRightJoinSideTransform(SharedHeader input_header, JoinPtr join_, FinishCounterPtr finish_counter_)
    : IProcessor({input_header}, {Block()}), join(std::move(join_)), finish_counter(std::move(finish_counter_))
{
    spillable = typeid_cast<GraceHashJoin *>(join.get());
}

InputPort * FillingRightJoinSideTransform::addTotalsPort()
{
    if (inputs.size() > 1)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Totals port was already added to FillingRightJoinSideTransform");

    return &inputs.emplace_back(inputs.front().getHeader(), this);
}

IProcessor::Status FillingRightJoinSideTransform::prepare()
{
    auto & output = outputs.front();

    /// Check can output.
    if (output.isFinished())
    {
        for (auto & input : inputs)
            input.close();
        return Status::Finished;
    }

    if (!output.canPush())
    {
        for (auto & input : inputs)
            input.setNotNeeded();
        return Status::PortFull;
    }

    auto & input = inputs.front();

    if (stop_reading)
    {
        input.close();
    }
    else if (!input.isFinished())
    {
        input.setNeeded();

        if (!input.hasData())
            return Status::NeedData;

        chunk = input.pull(true);
        return Status::Ready;
    }

    if (inputs.size() > 1)
    {
        auto & totals_input = inputs.back();
        if (!totals_input.isFinished())
        {
            totals_input.setNeeded();

            if (!totals_input.hasData())
                return Status::NeedData;

            chunk = totals_input.pull(true);
            for_totals = true;
            return Status::Ready;
        }
    }
    else if (!set_totals)
    {
        chunk.setColumns(inputs.front().getHeader().cloneEmpty().getColumns(), 0);
        for_totals = true;
        return Status::Ready;
    }

    if (finish_counter->isLast())
        join->onBuildPhaseFinish();

    output.finish();
    return Status::Finished;
}

void FillingRightJoinSideTransform::work()
{
    auto & input = inputs.front();
    auto block = input.getHeader().cloneWithColumns(chunk.detachColumns());

    if (for_totals)
        join->setTotals(block);
    else
    {
        ProfileEvents::increment(ProfileEvents::JoinBuildTableRowCount, block.rows());
        stop_reading = !join->addBlockToJoin(block);
    }

    if (input.isFinished() && !join->supportParallelJoin())
        join->tryRerangeRightTableData();

    set_totals = for_totals;
}

ProcessorMemoryStats FillingRightJoinSideTransform::getMemoryStats()
{
    if (auto * grace_join = typeid_cast<GraceHashJoin *>(join.get()))
    {
        ProcessorMemoryStats res;
        res.spillable_memory_bytes = grace_join->getTotalByteCount();
        // in case the hash table will resize which requires more than 2x additional memory.
        // we must reserve enough memory.
        res.need_reserved_memory_bytes = res.spillable_memory_bytes * 3;
        return res;
    }
    return {};
}

bool FillingRightJoinSideTransform::spillOnSize(size_t bytes)
{
    if (auto * grace_join = typeid_cast<GraceHashJoin *>(join.get()))
    {
        auto total_bytes = grace_join->getTotalByteCount();
        if (total_bytes >= bytes)
        {
            grace_join->forceSpill();
            return true;
        }
    }
    return false;
}

DelayedJoinedBlocksWorkerTransform::DelayedJoinedBlocksWorkerTransform(
    SharedHeader output_header_,
    NonJoinedStreamBuilder non_joined_stream_builder_)
    : IProcessor(InputPorts{Block()}, OutputPorts{output_header_})
    , non_joined_stream_builder(std::move(non_joined_stream_builder_))
{
}

IProcessor::Status DelayedJoinedBlocksWorkerTransform::prepare()
{
    auto & output = outputs.front();
    auto & input = inputs.front();

    if (output.isFinished())
    {
        input.close();
        return Status::Finished;
    }

    if (!output.canPush())
    {
        input.setNotNeeded();
        return Status::PortFull;
    }

    if (inputs.size() != 1 && outputs.size() != 1)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "DelayedJoinedBlocksWorkerTransform must have exactly one input port");

    if (output_chunk)
    {
        input.setNotNeeded();

        if (!output.canPush())
            return Status::PortFull;

        output.push(std::move(output_chunk));
        output_chunk.clear();
        return Status::PortFull;
    }

    if (!task)
    {
        if (!input.hasData())
        {
            input.setNeeded();
            return Status::NeedData;
        }

        auto data = input.pullData(true);
        if (data.exception)
        {
            output.pushException(data.exception);
            return Status::Finished;
        }

        task = data.chunk.getChunkInfos().get<DelayedBlocksTask>();
        if (!task)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "DelayedJoinedBlocksWorkerTransform must have chunk info");
    }
    else
    {
        input.setNotNeeded();
    }

    // When delayed_blocks is nullptr, it means that all buckets have been joined.
    if (!task->delayed_blocks)
    {
        input.close();
        output.finish();
        return Status::Finished;
    }

    return Status::Ready;
}

void DelayedJoinedBlocksWorkerTransform::work()
{
    if (!task)
        return;

    Block block;
    /// All joined and non-joined rows from left stream are emitted, only right non-joined rows are left
    if (!task->delayed_blocks->isFinished())
    {
        block = task->delayed_blocks->next();
        if (block.empty())
            block = nextNonJoinedBlock();
    }
    else
    {
        block = nextNonJoinedBlock();
    }
    if (block.empty())
    {
        resetTask();
        return;
    }

    // Add block to the output
    auto rows = block.rows();
    output_chunk.setColumns(block.getColumns(), rows);
}

void DelayedJoinedBlocksWorkerTransform::resetTask()
{
    task.reset();
    non_joined_delayed_stream = nullptr;
}

Block DelayedJoinedBlocksWorkerTransform::nextNonJoinedBlock()
{
    // Before read from non-joined stream, all blocks in left file reader must have been joined.
    // For example, in HashJoin, it may return invalid mismatch rows from non-joined stream before
    // the all blocks in left file reader have been finished, since the used flags are incomplete.
    // To make only one processor could read from non-joined stream seems be a easy way.
    if (!non_joined_delayed_stream && task && task->left_delayed_stream_finish_counter->isLast())
    {
        non_joined_delayed_stream = non_joined_stream_builder();
    }

    if (non_joined_delayed_stream)
    {
        return non_joined_delayed_stream->next();
    }
    return {};
}

DelayedJoinedBlocksTransform::DelayedJoinedBlocksTransform(size_t num_streams, JoinPtr join_)
    : IProcessor(InputPorts{}, OutputPorts(num_streams, Block()))
    , join(std::move(join_))
{
}

void DelayedJoinedBlocksTransform::work()
{
    if (finished)
        return;

    delayed_blocks = join->getDelayedBlocks();
    finished = finished || delayed_blocks == nullptr;
}

IProcessor::Status DelayedJoinedBlocksTransform::prepare()
{
    for (auto & output : outputs)
    {
        if (output.isFinished())
        {
            /// If at least one output is finished, then we have read all data from buckets.
            /// Some workers can still be busy with joining the last chunk of data in memory,
            /// but after that they also will finish when they will try to get next chunk.
            finished = true;
            continue;
        }
        if (!output.canPush())
            return Status::PortFull;
    }

    if (finished)
    {
        // Since have memory limit, cannot handle all buckets parallelly by different
        // DelayedJoinedBlocksWorkerTransform. So send the same task to all outputs.
        // Wait for all DelayedJoinedBlocksWorkerTransform be idle before getting next bucket.
        for (auto & output : outputs)
        {
            if (output.isFinished())
                continue;
            Chunk chunk;
            chunk.getChunkInfos().add(std::make_shared<DelayedBlocksTask>());
            output.push(std::move(chunk));
            output.finish();
        }

        return Status::Finished;
    }

    if (delayed_blocks)
    {
        // This counter is used to ensure that only the last DelayedJoinedBlocksWorkerTransform
        // could read right non-joined blocks from the join.
        auto left_delayed_stream_finished_counter = std::make_shared<FinishCounter>(outputs.size());
        for (auto & output : outputs)
        {
            Chunk chunk;
            auto task = std::make_shared<DelayedBlocksTask>(delayed_blocks, left_delayed_stream_finished_counter);
            chunk.getChunkInfos().add(std::move(task));
            output.push(std::move(chunk));
        }
        delayed_blocks = nullptr;
        return Status::PortFull;
    }

    return Status::Ready;
}

}
