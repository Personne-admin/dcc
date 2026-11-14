local M = {}

function M.is_absolute(path, windows)
  return windows and (path:match('^%a:[/\\]') ~= nil or path:match('^[/\\][/\\]') ~= nil)
    or (not windows and path:sub(1, 1) == '/')
end

function M.join(directory, name)
  return directory:gsub('[/\\]$', '') .. '/' .. name
end

function M.dirname(path)
  return path:match('^(.*)[/\\][^/\\]+$')
end

-- This resolver never executes dcc/dccd just to find an executable.
function M.resolve(options, callback)
  options = options or {}
  local windows = options.windows
  if windows == nil then windows = vim.fn.has('win32') == 1 end
  local env = options.env or vim.fn.environ()
  local function environment(name)
    for key, value in pairs(env) do if key:lower() == name:lower() then return value end end
  end
  local executable = options.executable or function(path)
    local stat = vim.uv.fs_stat(path)
    return stat and stat.type == 'file' and vim.fn.executable(path) == 1
  end
  local lookup = options.lookup or vim.fn.exepath
  local function check(path)
    if windows and not path:match('%.[^/\\]+$') then path = path .. '.exe' end
    if windows and not path:lower():match('%.exe$') then return nil end
    if executable(path) then return path end
  end
  local function on_path(name)
    local found = lookup(name .. (windows and '.exe' or ''))
    if found and found ~= '' then return check(found) end
  end
  local function configured(path)
    if type(path) ~= 'string' or path == '' then return nil end
    if path:find('[/\\]') or M.is_absolute(path, windows) then
      if not M.is_absolute(path, windows) then path = M.join(options.cwd or vim.fn.getcwd(), path) end
      return check(path)
    end
    if windows and path:lower():sub(-4) == '.exe' then path = path:sub(1, -5) end
    return on_path(path)
  end
  if options.path ~= nil then
    local found = configured(options.path)
    callback(found and { path = found, source = 'explicit path' } or nil,
      not found and 'Configured executable is missing or not executable; remove the override or set an unquoted path' or nil)
    return
  end
  local name = options.name or 'dccd'
  local found = on_path(name)
  if found then callback({ path = found, source = 'PATH' }); return end
  local compiler = on_path('dcc')
  if compiler then
    found = check(M.join(M.dirname(compiler), name .. (windows and '.exe' or '')))
    if found then callback({ path = found, source = 'dcc sibling' }); return end
  end
  if windows and (name == 'dccd' or name == 'dpm') then
    local query = options.installations or function(done)
      require('dcc.msi').discover(env, options.system or vim.system, done)
    end
    query(function(entries, error)
      for _, entry in ipairs(entries or {}) do
        found = check(name == 'dccd' and entry.serverPath or M.join(M.dirname(entry.serverPath), 'dpm.exe'))
        if found then callback({ path = found, source = 'dcc MSI ' .. entry.version }); return end
      end
      local seen = {}
      for _, variable in ipairs({ 'ProgramW6432', 'ProgramFiles', 'ProgramFiles(x86)' }) do
        local root = environment(variable)
        if root and M.is_absolute(root, true) and not seen[root] then
          seen[root] = true
          found = check(M.join(root, 'dcc/bin/' .. name .. '.exe'))
          if found then callback({ path = found, source = 'MSI default', note = error }); return end
        end
      end
      callback(nil, error or name .. ' not found on PATH, beside dcc, or in registered dcc MSI products')
    end)
    return
  end
  callback(nil, name .. ' not found on PATH or beside dcc')
end

return M
