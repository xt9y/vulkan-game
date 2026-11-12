#!/usr/bin/env python3
"""Build the protocol regression executable using the project's include paths."""
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
entry = json.loads((root / 'compile_commands.json').read_text())[0]
includes = [arg for arg in entry['arguments'] if arg.startswith('-I')]
binary = root / 'build/protocol-test'
subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined',
                '-ffunction-sections', '-fdata-sections',
                '-Wno-missing-field-initializers', *includes, 'tests/protocol.cpp',
                'third-party/vulkan-systems/network/network/endpoint.cpp',
                '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                '-o', str(binary)], cwd=root, check=True)
subprocess.run([str(binary)], cwd=root, check=True)
