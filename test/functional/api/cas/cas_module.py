#
# Copyright(c) 2019-2022 Intel Corporation
# Copyright(c) 2024 Huawei Technologies Co., Ltd.
# SPDX-License-Identifier: BSD-3-Clause
#

from enum import Enum
from core.test_run import TestRun
from test_utils import os_utils
from test_utils.os_utils import ModuleRemoveMethod


class CasModule(Enum):
    cache = "cas_cache"


def reload_all_cas_modules():
    os_utils.unload_kernel_module(CasModule.cache.value, ModuleRemoveMethod.modprobe)
    os_utils.load_kernel_module(CasModule.cache.value)


def unload_all_cas_modules():
    os_utils.unload_kernel_module(CasModule.cache.value, os_utils.ModuleRemoveMethod.rmmod)


def is_cas_management_dev_present():
    return TestRun.executor.run("test -c /dev/cas_ctrl").exit_code == 0
