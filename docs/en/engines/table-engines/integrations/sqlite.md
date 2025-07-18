---
description: 'The engine allows to import and export data to SQLite and supports queries
  to SQLite tables directly from ClickHouse.'
sidebar_label: 'SQLite'
sidebar_position: 185
slug: /engines/table-engines/integrations/sqlite
title: 'SQLite'
---

import CloudNotSupportedBadge from '@theme/badges/CloudNotSupportedBadge';

# SQLite

<CloudNotSupportedBadge/>

The engine allows to import and export data to SQLite and supports queries to SQLite tables directly from ClickHouse.

## Creating a table {#creating-a-table}

```sql
    CREATE TABLE [IF NOT EXISTS] [db.]table_name
    (
        name1 [type1],
        name2 [type2], ...
    ) ENGINE = SQLite('db_path', 'table')
```

**Engine Parameters**

- `db_path` — Path to SQLite file with a database.
- `table` — Name of a table in the SQLite database.

## Usage example {#usage-example}

Shows a query creating the SQLite table:

```sql
SHOW CREATE TABLE sqlite_db.table2;
```

```text
CREATE TABLE SQLite.table2
(
    `col1` Nullable(Int32),
    `col2` Nullable(String)
)
ENGINE = SQLite('sqlite.db','table2');
```

Returns the data from the table:

```sql
SELECT * FROM sqlite_db.table2 ORDER BY col1;
```

```text
┌─col1─┬─col2──┐
│    1 │ text1 │
│    2 │ text2 │
│    3 │ text3 │
└──────┴───────┘
```

**See Also**

- [SQLite](../../../engines/database-engines/sqlite.md) engine
- [sqlite](../../../sql-reference/table-functions/sqlite.md) table function
