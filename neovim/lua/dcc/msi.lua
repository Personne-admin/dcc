local M = {}

M.upgrade_code = "BC06D996-A013-4321-9967-7BCEDBBD011B"
M.query = [=[
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
function Property($object, $name, [object[]]$arguments) {
    $object.GetType().InvokeMember($name, [Reflection.BindingFlags]::GetProperty, $null, $object, $arguments)
}
function Method($object, $name, [object[]]$arguments) {
    $object.GetType().InvokeMember($name, [Reflection.BindingFlags]::InvokeMethod, $null, $object, $arguments)
}
$installer = New-Object -ComObject WindowsInstaller.Installer
$products = Property $installer 'RelatedProducts' @('{BC06D996-A013-4321-9967-7BCEDBBD011B}')
$results = @()
foreach ($product in $products) {
    try {
        if ((Property $installer 'ProductState' @($product)) -ne 5) { continue }
        $version = Property $installer 'ProductInfo' @($product, 'VersionString')
        $location = $null
        try { $location = Property $installer 'ProductInfo' @($product, 'InstallLocation') } catch { }
        $server = $null
        if ($location) { $server = [IO.Path]::Combine($location, 'bin', 'dccd.exe') }
        if (-not $server -or -not [IO.File]::Exists($server)) {
            $package = Property $installer 'ProductInfo' @($product, 'LocalPackage')
            $database = Method $installer 'OpenDatabase' @($package, 0)
            $view = Method $database 'OpenView' @('SELECT `ComponentId` FROM `Component` WHERE `Component` = ''c_bin_dccd_exe''')
            Method $view 'Execute' @() | Out-Null
            $record = Method $view 'Fetch' @()
            if ($record) {
                $component = Property $record 'StringData' @(1)
                $server = Property $installer 'ComponentPath' @($product, $component)
            }
            Method $view 'Close' @() | Out-Null
        }
        if ($server) {
            $results += @{ productCode = $product; upgradeCode = 'BC06D996-A013-4321-9967-7BCEDBBD011B'; version = $version; serverPath = $server }
        }
    } catch { }
}
ConvertTo-Json -InputObject @($results) -Compress
]=]

function M.discover(env, system, callback)
  local root
  for key, value in pairs(env) do if key:lower() == 'systemroot' then root = value end end
  if not root or not root:match('^%a:[/\\]') then
    callback(nil, 'SystemRoot unavailable; cannot query Windows Installer')
    return
  end
  local script = M.query:gsub('.', function(c) return c .. '\0' end)
  local command = { root .. '/System32/WindowsPowerShell/v1.0/powershell.exe',
    '-NoLogo', '-NoProfile', '-NonInteractive', '-EncodedCommand', vim.base64.encode(script) }
  local ok, err = pcall(system, command, { text = true, timeout = 5000 }, function(result)
    vim.schedule(function()
      if result.code ~= 0 then
        callback(nil, 'Windows Installer query failed (exit ' .. tostring(result.code) .. ')')
        return
      end
      local parsed, entries = pcall(vim.json.decode, ((result.stdout or ''):gsub('^\239\187\191', '')))
      if not parsed or type(entries) ~= 'table' or not vim.islist(entries) then
        callback(nil, 'Invalid Windows Installer query response')
        return
      end
      local valid = {}
      for _, entry in ipairs(entries) do
        if type(entry) == 'table' and type(entry.upgradeCode) == 'string'
          and entry.upgradeCode:upper() == M.upgrade_code
          and type(entry.productCode) == 'string' and entry.productCode:match('^%{[%x%-]+%}$')
          and type(entry.version) == 'string' and type(entry.serverPath) == 'string'
          and (entry.serverPath:match('^%a:[/\\]') or entry.serverPath:match('^[/\\][/\\]'))
          and entry.serverPath:lower():match('[/\\]dccd%.exe$') then
          valid[#valid + 1] = entry
        end
      end
      table.sort(valid, function(a, b)
        local av, bv = {}, {}
        for part in a.version:gmatch('%d+') do av[#av + 1] = tonumber(part) end
        for part in b.version:gmatch('%d+') do bv[#bv + 1] = tonumber(part) end
        for i = 1, 3 do
          if (av[i] or 0) ~= (bv[i] or 0) then return (av[i] or 0) > (bv[i] or 0) end
        end
        return a.serverPath < b.serverPath
      end)
      callback(valid)
    end)
  end)
  if not ok then callback(nil, 'Cannot start Windows Installer query: ' .. tostring(err)) end
end

return M
