import gdbremote_testcase
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *


class TestGdbRemoteThreadExtendedInfo(gdbremote_testcase.GdbRemoteTestCaseBase):
    def start_inferior(self):
        self.build()
        self.set_inferior_startup_launch()
        self.prep_debug_monitor_and_inferior()

    # debugserver answers "OK" to both of these, so only exercise lldb-server.
    @add_test_categories(["llgs"])
    @skipIfWindows
    def test_jThreadExtendedInfo_unsupported(self):
        """A "jThreadExtendedInfo:" query replies with an empty packet when no
        thread has extended information to supply."""
        self.start_inferior()
        self.test_sequence.add_log_lines(
            [
                "read packet: $jThreadExtendedInfo:#b9",
                "send packet: $#00",
            ],
            True,
        )
        self.expect_gdbremote_sequence()

    # debugserver answers "OK" to both of these, so only exercise lldb-server.
    @add_test_categories(["llgs"])
    @skipIfWindows
    def test_jThreadExtendedInfo_malformed_arguments(self):
        """Arguments that aren't a JSON dictionary are rejected with an error.
        This also proves the packet reaches its handler rather than falling
        through to the generic unimplemented packet response."""
        self.start_inferior()
        self.test_sequence.add_log_lines(
            [
                "read packet: $jThreadExtendedInfo:not-json#00",
                {
                    "direction": "send",
                    "regex": r"^\$E[0-9a-fA-F]{2}",
                },
            ],
            True,
        )
        self.expect_gdbremote_sequence()
