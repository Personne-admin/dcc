local M = {}

function M.directory(bufnr)
  if vim.bo[bufnr].buftype ~= '' then return vim.uv.cwd() end
  local name = vim.api.nvim_buf_get_name(bufnr)
  return name ~= '' and vim.fs.dirname(name) or vim.uv.cwd()
end

function M.dpm_root(bufnr)
  return vim.fs.root(M.directory(bufnr), 'project.dcon')
end

local function is_cached_package(root)
  if not root then return false end
  local hash = vim.fs.basename(root)
  return #hash == 64 and hash:match('^%x+$') ~= nil and vim.fs.basename(vim.fs.dirname(root)) == 'src'
end

function M.cached_package(bufnr)
  local root = M.dpm_root(bufnr)
  return is_cached_package(root) and root or nil
end

function M.owning_root(bufnr)
  if not M.cached_package(bufnr) then return nil end
  local function usable(client)
    local root = client.config.root_dir
    return root and not is_cached_package(root) and not client:is_stopped() and root or nil
  end
  local alternate = vim.fn.bufnr('#')
  if alternate > 0 and alternate ~= bufnr then
    for _, client in ipairs(vim.lsp.get_clients({ name = 'dccd', bufnr = alternate })) do
      if usable(client) then return usable(client) end
    end
  end
  local clients = vim.lsp.get_clients({ name = 'dccd' })
  table.sort(clients, function(a, b) return a.id < b.id end)
  for _, client in ipairs(clients) do
    if usable(client) then return usable(client) end
  end
  return nil
end

function M.root(bufnr, override)
  local root
  if type(override) == 'function' then root = override(bufnr)
  elseif type(override) == 'string' then root = override
  else
    local directory = M.directory(bufnr)
    root = M.owning_root(bufnr) or M.dpm_root(bufnr) or vim.fs.root(directory, 'compile_commands.json')
      or vim.fs.root(directory, 'dcc.json') or directory
  end
  if root == nil then return nil end
  return vim.fs.normalize(vim.uv.fs_realpath(root) or vim.fn.fnamemodify(root, ':p'))
end

return M
