local repo = vim.fs.normalize(vim.env.DCC_ROOT or vim.fn.getcwd())
vim.opt.runtimepath:prepend(repo .. '/neovim')
vim.cmd('filetype plugin on')
local checks = 0
local function check(value, message)
  checks = checks + 1
  assert(value, message or ('check ' .. checks))
end
local function wait(predicate, message, timeout)
  check(vim.wait(timeout or 10000, predicate, 10), message)
end
local temporary = vim.fs.normalize(vim.fn.tempname()) .. ' DC tests 工具'
vim.fn.mkdir(temporary, 'p')
local function write(path, lines)
  vim.fn.mkdir(vim.fs.dirname(path), 'p')
  vim.fn.writefile(type(lines) == 'table' and lines or vim.split(lines, '\n', { plain = true }), path)
end
local function open(path)
  vim.cmd('edit ' .. vim.fn.fnameescape(path))
  return vim.api.nvim_get_current_buf()
end
local function resolve(options)
  local completed, selected, error = false
  require('dcc.discovery').resolve(options, function(found, reason) completed, selected, error = true, found, reason end)
  wait(function() return completed end, 'discovery did not finish')
  return selected, error
end
local function unit()
  local discovery = require('dcc.discovery')
  local files = { ['C:/Program Files/dcc/bin/dccd.exe'] = true, ['D:/工具/custom.exe'] = true }
  local function options(extra)
    return vim.tbl_extend('force', { windows = true, env = {}, executable = function(path) return files[path] == true end,
      lookup = function() return '' end, installations = function(done) done({}) end }, extra or {})
  end
  check(resolve(options({ path = 'D:/工具/custom.exe' })).source == 'explicit path', 'explicit spaced/Unicode override')
  local selected, error = resolve(options({ path = 'D:/missing.exe', lookup = function() return 'C:/Program Files/dcc/bin/dccd.exe' end }))
  check(not selected and error:find('Configured'), 'invalid explicit override must not fall back')
  check(resolve(options({ lookup = function(name) return name == 'dccd.exe' and 'D:/工具/custom.exe' or '' end })).source == 'PATH')
  check(resolve(options({ lookup = function(name) return name == 'dcc.exe' and 'C:/Program Files/dcc/bin/dcc.exe' or '' end })) == nil, 'missing compiler must not supply a sibling')
  files['C:/Program Files/dcc/bin/dcc.exe'] = true
  check(resolve(options({ lookup = function(name) return name == 'dcc.exe' and 'C:/Program Files/dcc/bin/dcc.exe' or '' end })).source == 'dcc sibling')
  check(resolve(options({ installations = function(done) done({ { serverPath = 'C:/Program Files/dcc/bin/dccd.exe', version = '0.6.0' } }) end })).source:find('MSI'))
  check(resolve(options({ env = { ProgramFiles = 'C:/Program Files' } })).source == 'MSI default')
  local absent, reason = resolve(options())
  check(not absent and reason:find('not found'), 'missing server diagnostic')
  local missing_dpm, dpm_error = resolve(options({ name = 'dpm' }))
  check(not missing_dpm and dpm_error:find('not found'), 'missing Windows DPM is diagnosed')
  files['C:/Program Files/dcc/bin/dpm.exe'] = true
  check(resolve(options({ name = 'dpm', installations = function(done) done({ { serverPath = 'C:/Program Files/dcc/bin/dccd.exe', version = '0.6.0' } }) end })).source:find('MSI'), 'native DPM MSI discovery with stale PATH')
  local linux = resolve({ windows = false, lookup = function(name) return name == 'dccd' and '/tool chain/dccd' or '' end,
    executable = function(path) return path == '/tool chain/dccd' end })
  check(linux.path == '/tool chain/dccd', 'Linux PATH discovery')
  local msi = require('dcc.msi')
  check(table.concat(vim.fn.readfile(repo .. '/vscode/src/windowsInstallation.ts'), '\n'):find(msi.upgrade_code, 1, true), 'product identity drift')
  local done, records = false
  local entries = {
    { productCode = '{12345678-1234-1234-1234-123456789ABC}', upgradeCode = msi.upgrade_code, version = '0.6.0', serverPath = 'D:/用户工具/bin/dccd.exe' },
    { productCode = '{12345678-1234-1234-1234-123456789ABD}', upgradeCode = msi.upgrade_code, version = '0.10.0', serverPath = 'E:/custom/bin/dccd.exe' },
    { productCode = '{12345678-1234-1234-1234-123456789ABE}', upgradeCode = 'unrelated', version = '9.0.0', serverPath = 'F:/bad/dccd.exe' },
  }
  msi.discover({ SystemRoot = 'C:/Windows' }, function(argv, opts, exit)
    check(argv[1] == 'C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe', 'trusted PowerShell')
    check(argv[5] == '-EncodedCommand' and opts.timeout == 5000, 'structured bounded query')
    check(vim.base64.decode(argv[6]) == msi.query:gsub('.', function(c) return c .. '\0' end), 'UTF-16LE query')
    exit({ code = 0, stdout = vim.json.encode(entries) })
  end, function(found) records, done = found, true end)
  check(not done, 'MSI query completion must be scheduled')
  wait(function() return done end, 'MSI callback')
  check(#records == 2 and records[1].version == '0.10.0', 'identity filtering and version ordering')
  local diagnostics = require('dcc.tasks').diagnostics('error: wrong value\n  --> src/main.dc:3:9\n', '/project')
  check(#diagnostics == 1 and diagnostics[1].lnum == 3 and diagnostics[1].text == 'wrong value', 'compiler diagnostics to quickfix')
  check(resolve(options({ path = 'CUSTOM.EXE', lookup = function(name) return name == 'CUSTOM.exe' and 'D:/工具/custom.exe' or '' end })).source == 'explicit path', 'case-insensitive Windows suffix')
  check(resolve(options({ env = { PROGRAMFILES = 'C:/Program Files' } })).source == 'MSI default', 'case-insensitive Windows environment')
  local discovery_resolve, system, notify = discovery.resolve, vim.system, vim.notify
  local project = temporary .. '/argv project'
  write(project .. '/project.dcon', '{}')
  open(project .. '/source.dc')
  local tasks = require('dcc.tasks')
  local captured, complete
  discovery.resolve = function(_, callback) callback({ path = 'C:/Program Files/dpm.exe' }) end
  vim.notify = function() end
  vim.system = function(argv, opts, callback)
    captured, complete = { argv = argv, opts = opts }, callback
    return { kill = function() end }
  end
  tasks.run('run', { 'product with spaces', '--', '', 'a;b', '工具' }, {})
  check(vim.deep_equal(captured.argv, { 'C:/Program Files/dpm.exe', 'run', 'product with spaces', '--', '', 'a;b', '工具' }), 'DPM argument boundaries and no shell')
  check(captured.opts.cwd == project and next(tasks.jobs) ~= nil, 'asynchronous DPM project cwd')
  local first = captured
  tasks.run('build', {}, {})
  check(captured == first, 'one DPM job per root')
  captured.opts.stderr(nil, 'source.dc:2:3: failed\n')
  complete({ code = 7 })
  wait(function() return next(tasks.jobs) == nil end, 'DPM completion')
  check(tasks.last.title == 'DPM run (exit 7)' and #vim.fn.getqflist() == 1, 'nonzero exit and quickfix')
  vim.system, discovery.resolve, vim.notify = system, discovery_resolve, notify
  local roots = require('dcc.project')
  check(roots.root(0, function() return nil end) == nil, 'root opt-out')
  check(roots.root(0, temporary) == temporary, 'root override')
  local plugin = require('dcc')
  check(not pcall(plugin.setup, { dccd = {} }), 'reject invalid executable option')
  check(not pcall(plugin.setup, { unknown = true }), 'reject unknown options')
  vim.cmd('silent! %bwipeout!')
end

local function integration()
  local plugin = require('dcc')
  local dccd = repo .. '/build/bin/dccd'
  local dpm = repo .. '/build/bin/dpm'
  check(vim.fn.executable(dccd) == 1 and vim.fn.executable(dpm) == 1, 'build dccd and dpm before integration tests')
  if vim.fn.has('win32') == 1 then error('native Windows DPM integration requires a separately available Windows DPM') end
  local compiler = temporary .. '/bounded-dcc'
  write(compiler, { '#!/bin/sh', 'exec timeout 60 ' .. vim.fn.shellescape(repo .. '/build/bin/dcc') .. ' "$@"' })
  vim.fn.setfperm(compiler, 'rwx------')
  vim.env.DCC = compiler
  vim.env.DPM_HOME = temporary .. '/dpm-home'
  local project = temporary .. '/hello project'
  local result = vim.system({ dpm, 'new', project }, { text = true, timeout = 30000 }):wait()
  check(result.code == 0, result.stderr)
  local main = project .. '/src/main.dc'
  local helper = project .. '/src/helper.dc'
  local source = { 'module main;', 'import helper;', 'public i32 main() {', '    i32 value = helper::twice(21);', '    return value - 42;', '}' }
  write(main, source)
  write(helper, { 'module helper;', 'public i32 twice(i32 value) { return value + value; }' })
  local tools = temporary .. '/tool chain 工具'
  vim.fn.mkdir(tools, 'p')
  check(vim.uv.fs_symlink(dccd, tools .. '/dccd'), 'server symlink')
  check(vim.uv.fs_symlink(repo .. '/build/bin/dcc', tools .. '/dcc'), 'compiler symlink')
  vim.env.PATH = tools .. ':' .. vim.env.PATH
  plugin.setup({ dpm = dpm, settings = { dcc = { includePaths = { 'src' } } } })
  local buffer = open(main)
  wait(function() local c = vim.lsp.get_clients({ bufnr = buffer, name = 'dccd' }); return #c == 1 and c[1].initialized end, 'LSP initialize/attachment')
  local client = vim.lsp.get_clients({ bufnr = buffer, name = 'dccd' })[1]
  check(client.config.root_dir == project, 'DPM root')
  check(client.config.cmd ~= nil and table.concat(plugin.snapshot(), '\n'):find(tools .. '/dccd', 1, true), 'real spaced/Unicode executable path')
  check(table.concat(plugin.snapshot(), '\n'):find('Discovery: PATH', 1, true), 'native Linux PATH discovery')
  vim.cmd('DccInfo')
  wait(function() return table.concat(plugin.snapshot(), '\n'):find('Sibling compiler:', 1, true) ~= nil end, 'asynchronous compiler version info')
  local helper_buffer = open(helper)
  wait(function() return vim.lsp.buf_is_attached(helper_buffer, client.id) end, 'helper must attach')
  check(#vim.lsp.get_clients({ name = 'dccd' }) == 1, 'duplicate process for one project')
  vim.api.nvim_set_current_buf(buffer)
  local function request(method, params)
    local response = client:request_sync(method, params, 5000, buffer)
    check(response and not response.err, method .. ': ' .. vim.inspect(response))
    return response.result
  end
  local uri = vim.uri_from_bufnr(buffer)
  local position = { line = 3, character = source[4]:find('value', 1, true) - 1 }
  local hover = request('textDocument/hover', { textDocument = { uri = uri }, position = position })
  check(hover and hover ~= vim.NIL and hover.contents, 'hover payload: ' .. vim.inspect({ hover = hover, diagnostics = vim.diagnostic.get(buffer), uri = uri, clients = #vim.lsp.get_clients() }))
  local completion = request('textDocument/completion', { textDocument = { uri = uri }, position = { line = 4, character = 12 } })
  local items = completion.items or completion
  check(vim.iter(items):any(function(item) return item.label == 'value' end), 'completion includes local variable')
  local definition = request('textDocument/definition', { textDocument = { uri = uri }, position = { line = 3, character = source[4]:find('twice', 1, true) } })
  if definition.uri then definition = { definition } end
  check(#definition > 0 and (definition[1].uri or definition[1].targetUri) == vim.uri_from_fname(helper), 'imported definition')
  local refs = request('textDocument/references', { textDocument = { uri = uri }, position = position, context = { includeDeclaration = true } })
  check(#refs >= 2, 'reference locations')
  local renamed = request('textDocument/rename', { textDocument = { uri = uri }, position = position, newName = 'answer' })
  check(renamed.changes and renamed.changes[uri] and #renamed.changes[uri] >= 2, 'rename edits')
  vim.api.nvim_buf_set_lines(buffer, 4, 5, false, { '    return missing;' })
  wait(function() return #vim.diagnostic.get(buffer) > 0 end, 'diagnostics after change')
  vim.api.nvim_buf_set_lines(buffer, 0, -1, false, source)
  wait(function() return #vim.diagnostic.get(buffer) == 0 end, 'diagnostics clear')
  vim.api.nvim_buf_set_lines(buffer, 0, -1, false, { 'module main;', 'import helper;', 'public i32 main(){i32 value=helper::twice(21);return value-42;}' })
  local edits = request('textDocument/formatting', { textDocument = { uri = uri }, options = { tabSize = 4, insertSpaces = true } })
  check(#edits > 0, 'formatting must provide edits')
  vim.lsp.util.apply_text_edits(edits, buffer, client.offset_encoding)
  check(#vim.api.nvim_buf_get_lines(buffer, 0, -1, false) > 3, 'formatted layout')
  vim.api.nvim_buf_set_lines(buffer, 0, -1, false, source)
  local original_notify, watched = client.notify, false
  client.notify = function(self, method, params)
    if method == 'workspace/didChangeWatchedFiles' then watched = params.changes[1].uri == uri end
    return original_notify(self, method, params)
  end
  vim.cmd('write')
  check(watched, 'save sends workspace notification on Linux')
  client.notify = original_notify
  vim.cmd('DccBuild --offline')
  wait(function() return next(require('dcc.tasks').jobs) == nil end, 'DPM build did not finish', 60000)
  check(vim.fn.filereadable(project .. '/compile_commands.json') == 1, 'real DPM build writes compilation database')
  vim.cmd('DccRun --offline')
  wait(function() return next(require('dcc.tasks').jobs) == nil end, 'DPM run did not finish', 60000)
  check(require('dcc.tasks').last.title == 'DPM run (exit 0)', 'real program execution')
  vim.api.nvim_set_current_buf(buffer)
  local old_id = client.id
  vim.cmd('DccRestart')
  wait(function() local c = vim.lsp.get_clients({ bufnr = buffer, name = 'dccd' }); return #c == 1 and c[1].initialized and c[1].id ~= old_id end, 'restart and reattach')
  check(client:is_stopped(), 'old process shutdown')
  local hash = string.rep('ab', 32)
  local cached = temporary .. '/dpm cache/src/' .. hash .. '/src/pkg/lib.dc'
  write(temporary .. '/dpm cache/src/' .. hash .. '/project.dcon', 'dpm::v1::Project { name = "pkg", version = "0.1.0" }')
  write(cached, { 'module pkg::lib;', 'public i32 value() { return 1; }' })
  vim.api.nvim_set_current_buf(buffer)
  local project_client = vim.lsp.get_clients({ bufnr = buffer, name = 'dccd' })[1]
  local dependency = open(cached)
  check(require('dcc.project').cached_package(dependency) ~= nil, 'cached DPM package detected')
  wait(function() local c = vim.lsp.get_clients({ bufnr = dependency, name = 'dccd' }); return #c == 1 and c[1].initialized end, 'cached package attachment')
  check(vim.lsp.get_clients({ bufnr = dependency, name = 'dccd' })[1].id == project_client.id, 'cached package keeps the project client')
  check(#vim.lsp.get_clients({ name = 'dccd' }) == 1, 'no extra server for a cached package')
  vim.api.nvim_buf_delete(dependency, { force = true })
  vim.api.nvim_set_current_buf(buffer)
  local standalone = temporary .. '/standalone/alone.dc'
  write(standalone, { 'module alone;', 'public i32 value() { return 42; }' })
  local alone = open(standalone)
  wait(function() local c = vim.lsp.get_clients({ bufnr = alone, name = 'dccd' }); return #c == 1 and c[1].initialized end, 'standalone attachment')
  check(vim.lsp.get_clients({ bufnr = alone, name = 'dccd' })[1].config.root_dir == vim.fs.dirname(standalone), 'standalone root')
  check(#vim.lsp.get_clients({ name = 'dccd' }) == 2, 'one process per distinct root')
  local notices = {}
  local original_notify = vim.notify
  vim.notify = function(message) notices[#notices + 1] = message end
  vim.cmd('DccBuild')
  check(notices[#notices]:find('No project.dcon'), 'standalone build must not execute DPM')
  vim.notify = original_notify
  plugin.setup({ dccd = '/dcc-missing-server', dpm_commands = false })
  wait(function() return #vim.lsp.get_clients({ name = 'dccd' }) == 0 end, 'stop previous configured clients')
  check(vim.fn.exists(':DccBuild') == 0, 'optional DPM commands')
  check(table.concat(plugin.snapshot(), '\n'):find('Configured executable'), 'missing server health')
end

local ok, error = xpcall(function() unit(); if vim.env.DCC_NVIM_UNIT_ONLY ~= '1' then integration() end end, debug.traceback)
for _, client in ipairs(vim.lsp.get_clients({ name = 'dccd' })) do client:stop(true) end
vim.wait(3000, function() return #vim.lsp.get_clients({ name = 'dccd' }) == 0 end, 10)
require('dcc.tasks').stop()
vim.fn.delete(temporary, 'rf')
if not ok then io.stderr:write(error .. '\n'); vim.cmd('cquit 1') end
print(('PASS %d Neovim discovery, LSP and DPM checks'):format(checks))
vim.cmd('qa!')
