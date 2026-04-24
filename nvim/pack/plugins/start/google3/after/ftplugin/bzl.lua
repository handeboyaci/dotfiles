vim.keymap.set('n', 'gf', [[f"h"oyiwF"gf:call search("name = \"<C-R><C-R>o\"")<CR>]], { buffer = true, silent = true })

-- Trim // from the beginning; if there are no slices left re-open this file as a
-- hack, otherwise open the BUILD file in the v:fname path.
vim.opt_local.includeexpr = "v:lua.require('google3.utils').bzl_include_expr(v:fname)"
