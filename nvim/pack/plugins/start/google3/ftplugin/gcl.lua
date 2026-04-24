-- hyphen(-) is a valid identifier character, often used in varz names.
vim.opt_local.isident:append('-')

vim.opt_local.formatoptions:remove('t')
vim.opt_local.comments = '://'
vim.opt_local.commentstring = '// %s'

-- Make gf work with absolute imports
vim.opt_local.includeexpr = "substitute(v:fname,'//','','')"
vim.opt_local.include = "^import '//"

-- Enable syntax-based folding, if specified.
vim.opt_local.foldmethod = 'syntax'
vim.opt_local.foldtext = "v:lua.require('google3.utils').gcl_fold()"
