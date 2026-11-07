#!/usr/bin/env python3
"""Read-only EngineWire check: confirm which editor owns the MCP port, then handshake, list tools and search.

    /usr/bin/python3 -I scripts/mcp-smoke.py --project <path to .uproject> [--engine 5.8] [--url <mcp url>]

Stops before sending the token if the port owner is not the expected project and engine. Never prints the token,
never calls execute, and writes nothing. Exit code 0 only when every check passes.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from urllib.parse import urlparse

SCRIPTS = os.path.dirname(os.path.abspath(__file__))
PROTOCOL = '2025-06-18'


def run(args):
    return subprocess.run(args, capture_output=True, check=True, encoding='utf-8', errors='replace').stdout


def editor_identity(port):
    """PID, command line and engine version of the process listening on the port."""
    try:
        pids = run(['lsof', '-nP', '-iTCP:%d' % port, '-sTCP:LISTEN', '-t']).split()
    except subprocess.CalledProcessError:  # lsof exits 1 when it finds nothing
        pids = []
    if not pids:
        raise RuntimeError('Nothing is listening on port %d' % port)
    if len(pids) > 1:  # SO_REUSEPORT lets editors share the port, and the newest gets the connections
        raise RuntimeError('%d processes are listening on port %d (%s); close all but one' % (len(pids), port, ', '.join(pids)))
    pid = int(pids[0])
    cmdline = run(['ps', '-p', str(pid), '-o', 'args=']).strip()
    name = os.path.basename(run(['ps', '-p', str(pid), '-o', 'comm=']).strip())
    if not name.lower().startswith('unrealeditor'):
        raise RuntimeError('Port %d belongs to %s, not UnrealEditor' % (port, name))
    root = re.search(r'^(.*?)/Engine/Binaries/', run(['ps', '-p', str(pid), '-o', 'comm=']).strip())
    if not root:
        raise RuntimeError('Cannot find the engine directory of PID %d' % pid)
    with open(os.path.join(root.group(1), 'Engine', 'Build', 'Build.version'), encoding='utf-8') as f:
        v = json.load(f)
    return pid, cmdline, '%d.%d.%d' % (v['MajorVersion'], v['MinorVersion'], v['PatchVersion'])


def open_files_contain(pid, folder):
    """The editor holds files of its project open, which identifies it when the command line has no .uproject."""
    try:
        return folder + '/' in run(['lsof', '-p', str(pid), '-Fn'])
    except subprocess.CalledProcessError as e:
        return folder + '/' in (e.stdout or '')


def parse_body(raw):
    """A reply is JSON or a server-sent event stream; return the JSON-RPC message."""
    text = raw.decode('utf-8', 'replace').strip()
    if text.startswith('{'):
        return json.loads(text)
    for line in text.splitlines():
        if line.startswith('data:'):
            data = line[5:].strip()
            if data.startswith('{'):
                msg = json.loads(data)
                if 'result' in msg or 'error' in msg:
                    return msg
    raise RuntimeError('No JSON-RPC message in reply')


class Client:
    def __init__(self, url, headers):
        self.url, self.base, self.session, self.protocol, self.next_id = url, headers, None, None, 1

    def post(self, method, params=None, notify=False):
        msg = {'jsonrpc': '2.0', 'method': method}
        if params is not None:
            msg['params'] = params
        if not notify:
            msg['id'] = self.next_id
            self.next_id += 1
        headers = dict(self.base)
        headers.update({'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream'})
        if self.session:
            headers['Mcp-Session-Id'] = self.session
        if self.protocol:
            headers['MCP-Protocol-Version'] = self.protocol
        req = urllib.request.Request(self.url, json.dumps(msg).encode(), headers, method='POST')
        with urllib.request.urlopen(req, timeout=30) as r:
            self.session = r.headers.get('Mcp-Session-Id') or self.session
            raw = r.read()
        if notify:
            return None
        reply = parse_body(raw)
        if 'error' in reply:
            raise RuntimeError('%s failed: %s' % (method, reply['error'].get('message')))
        return reply['result']


def handshake(url, headers, query='actor list'):
    """Yield (check name, passed, detail) for the read-only MCP calls."""
    c = Client(url, headers)
    init = c.post('initialize', {'protocolVersion': PROTOCOL, 'capabilities': {},
                                 'clientInfo': {'name': 'enginewire-smoke', 'version': '1'}})
    c.protocol = init.get('protocolVersion')
    info = init.get('serverInfo', {})
    yield 'initialize', bool(c.session), 'server %s %s, protocol %s' % (info.get('name'), info.get('version'), c.protocol)
    c.post('notifications/initialized', notify=True)
    tools = c.post('tools/list', {}).get('tools', [])
    yield 'tools/list', bool(tools), '%d tools' % len(tools)
    by_name = {t['name']: t for t in tools}
    # The gateway is either one tool taking "operation" or one tool per operation; follow the published schema.
    gateway = next((t for t in tools if 'operation' in t.get('inputSchema', {}).get('properties', {})), None)
    if 'search' in by_name:
        name, args = 'search', {'query': query}
    elif gateway:
        name, args = gateway['name'], {'operation': 'search', 'query': query}
    else:
        yield 'search', False, 'no search operation among: %s' % ', '.join(sorted(by_name)[:10])
        return
    result = c.post('tools/call', {'name': name, 'arguments': args})
    text = json.dumps(result, ensure_ascii=False)
    yield 'search "%s"' % query, not result.get('isError') and 'nextCall' in text, '%d bytes' % len(text)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--project', required=True, help='absolute path of the expected .uproject')
    ap.add_argument('--engine', default='5.8', help='expected engine major.minor')
    ap.add_argument('--url', default='http://127.0.0.1:3000/mcp')
    a = ap.parse_args()
    project = os.path.abspath(a.project)
    failed = 0

    def report(name, passed, detail=''):
        nonlocal failed
        failed += 0 if passed else 1
        print('%s  %s%s' % ('PASS' if passed else 'FAIL', name, '  ' + detail if detail else ''))

    try:
        pid, cmdline, version = editor_identity(urlparse(a.url).port or 80)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as e:
        report('editor identity', False, str(e))
        return 1
    engine_ok = version.startswith(a.engine + '.')
    in_cmdline = project in cmdline
    project_ok = in_cmdline or open_files_contain(pid, os.path.dirname(project))
    report('engine version', engine_ok, 'UE %s, PID %d' % (version, pid))
    report('target project', project_ok, project)
    # Not a failure: it only decides whether the helper can find the project without a path argument.
    print('INFO  .uproject on command line: %s (the helper\'s auto-detect %s)' %
          ('yes' if in_cmdline else 'no', 'reads it' if in_cmdline else 'finds it from open files'))
    if not engine_ok or not project_ok:
        print('Stopped before sending the token: this is not the expected editor.')
        return 1

    try:
        headers = json.loads(run([sys.executable, '-I', os.path.join(SCRIPTS, 'mcp-headers.py'), project]))
        for name, passed, detail in handshake(a.url, headers):
            report(name, passed, detail)
    except subprocess.CalledProcessError as e:
        report('capability token', False, (e.stderr or '').strip())
    except urllib.error.HTTPError as e:
        report('MCP request', False, 'HTTP %d %s' % (e.code, e.reason))
    except (OSError, RuntimeError, ValueError) as e:
        report('MCP request', False, str(e))
    print('Result: %s' % ('all checks passed' if not failed else '%d failed' % failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
