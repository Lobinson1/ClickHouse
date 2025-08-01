import argparse
import atexit
import logging
import mmap
import os
import pathlib
import random
import tempfile
import time
import signal
import sys

sys.path.append("..")
from integration.helpers.cluster import is_port_free, ZOOKEEPER_CONTAINERS


# Needs to get free ports before importing ClickHouseCluster
def get_unique_free_ports(total):
    ports = []
    for port in range(30000, 55000):
        if is_port_free(port) and port not in ports:
            ports.append(port)

        if len(ports) == total:
            return ports

    raise Exception(f"Can't collect {total} ports. Collected: {len(ports)}")


os.environ["WORKER_FREE_PORTS"] = " ".join([str(p) for p in get_unique_free_ports(50)])

from environment import set_environment_variables
from integration.helpers.cluster import ClickHouseCluster, ClickHouseInstance
from integration.helpers.postgres_utility import get_postgres_conn
from generators import Generator, BuzzHouseGenerator
from oracles import ElOraculoDeTablas
from properties import (
    modify_server_settings,
    modify_user_settings,
    modify_keeper_settings,
)


def ordered_pair(value):
    try:
        x, y = map(int, value.split(","))
        if x > y:
            raise argparse.ArgumentTypeError(
                f"First value must be less than second (got {x} > {y})"
            )
        return (x, y)
    except ValueError:
        raise argparse.ArgumentTypeError(
            "Must be two comma-separated integers (e.g., '1,10')"
        )


def list_of_values(arg):
    return arg.split(",")


parser = argparse.ArgumentParser()
parser.add_argument(
    "--server-settings-prob",
    type=int,
    default=80,
    choices=range(0, 101),
    help="Probability to set server properties",
)
parser.add_argument(
    "--add-disk-settings-prob",
    type=int,
    default=80,
    choices=range(0, 101),
    help="Probability to set random disks",
)
parser.add_argument(
    "--number-disks",
    type=ordered_pair,
    default=(1, 5),
    help="Number of disks to generate. Two ordered integers separated by comma (e.g., 1,3)",
)
parser.add_argument(
    "--add-policy-settings-prob",
    type=int,
    default=70,
    choices=range(0, 101),
    help="Probability to set random storage policies",
)
parser.add_argument(
    "--add-remote-server-settings-prob",
    type=int,
    default=80,
    choices=range(0, 101),
    help="Probability to set random servers",
)
parser.add_argument(
    "--number-servers",
    type=ordered_pair,
    default=(1, 3),
    help="Number of servers to generate. Two ordered integers separated by comma (e.g., 1,3)",
)
parser.add_argument(
    "--add-filesystem-caches-prob",
    type=int,
    default=80,
    choices=range(0, 101),
    help="Probability to add filesystem caches",
)
parser.add_argument(
    "--number-caches",
    type=ordered_pair,
    default=(1, 3),
    help="Number of filesystem caches to generate. Two ordered integers separated by comma (e.g., 1,3)",
)
parser.add_argument(
    "--change-server-version-prob",
    type=int,
    default=80,
    choices=range(0, 101),
    help="Probability to change server version after restart",
)
parser.add_argument(
    "--client-binary", type=pathlib.Path, required=True, help="Path to client binary"
)
parser.add_argument(
    "--server-binaries",
    type=list_of_values,
    required=True,
    help="Path of server binaries to test",
)
parser.add_argument(
    "-c", "--client-config", type=pathlib.Path, help="Path to client configuration file"
)
parser.add_argument(
    "-g",
    "--generator",
    choices=["buzzhouse"],
    type=str.lower,
    required=True,
    help="What generator to use",
)
parser.add_argument(
    "-l",
    "--log-path",
    type=pathlib.Path,
    default=tempfile.NamedTemporaryFile(suffix=".log"),
    help="Log path",
)
parser.add_argument(
    "--replica-values",
    type=list_of_values,
    default="1",
    help="Comma separated list for replica values",
)
parser.add_argument(
    "--shard-values",
    type=list_of_values,
    default="1",
    help="Comma separated list for shard values",
)
parser.add_argument(
    "--server-config", type=pathlib.Path, help="Path to config.xml file"
)
parser.add_argument("-s", "--seed", type=int, default=0, help="Server fuzzer seed")
parser.add_argument(
    "-u", "--user-config", type=pathlib.Path, help="Path to users.xml file"
)
parser.add_argument(
    "--kill-server-prob",
    type=int,
    default=50,
    choices=range(0, 101),
    help="Probability to kill the server instead of shutting it down",
)
parser.add_argument(
    "--restart-clickhouse-prob",
    type=int,
    default=50,
    choices=range(0, 101),
    help="Probability to restart ClickHouse instead of integration servers",
)
parser.add_argument(
    "--time-between-shutdowns",
    type=ordered_pair,
    default=(20, 30),
    help="In seconds. Two ordered integers separated by comma (e.g., 30,60)",
)
parser.add_argument(
    "--time-between-integration-shutdowns",
    type=ordered_pair,
    default=(3, 5),
    help="In seconds. Two ordered integers separated by comma (e.g., 3,5)",
)
parser.add_argument(
    "--with-postgresql", action="store_true", help="With PostgreSQL integration"
)
parser.add_argument("--with-mysql", action="store_true", help="With MySQL integration")
parser.add_argument(
    "--without-minio",
    action="store_false",
    dest="with_minio",
    help="Without MinIO integration",
)
parser.add_argument(
    "--without-zookeeper",
    action="store_false",
    dest="with_zookeeper",
    help="Without Zookeeper server",
)
parser.add_argument("--with-nginx", action="store_true", help="With Nginx integration")
parser.add_argument(
    "--with-azurite", action="store_true", help="With Azure integration"
)
parser.add_argument(
    "--with-sqlite", action="store_true", help="With SQLite integration"
)
parser.add_argument(
    "--with-mongodb", action="store_true", help="With MongoDB integration"
)
parser.add_argument("--with-redis", action="store_true", help="With Redis integration")
parser.add_argument(
    "--mem-limit", type=str, default="", help="Set a memory limit, e.g. '1g'"
)
parser.add_argument(
    "--without-keeper-map-prefix",
    action="store_false",
    dest="add_keeper_map_prefix",
    help="Add 'keeper_map_path_prefix' server setting",
)
parser.add_argument(
    "--without-transactions",
    action="store_false",
    dest="add_transactions",
    help="Add 'allow_experimental_transactions' server setting",
)
parser.add_argument(
    "--without-distributed-ddl",
    action="store_false",
    dest="add_distributed_ddl",
    help="Add 'distributed_ddl' settings",
)
parser.add_argument(
    "--without-shared-catalog",
    action="store_false",
    dest="add_shared_catalog",
    help="Add 'shared_database_catalog' settings",
)
parser.add_argument(
    "--compare-table-dump-prob",
    type=int,
    default=50,
    choices=range(0, 101),
    help="Probability to compare contents of a table after a server restart",
)
parser.add_argument(
    "--set-locales-prob",
    type=int,
    default=50,
    choices=range(0, 101),
    help="Probability to send a random locale to all instances in a cluster",
)
parser.add_argument(
    "--set-timezones-prob",
    type=int,
    default=50,
    choices=range(0, 101),
    help="Probability to send a random timezone to all instances in a cluster",
)
parser.add_argument(
    "--keeper-settings-prob",
    type=int,
    default=80,
    choices=range(0, 101),
    help="Probability to set keeper server properties",
)

args = parser.parse_args()

if len(args.replica_values) != len(args.shard_values):
    raise Exception(
        f"The length of replica values {len(args.replica_values)} is not the same as shard values {len(args.shard_values)}"
    )

logging.basicConfig(
    filename=args.log_path,
    filemode="w",
    level=logging.INFO,
    format="%(asctime)s - %(name)s - %(levelname)s - %(message)s",
    force=True,
)
logger = logging.getLogger(__name__)

# Set seed first
seed = args.seed
if seed == 0:
    import secrets

    seed = secrets.randbits(64)  # 64 - bit random integer
random.seed(seed)
logger.info(f"Using seed: {seed}")

# Start the cluster, by using one the server binaries
server_path = os.path.join(tempfile.gettempdir(), f"clickhouse{seed}")
new_temp_server_path = os.path.join(tempfile.gettempdir(), f"clickhousetemp{seed}")
try:
    os.unlink(server_path)
except FileNotFoundError:
    pass
current_server = random.choice(args.server_binaries)
os.symlink(current_server, server_path)
os.environ["CLICKHOUSE_TESTS_SERVER_BIN_PATH"] = server_path

# Find if private binary is being used
is_private_binary = False
with open(current_server, "r+") as f:
    mm = mmap.mmap(f.fileno(), 0)
    is_private_binary = mm.find(b"LRU_OVERCOMMIT") > -1
    mm.close()

logger.info(f"Private binary {"" if is_private_binary else "not "}detected")
keeper_configs: list[str] = modify_keeper_settings(args, is_private_binary)
cluster = ClickHouseCluster(__file__, custom_keeper_configs=keeper_configs)

# Set environment variables such as locales and timezones
test_env_variables = set_environment_variables(logger, args, "cluster")

# Use random server settings sometimes
server_settings = args.server_config
user_settings = args.user_config
modified_server_settings = modified_user_settings = False
generated_clusters = 0
if server_settings is not None:
    modified_server_settings, server_settings, generated_clusters = (
        modify_server_settings(args, cluster, is_private_binary, server_settings)
    )
    if generated_clusters > 0:
        modified_user_settings, user_settings = modify_user_settings(
            user_settings, generated_clusters
        )

dolor_main_configs = [
    "../config/server.crt",
    "../config/server.key",
    "../config/server-cert.pem",
    "../config/server-key.pem",
    "../config/ca-cert.pem",
    "../config/dhparam.pem",
]
if server_settings is not None:
    dolor_main_configs.append(server_settings)


servers: list[ClickHouseInstance] = []
for i in range(0, len(args.replica_values)):
    servers.append(
        cluster.add_instance(
            f"node{i}",
            with_dolor=True,
            stay_alive=True,
            copy_common_configs=False,
            with_zookeeper=args.with_zookeeper,
            with_minio=args.with_minio,
            with_nginx=args.with_nginx,
            with_azurite=args.with_azurite,
            with_postgres=args.with_postgresql,
            with_mysql8=args.with_mysql,
            with_mongo=args.with_mongodb,
            with_redis=args.with_redis,
            mem_limit=None if args.mem_limit == "" else args.mem_limit,
            main_configs=dolor_main_configs,
            user_configs=[user_settings] if user_settings is not None else [],
            env_variables=test_env_variables,
            macros={"replica": args.replica_values[i], "shard": args.shard_values[i]},
        )
    )
cluster.start()
logger.info(
    f"Starting cluster with {len(servers)} server(s) and server binary {current_server} "
)
for i in range(0, len(args.replica_values)):
    logger.info(f"Server node{i} running on host {servers[i].ip_address}, port 9000")
servers[len(servers) - 1].wait_start(8)

if args.with_postgresql:
    postgres_conn = get_postgres_conn(
        ip=cluster.postgres_ip, port=cluster.postgres_port
    )
    cursor = postgres_conn.cursor()
    cursor.execute(f"CREATE DATABASE test")
    cursor.close()
    postgres_conn.close()

# Start the load generator, at the moment only BuzzHouse is available
generator: Generator = Generator(pathlib.Path(), pathlib.Path(), None)
if args.generator == "buzzhouse":
    generator = BuzzHouseGenerator(args, cluster)
logger.info("Start load generator")
client = generator.run_generator(servers[0], logger, args)


def dolor_cleanup():
    if client.process.poll() is None:
        client.process.kill()
        client.process.wait()
    try:
        cluster.shutdown()
    except:
        pass
    if modified_server_settings:
        try:
            os.unlink(server_settings)
        except FileNotFoundError:
            pass
    if modified_user_settings:
        try:
            os.unlink(user_settings)
        except FileNotFoundError:
            pass
    try:
        os.unlink(server_path)
    except FileNotFoundError:
        pass
    try:
        os.unlink(new_temp_server_path)
    except FileNotFoundError:
        pass
    try:
        os.unlink(generator.temp.name)
    except FileNotFoundError:
        pass
    for entry in keeper_configs:
        try:
            os.unlink(entry)
        except FileNotFoundError:
            pass


def my_signal_handler(sig, frame):
    dolor_cleanup()
    sys.exit(0)


signal.signal(signal.SIGINT, my_signal_handler)
atexit.register(dolor_cleanup)
time.sleep(3)

integrations = []
if args.with_zookeeper:
    integrations.extend(["zookeeper", "zookeeper"])  # Increased probability
if args.with_minio:
    integrations.append("minio")
if args.with_nginx:
    integrations.append("nginx")
if args.with_azurite:
    integrations.append("azurite")
if args.with_postgresql:
    integrations.append("postgres")
if args.with_mysql:
    integrations.append("mysql8")
if args.with_mongodb:
    integrations.append("mongo")
if args.with_redis:
    integrations.append("redis")

# This is the main loop, run while client and server are running
all_running = True
tables_oracle: ElOraculoDeTablas = ElOraculoDeTablas()
lower_bound, upper_bound = args.time_between_shutdowns
integration_lower_bound, integration_upper_bound = (
    args.time_between_integration_shutdowns
)
while all_running:
    start = time.time()
    finish = start + random.randint(lower_bound, upper_bound)

    while all_running and start < finish:
        interval = 1
        if client.process.poll() is not None:
            logger.info("Load generator finished")
            all_running = False
        for server in servers:
            try:
                server.query("SELECT 1;")
            except:
                logger.info(f"The server {server.name} is not running")
                all_running = False
        time.sleep(interval)
        start += interval

    if not all_running:
        break

    dump_table = (
        tables_oracle.collect_table_hash_before_shutdown(cluster, logger)
        if random.randint(1, 100) <= args.compare_table_dump_prob
        else None
    )
    kill_server = random.randint(1, 100) <= args.kill_server_prob
    # Pick one of the servers to restart
    # Restart ClickHouse
    if random.randint(1, 100) <= args.restart_clickhouse_prob:
        next_pick = random.choice(servers)
        logger.info(
            f"Restarting the server {next_pick.name} with {"kill" if kill_server else "manual shutdown"}"
        )

        next_pick.stop_clickhouse(stop_wait_sec=10, kill=kill_server)
        # Replace server binary, using a new temporary symlink, then replace the old one
        if (
            len(args.server_binaries) > 1
            and random.randint(1, 100) <= args.change_server_version_prob
        ):
            if len(servers) == 1 and len(args.server_binaries) == 2:
                current_server = (
                    args.server_binaries[0]
                    if current_server == args.server_binaries[1]
                    else args.server_binaries[1]
                )
            else:
                current_server = random.choice(args.server_binaries)
            logger.info(f"Using the server binary {current_server} after restart")
            try:
                os.unlink(new_temp_server_path)
            except FileNotFoundError:
                pass
            os.symlink(current_server, new_temp_server_path)
            os.rename(new_temp_server_path, server_path)
        time.sleep(15)  # Let the zookeeper session expire
        next_pick.start_clickhouse(start_wait_sec=10, retry_start=False)
    elif len(integrations) > 0:
        # Restart any other integration
        next_pick = random.choice(integrations)
        choosen_instances = []
        available_options = {
            "zookeeper": list(ZOOKEEPER_CONTAINERS),
            "minio": ["minio1"],
            "nginx": ["nginx"],
            "azurite": ["azurite1"],
            "postgres": ["postgres1"],
            "mysql8": ["mysql80"],
            "mongo": ["mongo1", "mongo_no_cred", "mongo_secure"],
            "redis": ["redis1"],
        }

        restart_choices = list(available_options[next_pick])
        random.shuffle(restart_choices)
        for i in range(0, random.randint(1, len(restart_choices))):
            choosen_instances.append(restart_choices[i])
        logger.info(
            f"Restarting {next_pick} instances {', '.join(choosen_instances)} with {"kill" if kill_server else "manual shutdown"}"
        )

        cluster.process_integration_nodes(
            next_pick, choosen_instances, "kill" if kill_server else "stop"
        )
        time.sleep(random.randint(integration_lower_bound, integration_upper_bound))
        cluster.process_integration_nodes(next_pick, choosen_instances, "start")

    tables_oracle.collect_table_hash_after_shutdown(cluster, logger, dump_table)
