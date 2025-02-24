# Copyright 2023 Redpanda Data, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

from rptest.services.cluster import cluster
from rptest.tests.redpanda_test import RedpandaTest
from rptest.services.admin import Admin
from rptest.services.redpanda import LoggingConfig
from rptest.clients.types import TopicSpec
from rptest.services.kgo_repeater_service import repeater_traffic
from rptest.utils.mode_checks import skip_debug_mode


class CPUProfilerAdminAPITest(RedpandaTest):
    topics = (TopicSpec(partition_count=30, replication_factor=3), )

    def __init__(self, test_context):
        super(CPUProfilerAdminAPITest, self).__init__(
            test_context=test_context,
            num_brokers=3,
            log_config=LoggingConfig('info',
                                     logger_levels={'resources': 'trace'}),
            extra_rp_conf={
                "cpu_profiler_enabled": True,
                "cpu_profiler_sample_period_ms": 50,
            })

        self.admin = Admin(self.redpanda)

    @cluster(num_nodes=4)
    def test_get_cpu_profile(self):
        # Provide traffic so there is something to sample.
        with repeater_traffic(context=self.test_context,
                              redpanda=self.redpanda,
                              topics=[self.topic],
                              msg_size=4096,
                              workers=1) as repeater:
            repeater.await_group_ready()
            repeater.await_progress(1024,
                                    timeout_sec=100 if self.debug_mode else 75)

            profile = self.admin.get_cpu_profile()

            assert len(profile) > 0, "At least one shard should exist"
            assert len(
                profile[0]["samples"]
            ) > 0, "At least one cpu profile should've been collected."
