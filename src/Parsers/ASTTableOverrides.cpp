#include <IO/Operators.h>
#include <Parsers/ASTColumnDeclaration.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTTableOverrides.h>


namespace DB
{

ASTPtr ASTTableOverride::clone() const
{
    auto res = std::make_shared<ASTTableOverride>(*this);
    res->children.clear();
    res->table_name = table_name;
    if (columns)
        res->set(res->columns, columns->clone());
    if (storage)
        res->set(res->storage, storage->clone());
    return res;
}

void ASTTableOverride::formatImpl(WriteBuffer & ostr, const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const
{
    String nl_or_nothing = settings.one_line ? "" : "\n";
    String nl_or_ws = settings.one_line ? " " : "\n";

    if (is_standalone)
    {
        ostr << "TABLE OVERRIDE ";
        ASTIdentifier(table_name).format(ostr, settings, state, frame);
    }
    auto override_frame = frame;
    if (is_standalone)
    {
        ++override_frame.indent;
        ostr << nl_or_ws << '(' << nl_or_nothing;
    }
    String indent_str = settings.one_line ? "" : String(4 * override_frame.indent, ' ');
    size_t override_elems = 0;
    if (columns)
    {
        FormatStateStacked columns_frame = override_frame;
        columns_frame.expression_list_always_start_on_new_line = true;
        ostr << indent_str << "COLUMNS" << nl_or_ws << indent_str << "(";
        columns->format(ostr, settings, state, columns_frame);
        ostr << nl_or_nothing << indent_str << ")";
        ++override_elems;
    }
    if (storage)
    {
        const auto & format_storage_elem = [&](IAST * elem, const String & elem_name)
        {
            if (elem)
            {
                ostr << (override_elems++ ? nl_or_ws : "") << indent_str << elem_name << ' ';
                elem->format(ostr, settings, state, override_frame);
            }
        };
        format_storage_elem(storage->partition_by, "PARTITION BY");
        format_storage_elem(storage->primary_key, "PRIMARY KEY");
        format_storage_elem(storage->order_by, "ORDER BY");
        format_storage_elem(storage->sample_by, "SAMPLE BY");
        format_storage_elem(storage->ttl_table, "TTL");
    }

    if (is_standalone)
        ostr << nl_or_nothing << ')';
}

ASTPtr ASTTableOverrideList::clone() const
{
    auto res = std::make_shared<ASTTableOverrideList>(*this);
    res->cloneChildren();
    return res;
}

ASTPtr ASTTableOverrideList::tryGetTableOverride(const String & name) const
{
    auto it = positions.find(name);
    if (it == positions.end())
        return nullptr;
    return children[it->second];
}

void ASTTableOverrideList::setTableOverride(const String & name, ASTPtr ast)
{
    auto it = positions.find(name);
    if (it == positions.end())
    {
        positions[name] = children.size();
        children.emplace_back(ast);
    }
    else
    {
        children[it->second] = ast;
    }
}

void ASTTableOverrideList::removeTableOverride(const String & name)
{
    if (hasOverride(name))
    {
        size_t pos = positions[name];
        children.erase(children.begin() + pos);
        positions.erase(name);
        for (auto & pr : positions)
            if (pr.second > pos)
                --pr.second;
    }
}

bool ASTTableOverrideList::hasOverride(const String & name) const
{
    return positions.contains(name);
}

void ASTTableOverrideList::formatImpl(WriteBuffer & ostr, const FormatSettings & settings, FormatState & state, FormatStateStacked frame) const
{
    if (frame.expression_list_prepend_whitespace)
        ostr << ' ';

    for (ASTs::const_iterator it = children.begin(); it != children.end(); ++it)
    {
        if (it != children.begin())
        {
            ostr << (settings.one_line ? ", " : ",\n");
        }

        (*it)->format(ostr, settings, state, frame);
    }
}

}
