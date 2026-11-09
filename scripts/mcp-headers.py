#!/usr/bin/env python3
"""Python equivalent of mcp-headers.cjs for machines without Node (macOS ships python3)."""
import json
import os
import re
import subprocess
import sys
from urllib.parse import urlparse


def run(args):
    return subprocess.run(args, capture_output=True, check=True, encoding='utf-8', errors='replace').stdout


def listening_pid(port):
    """PID of the process listening on the MCP port."""
    if sys.platform == 'win32':
        for line in run(['netstat', '-ano', '-p', 'tcp']).splitlines():
            cols = line.split()
            if len(cols) >= 5 and 'LISTEN' in cols[3].upper() and cols[1].endswith(':%d' % port):
                return int(cols[4])
    else:
        pids = run(['lsof', '-nP', '-iTCP:%d' % port, '-sTCP:LISTEN', '-t']).split()
        if len(pids) > 1:  # SO_REUSEPORT lets editors share the port, and the newest gets the connections
            raise RuntimeError('%d editors are listening on port %d; close all but one' % (len(pids), port))
        if pids:
            return int(pids[0])
    raise RuntimeError('Nothing is listening on port %d' % port)


def process_info(pid):
    """Executable name and full command line of a process."""
    if sys.platform == 'win32':
        ps = ('[Console]::OutputEncoding=[Text.Encoding]::UTF8;'
              '$p=Get-CimInstance Win32_Process -Filter "ProcessId=%d";$p.Name;$p.CommandLine;$p.ExecutablePath' % pid)
        lines = run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', ps]).splitlines() + ['', '', '']
        return lines[0], lines[1], lines[2]
    return (os.path.basename(run(['ps', '-p', str(pid), '-o', 'comm=']).strip()),
            run(['ps', '-p', str(pid), '-o', 'args=']).strip(), '')


def project_from_command_line(cmdline):
    """The .uproject on the editor command line; paths may contain spaces or CJK characters."""
    exe = re.search(r'UnrealEditor(?:-Cmd)?(?:\.exe)?"?(?=\s)', cmdline, re.I)
    args = cmdline[exe.end():] if exe else cmdline
    m = re.search(r'"([^"]+\.uproject)"|(\S.*?\.uproject)(?=\s|$)', args, re.I)
    if not m:
        raise RuntimeError('Editor command line has no .uproject')
    return m.group(1) or m.group(2)


def project_from_open_files(pid):
    """A Finder launch on Mac has no .uproject on the command line, but the editor holds the project's own modules open."""
    projects, seen = set(), set()
    for line in run(['lsof', '-p', str(pid), '-Fn']).splitlines():
        folder = os.path.dirname(line[1:]) if line.startswith('n/') else ''
        while folder not in ('', '/') and folder not in seen:
            seen.add(folder)
            try:
                found = [f for f in os.listdir(folder) if f.lower().endswith('.uproject')]
            except OSError:
                found = []
            if len(found) == 1:
                projects.add(os.path.join(folder, found[0]))
            folder = os.path.dirname(folder)
    if len(projects) != 1:
        raise RuntimeError('Cannot tell the open project from the editor (%d candidates); pass the .uproject path' % len(projects))
    return projects.pop()


def engine_user_dir(exe_path):
    """A Windows editor started with no project (it opens the last one by itself) keeps its Saved folder,
    and so the token, under %LOCALAPPDATA%\\UnrealEngine\\<major.minor>, not under the project."""
    engine_dir = os.path.normpath(os.path.join(os.path.dirname(exe_path), '..', '..'))
    with open(os.path.join(engine_dir, 'Build', 'Build.version'), encoding='utf-8') as f:
        version = json.load(f)
    folder = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'UnrealEngine',
                          '%d.%d' % (version['MajorVersion'], version['MinorVersion']))
    mcp_dir = os.path.join(folder, 'Saved', 'MCP')
    if not os.path.isdir(mcp_dir):
        raise RuntimeError('Editor command line has no .uproject and %s does not exist; pass the .uproject path' % mcp_dir)
    return folder


def auto_project():
    """With no project given, use the UnrealEditor listening on the MCP port, so the token matches the open project."""
    url = urlparse(os.environ.get('CLAUDE_CODE_MCP_SERVER_URL') or os.environ.get('UE_MCP_URL') or 'http://127.0.0.1:3000/mcp')
    pid = listening_pid(url.port or 80)
    name, cmdline, exe_path = process_info(pid)
    if not name.lower().startswith('unrealeditor'):
        raise RuntimeError('Port owner is not UnrealEditor')
    if sys.platform == 'darwin' and '.uproject' not in cmdline.lower():
        return project_from_open_files(pid)
    if sys.platform == 'win32' and '.uproject' not in cmdline.lower() and exe_path:
        return engine_user_dir(exe_path)
    return project_from_command_line(cmdline)


def headers(project):
    if not project or project == 'auto':
        project = auto_project()
    resolved = os.path.abspath(project)
    folder = os.path.dirname(resolved) if resolved.lower().endswith('.uproject') else resolved
    # Prefer the scoped token; the legacy all-or-nothing token is deprecated.
    mcp_dir = os.path.join(folder, 'Saved', 'MCP')
    scoped = os.path.join(mcp_dir, 'scoped-capability-token')
    path = scoped if os.path.isfile(scoped) else os.path.join(mcp_dir, 'capability-token')
    with open(path, encoding='utf-8') as f:
        token = f.read().strip()
    if not re.fullmatch(r'[a-fA-F0-9]{64}', token):
        raise ValueError('Invalid capability token format')
    return {'X-MCP-Capability-Token': token}


if __name__ == '__main__':
    try:
        sys.stdout.write(json.dumps(headers(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('UE_PROJECT_PATH'))))
    except Exception as e:
        sys.stderr.write('Cannot read MCP capability token: %s\n' % e)
        sys.exit(1)
