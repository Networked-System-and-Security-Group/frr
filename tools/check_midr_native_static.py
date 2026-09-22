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
tests += [root / 'tests/bgpd/test_midr_admission.c']
admission = [root/'bgpd/bgp_midr_admission.c', root/'bgpd/bgp_midr_admission.h']
files = prod + tests + admission
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

body = '\n'.join(v for k, v in sources.items() if k.startswith('bgpd/midr_') and k.endswith('.c'))
declarations = '\n'.join(v for k,v in sources.items() if k.startswith('bgpd/midr_') and k.endswith('.h'))
definitions = set(re.findall(r'\b(midr_\w+)\s*\([^;{}]*\)\s*\{', body))
declared = set(re.findall(r'\b(midr_\w+)\s*\([^;{}]*\)\s*;', declarations))
for symbol in sorted(declared - definitions):
    errors.append(f'public function without implementation: {symbol}')
calls = set(re.findall(r'\b(midr_\w+)\s*\(', body))
for symbol in sorted(calls - definitions - declared):
    errors.append(f'unresolved MIDR function reference: {symbol}')
for forbidden in ('popen', 'pclose', 'system', 'posix_spawn', 'execvp', 'execlp', 'waitpid', 'nanosleep'):
    if re.search(r'\b' + forbidden + r'\s*\(', body + sources['bgpd/bgp_midr_admission.c']):
        errors.append(f'forbidden external/blocking call: {forbidden}')
if 'midr_tier1_common_seed' in body or 'midr_trace_exec_' in body:
    errors.append('removed implementation still referenced')

manifest = (root/'bgpd/subdir.am').read_text(encoding='utf-8')
test_manifest = (root/'tests/bgpd/subdir.am').read_text(encoding='utf-8')
for path in prod + admission:
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

# Admission integration checks: these catch omitted integration sites, not C
# typing, FSM runtime behavior, or Linux socket correctness.
admission_body = sources['bgpd/bgp_midr_admission.c']
admission_header = sources['bgpd/bgp_midr_admission.h']

# Default deadlines must cover the bounded per-hop retry profile. This checks
# configuration consistency; C regression tests exercise actual event behavior.
def macro_number(path, name):
    value = re.search(r'^#define\s+' + name + r'\s+(\d+)U?\b',
                      (root/path).read_text(encoding='utf-8'), re.M)
    if not value:
        raise ValueError(f'missing numeric constant: {name}')
    return int(value[1])

engine_header = 'bgpd/midr_trace_engine.h'
probes = macro_number(engine_header, 'MIDR_TRACE_PROBES_PER_HOP')
ttl = macro_number(engine_header, 'MIDR_TRACE_MAX_TTL')
wait = macro_number(engine_header, 'MIDR_TRACE_PROBE_WAIT_MSEC')
pace = macro_number(engine_header, 'MIDR_TRACE_SEND_INTERVAL_MSEC')
execution = macro_number('bgpd/midr_trace_scheduler.c', 'MIDR_TRACE_DEFAULT_EXEC_TIMEOUT_MSEC')
queue = macro_number('bgpd/midr_trace_scheduler.c', 'MIDR_TRACE_DEFAULT_QUEUE_TIMEOUT_MSEC')
admission_deadline = macro_number('bgpd/bgp_midr_admission.c', 'ADMISSION_DEADLINE_MS')
grace = macro_number('bgpd/bgp_midr_ctrl.c', 'MIDR_CTRL_ADMISSION_GRACE_MS')
if probes != 3 or execution < ttl * probes * (wait + pace):
    errors.append('default traceroute budget cannot cover three probes per hop')
if admission_deadline <= queue + execution or grace <= admission_deadline:
    errors.append('admission/control deadlines do not cover default probe budget')
admission_defs = set(re.findall(r'\b(midr_admission_\w+)\s*\([^;{}]*\)\s*\{', admission_body))
admission_decls = set(re.findall(r'\b(midr_admission_\w+)\s*\([^;{}]*\)\s*;', admission_header))
if admission_decls != admission_defs:
    errors.append(f'admission API mismatch: {admission_decls ^ admission_defs}')

def clean_source(name):
    return stripped((root/name).read_text(encoding='utf-8'))

ctrl = clean_source('bgpd/bgp_midr_ctrl.c')
start = ctrl.index('static enum midr_admission_result midr_ctrl_connect_internal(')
end = ctrl.index('enum midr_admission_result midr_ctrl_connect(', start)
connect = ctrl[start:end]
gate = connect.index('midr_admission_gate(')
for call in ('midr_nds_ledger_note', 'midr_nds_adopt_group_peer',
             'midr_ctrl_send_peer_request', 'peer_remote_as'):
    if connect.index(call + '(') < gate:
        errors.append(f'build-neighbor side effect precedes admission: {call}')
for name, required in {
    'bgpd/bgp_network.c': ['midr_admission_peer_ready(peer)',
                           'midr_admission_begin(incoming)',
                           'midr_admission_begin(connection)'],
    'bgpd/bgp_fsm.c': ['midr_admission_check(connection)', 'case BGP_FSM_DEFERRED:',
                       'connection->midr_admission_permit = 0;'],
    'bgpd/bgp_midr_nds.c': ['midr_admission_init(bgp)', 'midr_admission_finish(bgp)',
                           'midr_admission_reset(bgp)', 'midr_admission_forget(bgp, transport)'],
}.items():
    source = clean_source(name)
    for marker in required:
        if marker not in source:
            errors.append(f'missing admission integration: {name}: {marker}')
if admission_body.index('token->owner = NULL;') > admission_body.index('midr_trace_cancel('):
    errors.append('cancel can expose a freed admission owner')
fsm = clean_source('bgpd/bgp_fsm.c')
if 'if (bgp_stop(connection) != BGP_FSM_FAILURE_AND_DELETE)' not in fsm:
    errors.append('admission deferral does not handle passive clone deletion')
if 'midr_tier1_vty_init();' not in clean_source('bgpd/bgp_vty.c') or \
        'bgp_midr_nds_vty_init();' not in clean_source('bgpd/bgpd.c'):
    errors.append('MIDR command initialization missing from bgpd startup')
for marker in ('peer_source_matches(peer, e)', 'connection->su_local',
               'bgp->vrf_id == VRF_DEFAULT', 'midr_nds_manual_sessions_restore(m->bgp)'):
    if marker not in admission_body:
        errors.append(f'missing admission context/recovery guard: {marker}')
for name, expected in [('bgpd/midr_ip2asn.c', 3), ('bgpd/midr_tier1_list.c', 2)]:
    if clean_source(name).count('midr_policy_notify_changed();') != expected:
        errors.append(f'policy publication notification missing: {name}')
for name in ('bgpd/midr_tier1_list.h', 'bgpd/midr_tier1_list.c'):
    if re.search(r'(?:DECLARE|DEFINE)_HOOK\(midr_policy_changed,\s*\(void\)',
                 clean_source(name)):
        errors.append(f'no-argument FRR hooks require (), not (void): {name}')
if 'hook_call(midr_policy_changed)' in clean_source('bgpd/midr_ip2asn.c'):
    errors.append('IP2ASN cannot call the Tier1 translation unit private hook dispatcher')
if clean_source('bgpd/midr_tier1_list.c').count('hook_call(midr_policy_changed)') != 1:
    errors.append('policy notifier must dispatch the hook in its defining translation unit')
if '#include "bgpd/bgp_vty.h"' not in (root/'bgpd/bgp_midr_admission.c').read_text(encoding='utf-8'):
    errors.append('admission requires bgp_vty.h for bgp_config_inprocess')
state_enum = re.search(r'enum admission_state\s*\{([^}]+)\}', admission_body)
if not state_enum or any(not value.strip().startswith('ADMISSION_')
                         for value in state_enum[1].split(',') if value.strip()):
    errors.append('admission state enumerators must use the ADMISSION_ prefix')
nds_vty = clean_source('bgpd/bgp_midr_nds_vty.c')
for node, command in [('BGP_NODE', 'midr_avoid_tier1_cmd'),
                      ('VIEW_NODE', 'show_midr_admission_cmd')]:
    if not re.search(r'install_element\(\s*' + node + r',\s*&' + command + r'\)', nds_vty):
        errors.append(f'admission command missing from {node}: {command}')

git = ['git', '-c', f'safe.directory={root.as_posix()}']
check = subprocess.run(git + ['diff', '--check'], cwd=root, capture_output=True, text=True)
if check.returncode:
    errors.append(check.stdout + check.stderr)
for error in errors:
    print('FAIL:', error)
print(f'Checked {len(prod)} production C/headers, {len(tests)} test C files, '
      f'{len(declared)} public functions, {len(cmds)} CLI commands; '
      f'{len(errors)} structural errors.')
print(f'Also checked {len(admission)} admission C/headers, '
      f'{len(admission_decls)} admission APIs and integration markers.')
print('Scope: lexical structure, includes, symbol presence, build registration, '
      'Python syntax and forbidden-call scan. No compilation or C tests executed.')
raise SystemExit(bool(errors))
