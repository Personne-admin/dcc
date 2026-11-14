local M = {}
local options = {}
local state = { generation = 0, status = 'not configured' }
local resolve_callbacks
local restart

local function notify(error)
  if state.error ~= error then
    vim.schedule(function() vim.notify('DCC: ' .. error .. '. See :DccInfo', vim.log.levels.ERROR) end)
  end
  state.error = error
end

local function resolve(callback)
  if state.server then callback(state.server); return end
  if state.error then callback(nil, state.error); return end
  if resolve_callbacks then resolve_callbacks[#resolve_callbacks + 1] = callback; return end
  local generation = state.generation
  state.status = 'resolving dccd'
  resolve_callbacks = { callback }
  require('dcc.discovery').resolve({ path = options.dccd }, function(selected, error)
    if generation ~= state.generation then return end
    state.server = selected
    state.status = selected and 'executable resolved' or 'discovery failed'
    if error then notify(error) end
    local callbacks = resolve_callbacks
    resolve_callbacks = nil
    for _, done in ipairs(callbacks) do done(selected, error) end
  end)
end

local function root_for(buffer, callback)
  local generation = state.generation
  local ok, root = pcall(require('dcc.project').root, buffer, options.root_dir)
  if not ok then notify('Root detection failed: ' .. tostring(root)); return end
  if not root then return end
  local stat = vim.uv.fs_stat(root)
  if not stat or stat.type ~= 'directory' then notify('Root is not a directory: ' .. root); return end
  resolve(function(selected)
    if selected and generation == state.generation and vim.api.nvim_buf_is_valid(buffer) and vim.bo[buffer].filetype == 'dc' then
      callback(root)
    end
  end)
end

local function attach_open_buffers()
  for _, buffer in ipairs(vim.api.nvim_list_bufs()) do
    if vim.api.nvim_buf_is_loaded(buffer) and vim.bo[buffer].filetype == 'dc' and vim.bo[buffer].buftype == '' then
      root_for(buffer, function(root)
        local config = vim.deepcopy(vim.lsp.config.dccd)
        config.root_dir = root
        vim.lsp.start(config, { bufnr = buffer })
      end)
    end
  end
end

local function resume()
  restart = nil
  vim.lsp.enable('dccd')
  attach_open_buffers()
end

function M.restart()
  if restart then return end
  state.generation = state.generation + 1
  state.server, state.error, state.compiler_version, resolve_callbacks = nil, nil, nil, nil
  state.status = 'restarting'
  vim.lsp.enable('dccd', false)
  local clients = vim.lsp.get_clients({ name = 'dccd' })
  restart = {}
  for _, client in ipairs(clients) do restart[client.id] = true end
  if not next(restart) then resume(); return end
  for _, client in ipairs(clients) do client:stop() end
  local pending = restart
  vim.defer_fn(function()
    if restart ~= pending then return end
    for _, client in ipairs(clients) do if restart[client.id] then client:stop(true) end end
  end, 2000)
end

function M.snapshot()
  local ok, root = pcall(require('dcc.project').root, 0, options.root_dir)
  local lines = {
    'Neovim: ' .. tostring(vim.version()),
    'dccd: ' .. (state.server and state.server.path or '(unresolved)'),
    'Discovery: ' .. (state.server and state.server.source or '(none)'),
    'Status: ' .. state.status,
    'Current root: ' .. (ok and (root or '(disabled)') or '(root detection failed: ' .. tostring(root) .. ')'),
  }
  if state.error then lines[#lines + 1] = 'Failure: ' .. state.error end
  if state.server and state.server.note then lines[#lines + 1] = state.server.note end
  for _, client in ipairs(vim.lsp.get_clients({ name = 'dccd' })) do
    lines[#lines + 1] = ('Client %d: %s, root %s'):format(client.id,
      client.initialized and 'LSP initialized' or 'starting', client.config.root_dir or '(none)')
    lines[#lines + 1] = 'Server version: ' .. (client.server_info and client.server_info.version or 'not reported by dccd')
  end
  if state.compiler_version then lines[#lines + 1] = 'Sibling compiler: ' .. state.compiler_version end
  local task = require('dcc.tasks').last
  if task then lines[#lines + 1] = task.title .. ', output buffer ' .. task.buffer end
  lines[#lines + 1] = 'Configure dccd/dpm with require("dcc").setup(); retry with :DccRestart.'
  lines[#lines + 1] = 'LSP log: ' .. vim.lsp.log.get_filename()
  return lines
end

function M.info()
  resolve(function(selected)
    local function show() vim.notify(table.concat(M.snapshot(), '\n'), vim.log.levels.INFO, { title = 'DCC' }) end
    if not selected then show(); return end
    local discovery = require('dcc.discovery')
    local compiler = discovery.join(discovery.dirname(selected.path), vim.fn.has('win32') == 1 and 'dcc.exe' or 'dcc')
    if vim.fn.executable(compiler) ~= 1 then show(); return end
    local ok = pcall(vim.system, { compiler, '--version' }, { text = true, timeout = 2000 }, function(result)
      vim.schedule(function()
        if result.code == 0 then state.compiler_version = vim.trim(result.stdout or '') end
        show()
      end)
    end)
    if not ok then show() end
  end)
end

function M.setup(config)
  if vim.fn.has('nvim-0.11') ~= 1 then error('dcc.nvim requires Neovim 0.11 or newer') end
  config = config or {}
  for key in pairs(config) do
    if not vim.tbl_contains({ 'dccd', 'dpm', 'settings', 'dpm_commands', 'root_dir' }, key) then error('Unknown dcc option: ' .. key) end
  end
  for _, key in ipairs({ 'dccd', 'dpm' }) do
    if config[key] ~= nil and type(config[key]) ~= 'string' then error(key .. ' must be an executable path string') end
  end
  if config.root_dir ~= nil and type(config.root_dir) ~= 'string' and type(config.root_dir) ~= 'function' then error('root_dir must be a path or function') end
  if config.settings ~= nil and type(config.settings) ~= 'table' then error('settings must be a table') end
  if config.dpm_commands ~= nil and type(config.dpm_commands) ~= 'boolean' then error('dpm_commands must be boolean') end
  options = vim.tbl_deep_extend('force', { settings = {}, dpm_commands = true }, config)
  vim.filetype.add({ extension = { dc = 'dc' } })
  vim.lsp.config('dccd', {
    name = 'dccd',
    filetypes = { 'dc' }, settings = options.settings,
    root_dir = root_for,
    cmd = function(dispatchers)
      assert(state.server, 'dccd discovery must complete before launch')
      state.status = 'starting LSP'
      local ok, rpc = pcall(vim.lsp.rpc.start, { state.server.path }, dispatchers)
      if not ok then
        state.status = 'launch failed'
        notify(tostring(rpc))
        error(rpc, 0)
      end
      return rpc
    end,
    on_init = function() state.status = 'LSP initialized' end,
    on_exit = function(code, signal, id)
      vim.schedule(function()
        state.status = ('dccd exited (%d, signal %d)'):format(code, signal)
        if restart and restart[id] then
          restart[id] = nil
          if not next(restart) then vim.schedule(resume) end
        end
      end)
    end,
  })
  vim.api.nvim_create_user_command('DccInfo', M.info, { force = true, desc = 'DCC server discovery and LSP status' })
  vim.api.nvim_create_user_command('DccRestart', M.restart, { force = true, desc = 'Rediscover dccd and restart project clients' })
  for _, command in ipairs({ 'DccBuild', 'DccRun' }) do pcall(vim.api.nvim_del_user_command, command) end
  if options.dpm_commands then
    for command, action in pairs({ DccBuild = 'build', DccRun = 'run' }) do
      vim.api.nvim_create_user_command(command, function(args) require('dcc.tasks').run(action, args.fargs, options) end,
        { force = true, nargs = '*', desc = 'Run DPM ' .. action .. ' asynchronously for this project' })
    end
  end
  local group = vim.api.nvim_create_augroup('dcc.processes', { clear = true })
  vim.api.nvim_create_autocmd('VimLeavePre', { group = group, callback = function() require('dcc.tasks').stop() end })
  vim.api.nvim_create_autocmd('BufWritePost', { group = group, callback = function(args)
    local name = vim.api.nvim_buf_get_name(args.buf)
    local base = vim.fs.basename(name)
    if vim.bo[args.buf].filetype ~= 'dc' and not vim.tbl_contains({ 'project.dcon', 'project.lock.dcon', 'compile_commands.json', 'dcc.json' }, base) then return end
    local path = vim.fs.normalize(vim.uv.fs_realpath(name) or name)
    for _, client in ipairs(vim.lsp.get_clients({ name = 'dccd' })) do
      local root = client.config.root_dir
      if client.initialized and root and (path == root or path:sub(1, #root + 1) == root .. '/') then
        client:notify('workspace/didChangeWatchedFiles', { changes = { { uri = vim.uri_from_fname(name), type = 2 } } })
      end
    end
  end })
  M.restart()
end

return M
