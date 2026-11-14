local M = {}
function M.check()
  vim.health.start('DC first-party integration')
  if vim.fn.has('nvim-0.11') == 1 then vim.health.ok('Neovim 0.11+ native LSP APIs available')
  else vim.health.error('Neovim 0.11 or newer is required'); return end
  for _, line in ipairs(require('dcc').snapshot()) do vim.health.info(line) end
  vim.health.info('Run :DccInfo to resolve executables; :DccRestart retries failed discovery.')
  if vim.fn.has('win32') == 1 then vim.health.warn('The current dcc MSI does not ship native Windows DPM. Configure a compatible executable separately.') end
end
return M
