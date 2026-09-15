"""Read-only structural checks; intentionally not a compiler or runtime test."""
from pathlib import Path
import ast
import re
import subprocess

root = Path(__file__).resolve().parents[1]
prod = sorted(root.glob('bgpd/midr_*.[ch]'))
tests = sorted(root.glob('tests/bgpd/test_midr_tier1*.[ch]'))
tests += sorted(root.glob('tests/bgpd/test_midr_trace*.[ch]'))
tests += [root / 'tests/bgpd/test_midr_ip2asn_update.c']
files = prod + tests
errors = []

def stripped(text):
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*[\s\S]*?\*/|//[^\n]*'
    return re.sub(pattern, lambda m: '\n' * m.group().count('\n') + ' ', text)

sources = {}
for path in files:
    text = path.read_text(encoding='utf-8')
    name = path.relative_to(root).as_posix()
    clean = stripped(text)
    sources[name] = clean
    stack = []
    pairs = {'}': '{', ')': '(', ']': '['}
    for pos, ch in enumerate(clean):
        if ch in '{([':
            stack.append((ch, pos))
        elif ch in '})]':
            if not stack or stack[-1][0] != pairs[ch]:
                errors.append(f'{name}: unmatched {ch} at line {clean[:pos].count(chr(10))+1}')
                break
            stack.pop()
    if stack:
        errors.append(f'{name}: unclosed delimiters')
    directives = []
    for line in clean.splitlines():
        m = re.match(r'\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b', line)
        if not m:
            continue
        op = m[1]
        if op in ('if', 'ifdef', 'ifndef'):
            directives.append(op)
        elif not directives:
            errors.append(f'{name}: unmatched #{op}')
        elif op == 'endif':
            directives.pop()
    if directives:
        errors.append(f'{name}: unclosed preprocessor condition')
    for inc in re.findall(r'^#include "([^"]+)"', text, re.M):
        if not any((base / inc).is_file() for base in (root, root/'lib', path.parent)):
            errors.append(f'{name}: missing local include {inc}')
    for num, line in enumerate(text.splitlines(), 1):
        if line.rstrip() != line:
            errors.append(f'{name}:{num}: trailing whitespace')

body = '\n'.join(v for k, v in sources.items() if k.startswith('bgpd/') and k.endswith('.c'))
declarations = '\n'.join(v for k,v in sources.items() if k.endswith('.h'))
definitions = set(re.findall(r'\b(midr_\w+)\s*\([^;{}]*\)\s*\{', body))
declared = set(re.findall(r'\b(midr_\w+)\s*\([^;{}]*\)\s*;', declarations))
for symbol in sorted(declared - definitions):
    errors.append(f'public function without implementation: {symbol}')
calls = set(re.findall(r'\b(midr_\w+)\s*\(', body))
for symbol in sorted(calls - definitions - declared):
    errors.append(f'unresolved MIDR function reference: {symbol}')
for forbidden in ('popen', 'pclose', 'system', 'posix_spawn', 'execvp', 'execlp', 'waitpid', 'nanosleep'):
    if re.search(r'\b' + forbidden + r'\s*\(', body):
        errors.append(f'forbidden external/blocking call: {forbidden}')
if 'midr_tier1_common_seed' in body or 'midr_trace_exec_' in body:
    errors.append('removed implementation still referenced')

manifest = (root/'bgpd/subdir.am').read_text(encoding='utf-8')
test_manifest = (root/'tests/bgpd/subdir.am').read_text(encoding='utf-8')
for path in prod:
    if path.relative_to(root).as_posix() not in manifest:
        errors.append(f'production file missing from Automake: {path.name}')
for path in tests:
    if path.relative_to(root).as_posix() not in test_manifest:
        errors.append(f'test source missing from Automake: {path.name}')
for path in [root/'tests/bgpd/test_midr_tier1.py', root/'tests/bgpd/test_midr_native_trace.py']:
    ast.parse(path.read_text(), filename=str(path))

# Check newly added CLI definitions and registrations without evaluating macros.
vty = (root/'bgpd/midr_tier1_vty.c').read_text(encoding='utf-8')
cmds = set(re.findall(r'DEFUN\s*\(\s*\w+\s*,\s*(\w+)', vty))
installed = set(re.findall(r'install_element\([^,]+,\s*&(\w+)\)', vty))
if cmds != installed:
    errors.append(f'CLI declaration/installation mismatch: {cmds ^ installed}')
if re.search(r'\.\.\.\s+\[json\]', re.sub(r'/\*[\s\S]*?\*/', '', vty)):
    errors.append('invalid optional JSON after variadic CLI argument')

git = ['git', '-c', f'safe.directory={root.as_posix()}']
check = subprocess.run(git + ['diff', '--check'], cwd=root, capture_output=True, text=True)
if check.returncode:
    errors.append(check.stdout + check.stderr)
for error in errors:
    print('FAIL:', error)
print(f'Checked {len(prod)} production C/headers, {len(tests)} test C files, '
      f'{len(declared)} public functions, {len(cmds)} CLI commands; '
      f'{len(errors)} structural errors.')
print('Scope: lexical structure, includes, symbol presence, build registration, '
      'Python syntax and forbidden-call scan. No compilation or C tests executed.')
raise SystemExit(bool(errors))
