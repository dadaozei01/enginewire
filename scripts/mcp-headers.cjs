'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

function run(cmd, args) {
  return execFileSync(cmd, args, { encoding: 'utf8', windowsHide: true });
}

// PID of the process listening on the MCP port.
function listeningPid(port) {
  if (process.platform === 'win32') {
    for (const line of run('netstat', ['-ano', '-p', 'tcp']).split(/\r?\n/)) {
      const cols = line.trim().split(/\s+/);
      if (cols.length >= 5 && /LISTEN/i.test(cols[3]) && cols[1].endsWith(':' + port)) return Number(cols[4]);
    }
  } else {
    const pids = run('lsof', ['-nP', '-iTCP:' + port, '-sTCP:LISTEN', '-t']).trim().split(/\s+/).filter(Boolean);
    // SO_REUSEPORT lets editors share the port, and the newest gets the connections.
    if (pids.length > 1) throw new Error(pids.length + ' editors are listening on port ' + port + '; close all but one');
    if (pids[0]) return Number(pids[0]);
  }
  throw new Error('Nothing is listening on port ' + port);
}

// Executable name and full command line of a process.
function processInfo(pid) {
  if (process.platform === 'win32') {
    const ps = '[Console]::OutputEncoding=[Text.Encoding]::UTF8;' +
      '$p=Get-CimInstance Win32_Process -Filter "ProcessId=' + pid + '";$p.Name;$p.CommandLine;$p.ExecutablePath';
    const [name = '', cmdline = '', exePath = ''] = run('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', ps]).split(/\r?\n/);
    return { name, cmdline, exePath };
  }
  return { name: path.basename(run('ps', ['-p', String(pid), '-o', 'comm=']).trim()),
    cmdline: run('ps', ['-p', String(pid), '-o', 'args=']).trim() };
}

// The .uproject on the editor command line; paths may contain spaces or CJK characters.
function projectFromCommandLine(cmdline) {
  const exe = cmdline.match(/UnrealEditor(?:-Cmd)?(?:\.exe)?"?(?=\s)/i);
  const args = exe ? cmdline.slice(exe.index + exe[0].length) : cmdline;
  const m = args.match(/"([^"]+\.uproject)"|(\S.*?\.uproject)(?=\s|$)/i);
  if (!m) throw new Error('Editor command line has no .uproject');
  return m[1] || m[2];
}

// A Finder launch on Mac has no .uproject on the command line, but the editor holds the project's own modules open.
function projectFromOpenFiles(pid) {
  const projects = new Set();
  const seen = new Set();
  for (const line of run('lsof', ['-p', String(pid), '-Fn']).split('\n')) {
    let folder = line.startsWith('n/') ? path.dirname(line.slice(1)) : '';
    while (folder && folder !== '/' && !seen.has(folder)) {
      seen.add(folder);
      let found = [];
      try {
        found = fs.readdirSync(folder).filter((f) => f.toLowerCase().endsWith('.uproject'));
      } catch { /* an open file's folder may be unreadable */ }
      if (found.length === 1) projects.add(path.join(folder, found[0]));
      folder = path.dirname(folder);
    }
  }
  if (projects.size !== 1) throw new Error('Cannot tell the open project from the editor (' + projects.size + ' candidates); pass the .uproject path');
  return [...projects][0];
}

// A Windows editor started with no project (it opens the last one by itself) keeps its Saved folder,
// and so the token, under %LOCALAPPDATA%\UnrealEngine\<major.minor>, not under the project.
function engineUserDir(exePath) {
  const engineDir = path.resolve(path.dirname(exePath), '..', '..');
  const version = JSON.parse(fs.readFileSync(path.join(engineDir, 'Build', 'Build.version'), 'utf8'));
  const dir = path.join(process.env.LOCALAPPDATA || '', 'UnrealEngine', version.MajorVersion + '.' + version.MinorVersion);
  if (!fs.existsSync(path.join(dir, 'Saved', 'MCP'))) {
    throw new Error('Editor command line has no .uproject and ' + path.join(dir, 'Saved', 'MCP') + ' does not exist; pass the .uproject path');
  }
  return dir;
}

// With no project given, use the UnrealEditor that is listening on the MCP port,
// so the token always matches whichever project is open.
function autoProject() {
  const url = new URL(process.env.CLAUDE_CODE_MCP_SERVER_URL || process.env.UE_MCP_URL || 'http://127.0.0.1:3000/mcp');
  const pid = listeningPid(url.port || 80);
  const { name, cmdline, exePath } = processInfo(pid);
  if (!/^UnrealEditor/i.test(name)) throw new Error('Port owner is not UnrealEditor');
  if (process.platform === 'darwin' && !/\.uproject/i.test(cmdline)) return projectFromOpenFiles(pid);
  if (process.platform === 'win32' && !/\.uproject/i.test(cmdline) && exePath) return engineUserDir(exePath);
  return projectFromCommandLine(cmdline);
}

function headers(project) {
  if (!project || project === 'auto') project = autoProject();
  const resolved = path.resolve(project);
  const dir = resolved.toLowerCase().endsWith('.uproject') ? path.dirname(resolved) : resolved;
  // Prefer the scoped token; the legacy all-or-nothing token is deprecated.
  const mcpDir = path.join(dir, 'Saved', 'MCP');
  const scoped = path.join(mcpDir, 'scoped-capability-token');
  const file = fs.existsSync(scoped) ? scoped : path.join(mcpDir, 'capability-token');
  const token = fs.readFileSync(file, 'utf8').trim();
  if (!/^[a-f0-9]{64}$/i.test(token)) throw new Error('Invalid capability token format');
  return { 'X-MCP-Capability-Token': token };
}
module.exports = { headers, autoProject, projectFromCommandLine };
if (require.main === module) {
  try { process.stdout.write(JSON.stringify(headers(process.argv[2] || process.env.UE_PROJECT_PATH))); }
  catch (e) { process.stderr.write('Cannot read MCP capability token: ' + e.message + '\n'); process.exitCode = 1; }
}
