import pytest

from helpers.cluster import ClickHouseCluster
from helpers.test_tools import TSV

cluster = ClickHouseCluster(__file__)

# `randomize_settings` is set tot `False` to make result of `SHOW CREATE SETTINGS PROFILE` consistent
instance = cluster.add_instance("instance", randomize_settings=False)


def system_settings_profile(profile_name):
    return TSV(
        instance.query(
            "SELECT name, storage, num_elements, apply_to_all, apply_to_list, apply_to_except FROM system.settings_profiles WHERE name='"
            + profile_name
            + "'"
        )
    )


def system_settings_profile_elements(profile_name=None, user_name=None, role_name=None):
    where = ""
    if profile_name:
        where = " WHERE profile_name='" + profile_name + "'"
    elif user_name:
        where = " WHERE user_name='" + user_name + "'"
    elif role_name:
        where = " WHERE role_name='" + role_name + "'"
    return TSV(instance.query("SELECT * FROM system.settings_profile_elements" + where))


session_id_counter = 0


def new_session_id():
    global session_id_counter
    session_id_counter += 1
    return "session #" + str(session_id_counter)


@pytest.fixture(scope="module", autouse=True)
def setup_nodes():
    try:
        cluster.start()

        instance.query("CREATE USER robin")

        yield cluster

    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def reset_after_test():
    try:
        yield
    finally:
        instance.query("CREATE USER OR REPLACE robin")
        instance.query("DROP ROLE IF EXISTS worker")
        instance.query(
            "DROP SETTINGS PROFILE IF EXISTS xyz, alpha, P1, P2, P3, P4, P5, P6"
        )


def test_smoke():
    # Set settings and constraints via CREATE SETTINGS PROFILE ... TO user
    instance.query(
        "CREATE SETTINGS PROFILE xyz SETTINGS max_memory_usage = 100000001 MIN 90000000 MAX 110000000 TO robin"
    )
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE xyz")
        == "CREATE SETTINGS PROFILE `xyz` SETTINGS max_memory_usage = 100000001 MIN 90000000 MAX 110000000 TO robin\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "100000001\n"
    )
    assert (
        "Setting max_memory_usage shouldn't be less than 90000000"
        in instance.query_and_get_error("SET max_memory_usage = 80000000", user="robin")
    )
    assert (
        "Setting max_memory_usage shouldn't be greater than 110000000"
        in instance.query_and_get_error(
            "SET max_memory_usage = 120000000", user="robin"
        )
    )
    assert system_settings_profile("xyz") == [
        ["xyz", "local_directory", 1, 0, "['robin']", "[]"]
    ]
    assert system_settings_profile_elements(profile_name="xyz") == [
        [
            "xyz",
            "\\N",
            "\\N",
            0,
            "max_memory_usage",
            100000001,
            90000000,
            110000000,
            "\\N",
            "\\N",
        ]
    ]

    instance.query("ALTER SETTINGS PROFILE xyz TO NONE")
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE xyz")
        == "CREATE SETTINGS PROFILE `xyz` SETTINGS max_memory_usage = 100000001 MIN 90000000 MAX 110000000\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "0\n"
    )
    instance.query("SET max_memory_usage = 80000000", user="robin")
    instance.query("SET max_memory_usage = 120000000", user="robin")
    assert system_settings_profile("xyz") == [
        ["xyz", "local_directory", 1, 0, "[]", "[]"]
    ]
    assert system_settings_profile_elements(user_name="robin") == []

    # Set settings and constraints via CREATE USER ... SETTINGS PROFILE
    instance.query("ALTER USER robin SETTINGS PROFILE xyz")
    assert (
        instance.query("SHOW CREATE USER robin")
        == "CREATE USER robin IDENTIFIED WITH no_password SETTINGS PROFILE `xyz`\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "100000001\n"
    )
    assert (
        "Setting max_memory_usage shouldn't be less than 90000000"
        in instance.query_and_get_error("SET max_memory_usage = 80000000", user="robin")
    )
    assert (
        "Setting max_memory_usage shouldn't be greater than 110000000"
        in instance.query_and_get_error(
            "SET max_memory_usage = 120000000", user="robin"
        )
    )
    assert system_settings_profile_elements(user_name="robin") == [
        ["\\N", "robin", "\\N", 0, "\\N", "\\N", "\\N", "\\N", "\\N", "xyz"]
    ]

    instance.query("ALTER USER robin SETTINGS NONE")
    assert (
        instance.query("SHOW CREATE USER robin")
        == "CREATE USER robin IDENTIFIED WITH no_password\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "0\n"
    )
    instance.query("SET max_memory_usage = 80000000", user="robin")
    instance.query("SET max_memory_usage = 120000000", user="robin")
    assert system_settings_profile_elements(user_name="robin") == []


def test_settings_from_granted_role():
    # Set settings and constraints via granted role
    instance.query(
        "CREATE SETTINGS PROFILE xyz SETTINGS max_memory_usage = 100000001 MAX 110000000, max_ast_depth = 2000"
    )
    instance.query("CREATE ROLE worker SETTINGS PROFILE xyz")
    instance.query("GRANT worker TO robin")
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE xyz")
        == "CREATE SETTINGS PROFILE `xyz` SETTINGS max_memory_usage = 100000001 MAX 110000000, max_ast_depth = 2000\n"
    )
    assert (
        instance.query("SHOW CREATE ROLE worker")
        == "CREATE ROLE worker SETTINGS PROFILE `xyz`\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "100000001\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_ast_depth'",
            user="robin",
        )
        == "2000\n"
    )
    assert (
        "Setting max_memory_usage shouldn't be greater than 110000000"
        in instance.query_and_get_error(
            "SET max_memory_usage = 120000000", user="robin"
        )
    )
    assert system_settings_profile("xyz") == [
        ["xyz", "local_directory", 2, 0, "[]", "[]"]
    ]
    assert system_settings_profile_elements(profile_name="xyz") == [
        [
            "xyz",
            "\\N",
            "\\N",
            0,
            "max_memory_usage",
            100000001,
            "\\N",
            110000000,
            "\\N",
            "\\N",
        ],
        [
            "xyz",
            "\\N",
            "\\N",
            1,
            "max_ast_depth",
            2000,
            "\\N",
            "\\N",
            "\\N",
            "\\N",
        ],
    ]
    assert system_settings_profile_elements(role_name="worker") == [
        ["\\N", "\\N", "worker", 0, "\\N", "\\N", "\\N", "\\N", "\\N", "xyz"]
    ]

    instance.query("REVOKE worker FROM robin")
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "0\n"
    )
    instance.query("SET max_memory_usage = 120000000", user="robin")

    instance.query("ALTER ROLE worker SETTINGS NONE")
    instance.query("GRANT worker TO robin")
    assert instance.query("SHOW CREATE ROLE worker") == "CREATE ROLE worker\n"
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "0\n"
    )
    instance.query("SET max_memory_usage = 120000000", user="robin")
    assert system_settings_profile_elements(role_name="worker") == []

    # Set settings and constraints via CREATE SETTINGS PROFILE ... TO granted role
    instance.query("ALTER SETTINGS PROFILE xyz TO worker")
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE xyz")
        == "CREATE SETTINGS PROFILE `xyz` SETTINGS max_memory_usage = 100000001 MAX 110000000, max_ast_depth = 2000 TO worker\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "100000001\n"
    )
    assert (
        "Setting max_memory_usage shouldn't be greater than 110000000"
        in instance.query_and_get_error(
            "SET max_memory_usage = 120000000", user="robin"
        )
    )
    assert system_settings_profile("xyz") == [
        ["xyz", "local_directory", 2, 0, "['worker']", "[]"]
    ]

    instance.query("ALTER SETTINGS PROFILE xyz TO NONE")
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE xyz")
        == "CREATE SETTINGS PROFILE `xyz` SETTINGS max_memory_usage = 100000001 MAX 110000000, max_ast_depth = 2000\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "0\n"
    )
    instance.query("SET max_memory_usage = 120000000", user="robin")
    assert system_settings_profile("xyz") == [
        ["xyz", "local_directory", 2, 0, "[]", "[]"]
    ]


def test_inheritance():
    instance.query(
        "CREATE SETTINGS PROFILE xyz SETTINGS max_memory_usage = 100000002 CONST"
    )
    instance.query("CREATE SETTINGS PROFILE alpha SETTINGS PROFILE xyz TO robin")
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE xyz")
        == "CREATE SETTINGS PROFILE `xyz` SETTINGS max_memory_usage = 100000002 CONST\n"
    )
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE alpha")
        == "CREATE SETTINGS PROFILE `alpha` SETTINGS INHERIT `xyz` TO robin\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "100000002\n"
    )
    assert (
        "Setting max_memory_usage should not be changed"
        in instance.query_and_get_error("SET max_memory_usage = 80000000", user="robin")
    )

    assert system_settings_profile("xyz") == [
        ["xyz", "local_directory", 1, 0, "[]", "[]"]
    ]
    assert system_settings_profile_elements(profile_name="xyz") == [
        [
            "xyz",
            "\\N",
            "\\N",
            0,
            "max_memory_usage",
            100000002,
            "\\N",
            "\\N",
            "CONST",
            "\\N",
        ]
    ]
    assert system_settings_profile("alpha") == [
        ["alpha", "local_directory", 1, 0, "['robin']", "[]"]
    ]
    assert system_settings_profile_elements(profile_name="alpha") == [
        ["alpha", "\\N", "\\N", 0, "\\N", "\\N", "\\N", "\\N", "\\N", "xyz"]
    ]
    assert system_settings_profile_elements(user_name="robin") == []


def test_alter_and_drop():
    instance.query(
        "CREATE SETTINGS PROFILE xyz SETTINGS max_memory_usage = 100000003 MIN 90000000 MAX 110000000 TO robin"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "100000003\n"
    )
    assert (
        "Setting max_memory_usage shouldn't be less than 90000000"
        in instance.query_and_get_error("SET max_memory_usage = 80000000", user="robin")
    )
    assert (
        "Setting max_memory_usage shouldn't be greater than 110000000"
        in instance.query_and_get_error(
            "SET max_memory_usage = 120000000", user="robin"
        )
    )

    instance.query("ALTER SETTINGS PROFILE xyz SETTINGS readonly=1")
    assert (
        "Cannot modify 'max_memory_usage' setting in readonly mode"
        in instance.query_and_get_error("SET max_memory_usage = 80000000", user="robin")
    )

    instance.query("DROP SETTINGS PROFILE xyz")
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "0\n"
    )
    instance.query("SET max_memory_usage = 80000000", user="robin")
    instance.query("SET max_memory_usage = 120000000", user="robin")


def test_changeable_in_readonly():
    instance.query(
        "CREATE SETTINGS PROFILE xyz SETTINGS max_memory_usage = 100000003 MIN 90000000 MAX 110000000 CHANGEABLE_IN_READONLY SETTINGS readonly = 1 TO robin"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'max_memory_usage'",
            user="robin",
        )
        == "100000003\n"
    )
    assert (
        instance.query(
            "SELECT value FROM system.settings WHERE name = 'readonly'",
            user="robin",
        )
        == "1\n"
    )
    assert (
        "Setting max_memory_usage shouldn't be less than 90000000"
        in instance.query_and_get_error("SET max_memory_usage = 80000000", user="robin")
    )
    assert (
        "Setting max_memory_usage shouldn't be greater than 110000000"
        in instance.query_and_get_error(
            "SET max_memory_usage = 120000000", user="robin"
        )
    )

    assert system_settings_profile_elements(profile_name="xyz") == [
        [
            "xyz",
            "\\N",
            "\\N",
            0,
            "max_memory_usage",
            100000003,
            90000000,
            110000000,
            "CHANGEABLE_IN_READONLY",
            "\\N",
        ],
        [
            "xyz",
            "\\N",
            "\\N",
            1,
            "readonly",
            1,
            "\\N",
            "\\N",
            "\\N",
            "\\N",
        ],
    ]

    instance.query("SET max_memory_usage = 90000000", user="robin")
    instance.query("SET max_memory_usage = 110000000", user="robin")


def test_show_profiles():
    instance.query("CREATE SETTINGS PROFILE xyz")
    assert instance.query("SHOW SETTINGS PROFILES") == "default\nreadonly\nxyz\n"
    assert instance.query("SHOW PROFILES") == "default\nreadonly\nxyz\n"

    assert (
        instance.query("SHOW CREATE PROFILE xyz") == "CREATE SETTINGS PROFILE `xyz`\n"
    )

    query_expected_response = [
        "CREATE SETTINGS PROFILE `default`\n",
    ]
    assert (
        instance.query("SHOW CREATE SETTINGS PROFILE default")
        in query_expected_response
    )

    query_expected_response = [
        "CREATE SETTINGS PROFILE `default`\n"
        "CREATE SETTINGS PROFILE `readonly` SETTINGS readonly = 1\n"
        "CREATE SETTINGS PROFILE `xyz`\n",
    ]
    assert instance.query("SHOW CREATE PROFILES") in query_expected_response

    expected_access = (
        "CREATE SETTINGS PROFILE `default`\n"
        "CREATE SETTINGS PROFILE `readonly` SETTINGS readonly = 1\n"
        "CREATE SETTINGS PROFILE `xyz`\n"
    )

    query_response = instance.query("SHOW ACCESS")
    assert expected_access in query_response


def test_set_profile():
    instance.query(
        "CREATE SETTINGS PROFILE P1 SETTINGS max_memory_usage=10000000001 MAX 20000000002"
    )

    session_id = new_session_id()
    instance.http_query(
        "SET profile='P1'", user="robin", params={"session_id": session_id}
    )
    assert (
        instance.http_query(
            "SELECT getSetting('max_memory_usage')",
            user="robin",
            params={"session_id": session_id},
        )
        == "10000000001\n"
    )

    expected_error = "max_memory_usage shouldn't be greater than 20000000002"
    assert expected_error in instance.http_query_and_get_error(
        "SET max_memory_usage=20000000003",
        user="robin",
        params={"session_id": session_id},
    )


def test_changing_default_profiles_affects_new_sessions_only():
    instance.query("CREATE SETTINGS PROFILE P1 SETTINGS max_memory_usage=10000000001")
    instance.query("CREATE SETTINGS PROFILE P2 SETTINGS max_memory_usage=10000000002")
    instance.query("ALTER USER robin SETTINGS PROFILE P1")

    session_id = new_session_id()
    assert (
        instance.http_query(
            "SELECT getSetting('max_memory_usage')",
            user="robin",
            params={"session_id": session_id},
        )
        == "10000000001\n"
    )
    instance.query("ALTER USER robin SETTINGS PROFILE P2")
    assert (
        instance.http_query(
            "SELECT getSetting('max_memory_usage')",
            user="robin",
            params={"session_id": session_id},
        )
        == "10000000001\n"
    )

    other_session_id = new_session_id()
    assert (
        instance.http_query(
            "SELECT getSetting('max_memory_usage')",
            user="robin",
            params={"session_id": other_session_id},
        )
        == "10000000002\n"
    )


def test_function_current_profiles():
    instance.query("CREATE SETTINGS PROFILE P1, P2")
    instance.query("ALTER USER robin SETTINGS PROFILE P1, P2")
    instance.query("CREATE SETTINGS PROFILE P3 TO robin")
    instance.query("CREATE SETTINGS PROFILE P4")
    instance.query("CREATE SETTINGS PROFILE P5 SETTINGS INHERIT P4")
    instance.query("CREATE ROLE worker SETTINGS PROFILE P5")
    instance.query("GRANT worker TO robin")
    instance.query("CREATE SETTINGS PROFILE P6")

    session_id = new_session_id()
    assert (
        instance.http_query(
            "SELECT defaultProfiles(), currentProfiles(), enabledProfiles()",
            user="robin",
            params={"session_id": session_id},
        )
        == "['P1','P2']\t['default','P3','P5','P1','P2']\t['default','P3','P4','P5','P1','P2']\n"
    )

    instance.http_query(
        "SET profile='P6'", user="robin", params={"session_id": session_id}
    )
    assert (
        instance.http_query(
            "SELECT defaultProfiles(), currentProfiles(), enabledProfiles()",
            user="robin",
            params={"session_id": session_id},
        )
        == "['P1','P2']\t['P6']\t['default','P3','P4','P5','P1','P2','P6']\n"
    )

    instance.http_query(
        "SET profile='P5'", user="robin", params={"session_id": session_id}
    )
    assert (
        instance.http_query(
            "SELECT defaultProfiles(), currentProfiles(), enabledProfiles()",
            user="robin",
            params={"session_id": session_id},
        )
        == "['P1','P2']\t['P5']\t['default','P3','P1','P2','P6','P4','P5']\n"
    )

    instance.query("ALTER USER robin SETTINGS PROFILE P2")
    assert (
        instance.http_query(
            "SELECT defaultProfiles(), currentProfiles(), enabledProfiles()",
            user="robin",
            params={"session_id": session_id},
        )
        == "['P2']\t['P5']\t['default','P3','P1','P2','P6','P4','P5']\n"
    )


def test_allow_ddl():
    assert "it's necessary to have the grant" in instance.query_and_get_error(
        "CREATE TABLE tbl(a Int32) ENGINE=Log", user="robin"
    )
    assert "it's necessary to have the grant" in instance.query_and_get_error(
        "GRANT CREATE ON tbl TO robin", user="robin"
    )
    assert "DDL queries are prohibited" in instance.query_and_get_error(
        "CREATE TABLE tbl(a Int32) ENGINE=Log", settings={"allow_ddl": 0}
    )

    instance.query("GRANT CREATE ON tbl TO robin")
    instance.query("GRANT TABLE ENGINE ON Log TO robin")
    instance.query("CREATE TABLE tbl(a Int32) ENGINE=Log", user="robin")
    instance.query("DROP TABLE tbl")


def test_allow_introspection():
    assert (
        instance.query(
            "SELECT demangle('a')", settings={"allow_introspection_functions": 1}
        )
        == "signed char\n"
    )

    assert "Introspection functions are disabled" in instance.query_and_get_error(
        "SELECT demangle('a')"
    )
    assert "it's necessary to have the grant" in instance.query_and_get_error(
        "SELECT demangle('a')", user="robin"
    )
    assert "it's necessary to have the grant" in instance.query_and_get_error(
        "SELECT demangle('a')",
        user="robin",
        settings={"allow_introspection_functions": 1},
    )

    instance.query("GRANT demangle ON *.* TO robin")
    assert "Introspection functions are disabled" in instance.query_and_get_error(
        "SELECT demangle('a')", user="robin"
    )
    assert (
        instance.query(
            "SELECT demangle('a')",
            user="robin",
            settings={"allow_introspection_functions": 1},
        )
        == "signed char\n"
    )

    instance.query("ALTER USER robin SETTINGS allow_introspection_functions=1")
    assert instance.query("SELECT demangle('a')", user="robin") == "signed char\n"

    instance.query("ALTER USER robin SETTINGS NONE")
    assert "Introspection functions are disabled" in instance.query_and_get_error(
        "SELECT demangle('a')", user="robin"
    )

    instance.query(
        "CREATE SETTINGS PROFILE xyz SETTINGS allow_introspection_functions=1 TO robin"
    )
    assert instance.query("SELECT demangle('a')", user="robin") == "signed char\n"

    instance.query("DROP SETTINGS PROFILE xyz")
    assert "Introspection functions are disabled" in instance.query_and_get_error(
        "SELECT demangle('a')", user="robin"
    )

    instance.query(
        "REVOKE demangle ON *.* FROM robin",
        settings={"allow_introspection_functions": 1},
    )
    assert "it's necessary to have the grant" in instance.query_and_get_error(
        "SELECT demangle('a')", user="robin"
    )


def test_settings_aliases():
    instance.query(
        "CREATE SETTINGS PROFILE P1 SETTINGS replication_alter_partitions_sync=2"
    )
    instance.query(
        "CREATE SETTINGS PROFILE P2 SETTINGS replication_alter_partitions_sync=0"
    )
    instance.query("ALTER USER robin SETTINGS PROFILE P1")

    assert (
        instance.http_query(
            "SELECT getSetting('alter_sync')",
            user="robin",
        )
        == "2\n"
    )

    instance.query("ALTER USER robin SETTINGS PROFILE P2")

    assert (
        instance.http_query(
            "SELECT getSetting('alter_sync')",
            user="robin",
        )
        == "0\n"
    )
