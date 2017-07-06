#include <DataStreams/ColumnGathererStream.h>
#include <common/logger_useful.h>
#include <iomanip>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int INCOMPATIBLE_COLUMNS;
    extern const int INCORRECT_NUMBER_OF_COLUMNS;
    extern const int EMPTY_DATA_PASSED;
    extern const int RECEIVED_EMPTY_DATA;
}

ColumnGathererStream::ColumnGathererStream(const BlockInputStreams & source_streams, const String & column_name_,
                                           const MergedRowSources & row_source_, size_t block_preferred_size_)
    : name(column_name_), row_source(row_source_), block_preferred_size(block_preferred_size_), log(&Logger::get("ColumnGathererStream"))
{
    if (source_streams.empty())
        throw Exception("There are no streams to gather", ErrorCodes::EMPTY_DATA_PASSED);

    children.assign(source_streams.begin(), source_streams.end());
}


String ColumnGathererStream::getID() const
{
    std::stringstream res;

    res << getName() << "(";
    for (size_t i = 0; i < children.size(); i++)
        res << (i == 0 ? "" : ", " ) << children[i]->getID();
    res << ")";

    return res.str();
}


void ColumnGathererStream::init()
{
    sources.reserve(children.size());
    for (size_t i = 0; i < children.size(); i++)
    {
        sources.emplace_back(children[i]->read(), name);

        Block & block = sources.back().block;

        /// Sometimes MergeTreeReader injects additional column with partitioning key
        if (block.columns() > 2 || !block.has(name))
            throw Exception("Block should have 1 or 2 columns and contain column with requested name", ErrorCodes::INCORRECT_NUMBER_OF_COLUMNS);

        if (i == 0)
        {
            column.name = name;
            column.type = block.getByName(name).type->clone();
            column.column = column.type->createColumn();
        }

        if (block.getByName(name).column->getName() != column.column->getName())
            throw Exception("Column types don't match", ErrorCodes::INCOMPATIBLE_COLUMNS);
    }
}


Block ColumnGathererStream::readImpl()
{
    /// Special case: single source and there are no skipped rows
    if (children.size() == 1 && row_source.size() == 0)
        return children[0]->read();

    /// Initialize first source blocks
    if (sources.empty())
        init();

    if (pos_global_start >= row_source.size())
        return Block();

    block_res = Block{column.cloneEmpty()};
    IColumn & column_res = *block_res.getByPosition(0).column;

    column_res.gather(*this);

    return std::move(block_res);
}


void ColumnGathererStream::fetchNewBlock(Source & source, size_t source_num)
{
    try
    {
        source.block = children[source_num]->read();
        source.update(name);
    }
    catch (Exception & e)
    {
        e.addMessage("Cannot fetch required block. Stream " + children[source_num]->getID() + ", part " + toString(source_num));
        throw;
    }

    if (0 == source.size)
    {
        throw Exception("Fetched block is empty. Stream " + children[source_num]->getID() + ", part " + toString(source_num),
                        ErrorCodes::RECEIVED_EMPTY_DATA);
    }
}


void ColumnGathererStream::readSuffixImpl()
{
    const BlockStreamProfileInfo & profile_info = getProfileInfo();
    double seconds = profile_info.total_stopwatch.elapsedSeconds();
    LOG_DEBUG(log, std::fixed << std::setprecision(2)
        << "Gathered column " << name
        << " (" << static_cast<double>(profile_info.bytes) / profile_info.rows << " bytes/elem.)"
        << " in " << seconds << " sec., "
        << profile_info.rows / seconds << " rows/sec., "
        << profile_info.bytes / 1048576.0 / seconds << " MiB/sec.");
}

}
