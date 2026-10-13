"""CLI coverage for always-active construction-time configuration errors."""

import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("CHAMPSIM_BINARY", ROOT / "bin/champsim")).resolve()


@unittest.skipUnless(BINARY.is_file(), "set CHAMPSIM_BINARY to exercise configuration errors")
class OperationalConfigErrorTests(unittest.TestCase):
    def run_knobs(self, *settings):
        command = [str(BINARY), "--knobs"]
        for setting in settings:
            command.extend(["--set", setting])
        return subprocess.run(command, text=True, capture_output=True, timeout=30)

    def test_unrepresentable_aggregate_dram_capacity_is_a_cli_error(self):
        result = self.run_knobs("dram-model=legacy", "pmem.bank_rows=4611686018427387904")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("ERROR:", result.stderr)
        self.assertIn("DRAM address mapping capacity is not representable", result.stderr)

    def test_binding_boundaries_are_valid(self):
        result = self.run_knobs(
            "dram-model=legacy",
            "vmem.pte_page_size=2048",
            "ooo_cpu.cpu0.register_file_size=32767",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("vmem.pte_page_size = 2048", result.stdout)
        self.assertIn("ooo_cpu.cpu0.register_file_size = 32767", result.stdout)

    def test_rob_partition_rejects_unimplemented_value(self):
        result = self.run_knobs("ooo_cpu.cpu0.rob_partition=dynamic")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("ERROR:", result.stderr)
        self.assertIn("rob_partition must be 'equal'", result.stderr)


if __name__ == "__main__":
    unittest.main()
