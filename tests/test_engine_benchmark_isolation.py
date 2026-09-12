"""Exercise the actual benchmark link adapter without building or running a game."""
import ast
from pathlib import Path
import shlex
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock

SOURCE = Path(__file__).parent/'integration/build_engine_benchmark.py'


class EngineBenchmarkIsolation(unittest.TestCase):
    def test_every_object_destination_is_benchmark_owned(self):
        source = SOURCE.read_text()
        tree = ast.parse(source)
        collect = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'collect')
        destinations = [n.value for n in ast.walk(collect) if isinstance(n, ast.Assign)
                        and any(isinstance(t, ast.Name) and t.id == 'obj' for t in n.targets)]
        self.assertGreaterEqual(len(destinations), 1)
        for destination in destinations:
            self.assertIn('OUTPUT', ast.unparse(destination))
        self.assertNotIn('original_objects', source)

    def test_only_owned_final_link_uses_lossless_response_file(self):
        tree = ast.parse(SOURCE.read_text())
        function = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'run_benchmark_command')
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)/'benchmark'
            output.mkdir()
            run = Mock(return_value='completed')
            scope = dict(Path=Path, OUTPUT=output, build=SimpleNamespace(ZIG='zig.exe'),
                         original_subprocess_run=run)
            exec(compile(ast.Module(body=[function], type_ignores=[]), str(SOURCE), 'exec'), scope)
            call = scope['run_benchmark_command']
            command = ['zig.exe', 'c++', r'C:\a folder\file.o', 'quote"name.o',
                       '-o', str(output/'release/mp6native.exe')]
            self.assertEqual(call(command, capture_output=True, text=True), 'completed')
            response = output/'link.rsp'
            self.assertEqual(shlex.split(response.read_text()), command[2:])
            run.assert_called_once_with(['zig.exe', 'c++', '@'+str(response)], capture_output=True, text=True)
            saved = response.read_bytes()
            for other in ([*command[:2], '-c', *command[2:]],
                          [*command[:-1], str(Path(temp)/'release/mp6native.exe')],
                          ['unrelated.exe', *command[1:]], ['zig.exe', 'version']):
                run.reset_mock()
                call(other, check=True)
                run.assert_called_once_with(other, check=True)
                self.assertEqual(response.read_bytes(), saved)


if __name__ == '__main__':
    unittest.main()
