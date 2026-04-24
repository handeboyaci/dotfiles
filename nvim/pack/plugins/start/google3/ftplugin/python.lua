if vim.b.citc_root then
  vim.opt_local.path = '.,' .. vim.fn.fnamemodify(vim.b.citc_root, ':h') .. ',' .. vim.b.citc_root .. '/third_party/py'
end
