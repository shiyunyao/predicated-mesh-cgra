import pathlib
import tempfile
import unittest

from tools.cgra_bench.run_rf_closure_ablation import CONFIGS, command_for


class RFClosureAblationRunnerTest(unittest.TestCase):
    def test_configs_only_change_the_two_rf_switches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build = root / "build"
            output = root / "out"
            commands = {
                name: command_for(root, build, output, "research", "m32", name)
                for name in CONFIGS
            }
            for name, command in commands.items():
                self.assertIn("--mapping-objective", command)
                self.assertIn("find-any-feasible", command)
                self.assertIn("--enable-recurrence-ingress", command)
                self.assertIn("--rf-port-aware", command)
                self.assertIn(CONFIGS[name]["rf_port_aware"], command)
                self.assertIn("--software-rotation", command)
                self.assertIn(CONFIGS[name]["software_rotation"], command)
            common = [item for item in commands["A_baseline"]
                      if item not in {"on", "off"}]
            self.assertTrue(common)


if __name__ == "__main__":
    unittest.main()
