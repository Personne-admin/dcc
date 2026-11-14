local M = {}

function M.directory(bufnr)
  if vim.bo[bufnr].buftype ~= '' then return vim.uv.cwd() end
  local name = vim.api.nvim_buf_get_name(bufnr)
  return name ~= '' and vim.fs.dirname(name) or vim.uv.cwd()
end

function M.dpm_root(bufnr)
  return vim.fs.root(M.directory(bufnr), 'project.dcon')
end

function M.root(bufnr, override)
  local root
  if type(override) == 'function' then root = override(bufnr)
  elseif type(override) == 'string' then root = override
  else
    local directory = M.directory(bufnr)
    root = M.dpm_root(bufnr) or vim.fs.root(directory, 'compile_commands.json')
      or vim.fs.root(directory, 'dcc.json') or directory
  end
  if root == nil then return nil end
  return vim.fs.normalize(vim.uv.fs_realpath(root) or vim.fn.fnamemodify(root, ':p'))
end

return M
