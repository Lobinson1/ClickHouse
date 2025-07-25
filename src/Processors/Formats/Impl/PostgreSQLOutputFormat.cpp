#include <Processors/Formats/Impl/PostgreSQLOutputFormat.h>

#include <Columns/IColumn.h>
#include <Formats/FormatFactory.h>
#include <Interpreters/ProcessList.h>

#include <Processors/Port.h>

namespace DB
{

PostgreSQLOutputFormat::PostgreSQLOutputFormat(WriteBuffer & out_, SharedHeader header_, const FormatSettings & settings_)
    : IOutputFormat(header_, out_)
    , format_settings(settings_)
    , message_transport(&out)
{
}

void PostgreSQLOutputFormat::writePrefix()
{
    const auto & header = getPort(PortKind::Main).getHeader();
    auto data_types = header.getDataTypes();

    if (header.columns())
    {
        std::vector<PostgreSQLProtocol::Messaging::FieldDescription> columns;
        columns.reserve(header.columns());

        for (size_t i = 0; i < header.columns(); ++i)
        {
            const auto & column_name = header.getColumnsWithTypeAndName()[i].name;
            columns.emplace_back(column_name, data_types[i]->getTypeId());
            serializations.emplace_back(data_types[i]->getDefaultSerialization());
        }
        message_transport.send(PostgreSQLProtocol::Messaging::RowDescription(columns));
    }
}

void PostgreSQLOutputFormat::consume(Chunk chunk)
{
    for (size_t i = 0; i != chunk.getNumRows(); ++i)
    {
        const Columns & columns = chunk.getColumns();
        std::vector<std::shared_ptr<PostgreSQLProtocol::Messaging::ISerializable>> row;
        row.reserve(chunk.getNumColumns());

        for (size_t j = 0; j != chunk.getNumColumns(); ++j)
        {
            if (columns[j]->isNullAt(i))
                row.push_back(std::make_shared<PostgreSQLProtocol::Messaging::NullField>());
            else
            {
                WriteBufferFromOwnString ostr;
                serializations[j]->serializeText(*columns[j], i, ostr, format_settings);
                row.push_back(std::make_shared<PostgreSQLProtocol::Messaging::StringField>(std::move(ostr.str())));
            }
        }

        message_transport.send(PostgreSQLProtocol::Messaging::DataRow(row));
    }
}

void PostgreSQLOutputFormat::flushImpl()
{
    message_transport.flush();
}

void registerOutputFormatPostgreSQLWire(FormatFactory & factory)
{
    factory.registerOutputFormat(
        "PostgreSQLWire",
        [](WriteBuffer & buf,
           const Block & sample,
           const FormatSettings & settings) { return std::make_shared<PostgreSQLOutputFormat>(buf, std::make_shared<const Block>(sample), settings); });
    factory.markOutputFormatNotTTYFriendly("PostgreSQLWire");
    factory.setContentType("PostgreSQLWire", "application/octet-stream");
}

}
