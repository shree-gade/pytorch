from torch._C._distributed_c10d import _DEFAULT_PG_TIMEOUT
import datetime
from datetime import timedelta
# Default process group wide timeout, if applicable.
# This only applies to the gloo and nccl backends
# (only if NCCL_BLOCKING_WAIT or NCCL_ASYNC_ERROR_HANDLING is set to 1).
# To make an attempt at backwards compatibility with THD, we use an
# extraordinarily high default timeout, given that THD did not have timeouts.
default_pg_timeout = _DEFAULT_PG_TIMEOUT + datetime.timedelta(0,18000)
