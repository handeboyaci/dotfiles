vim.opt_local.formatoptions:remove('t')
vim.opt_local.comments = '://'
vim.opt_local.commentstring = '// %s'

-- Make gf work with imports in Borg files
vim.opt_local.includeexpr = "substitute(v:fname,'//','','')"
vim.opt_local.include = "^import '//"

-- Enable syntax-based folding, if specified.
vim.opt_local.foldmethod = 'syntax'
vim.opt_local.foldtext = "v:lua.require('google3.utils').gcl_fold()"
