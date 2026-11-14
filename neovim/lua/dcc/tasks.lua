local M = { jobs = {}, last = nil }

local function display(lines, title)
  local buffer = vim.api.nvim_create_buf(false, true)
  vim.api.nvim_buf_set_name(buffer, 'dcc://dpm/' .. buffer)
  vim.bo[buffer].buftype = 'nofile'
  vim.bo[buffer].bufhidden = 'wipe'
  vim.bo[buffer].swapfile = false
  vim.api.nvim_buf_set_lines(buffer, 0, -1, false, lines)
  vim.bo[buffer].modifiable = false
  vim.cmd('botright 10split')
  vim.api.nvim_win_set_buf(0, buffer)
  M.last = { buffer = buffer, title = title }
end

function M.diagnostics(output, root)
  local entries, message, kind = {}, '', 'E'
  for line in (output .. '\n'):gmatch('(.-)\n') do
    line = line:gsub('\27%[[%d;]*m', '')
    local severity, text = line:match('^%s*(%a+):%s*(.*)')
    if severity == 'error' or severity == 'warning' then
      message, kind = text, severity == 'error' and 'E' or 'W'
    end
    local file, row, column = line:match('^%s*%-%->%s*(.-):(%d+):(%d+)%s*$')
    local gcc_file, gcc_row, gcc_column, gcc_text = line:match('^(.-):(%d+):(%d+):%s*(.*)$')
    if gcc_file then file, row, column, message = gcc_file, gcc_row, gcc_column, gcc_text end
    if file then
      if not require('dcc.discovery').is_absolute(file, vim.fn.has('win32') == 1) then file = root .. '/' .. file end
      entries[#entries + 1] = { filename = file, lnum = tonumber(row), col = tonumber(column), text = message, type = kind }
    end
  end
  return entries
end

function M.run(action, arguments, options)
  local root = require('dcc.project').dpm_root(vim.api.nvim_get_current_buf())
  if not root then vim.notify('No project.dcon for this buffer', vim.log.levels.ERROR); return end
  if M.jobs[root] then vim.notify('A DPM command is already running in ' .. root, vim.log.levels.WARN); return end
  require('dcc.discovery').resolve({ path = options.dpm, name = 'dpm' }, function(selected, error)
    if not selected then vim.notify(error, vim.log.levels.ERROR); return end
    local argv = { selected.path, action }
    vim.list_extend(argv, arguments)
    local chunks, bytes, truncated = {}, 0, false
    local function capture(_, data)
      if not data then return end
      local room = 128 * 1024 - bytes
      if #data > room then truncated = true end
      if room > 0 then
        local piece = data:sub(1, room)
        chunks[#chunks + 1], bytes = piece, bytes + #piece
      end
    end
    local job = { argv = argv, root = root }
    M.jobs[root] = job
    local ok, process = pcall(vim.system, argv, { cwd = root, text = true, stdout = capture, stderr = capture }, function(result)
      vim.schedule(function()
        M.jobs[root] = nil
        if job.stopping then return end
        local output = table.concat(chunks):gsub('\27%[[%d;]*m', '')
        if truncated then output = output .. '\n[output truncated at 128 KiB]' end
        local title = 'DPM ' .. action .. ' (exit ' .. result.code .. ')'
        vim.fn.setqflist({}, ' ', { title = title, items = M.diagnostics(output, root) })
        if result.code ~= 0 or action == 'run' then
          local lines = { title, 'Project: ' .. root, '' }
          vim.list_extend(lines, vim.split(output, '\n', { plain = true }))
          display(lines, title)
        end
        vim.notify(title, result.code == 0 and vim.log.levels.INFO or vim.log.levels.ERROR)
      end)
    end)
    if ok then job.process = process
    else M.jobs[root] = nil; vim.notify('Cannot start DPM: ' .. tostring(process), vim.log.levels.ERROR) end
  end)
end

function M.stop()
  for _, job in pairs(M.jobs) do
    job.stopping = true
    if job.process then job.process:kill(15) end
  end
end

return M
