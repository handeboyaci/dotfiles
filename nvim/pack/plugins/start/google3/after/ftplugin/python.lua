

vim.opt_local.isfname:remove(',')
vim.opt_local.include = [[^\s*\zs\(from \f\+ \)\?import \f\+\ze]]
vim.opt_local.includeexpr = "v:lua.require('google3.utils').python_include_expr(v:fname)"
vim.opt_local.suffixesadd:append('/__init__.py')

vim.keymap.set('n', 'gf', function()
  local line = vim.fn.getline('.')
  local include_regex = vim.opt_local.include:get()
  if vim.fn.match(line, include_regex) ~= -1 then
    return 'Vgf'
  else
    return 'gf'
  end
end, { buffer = true, expr = true })
