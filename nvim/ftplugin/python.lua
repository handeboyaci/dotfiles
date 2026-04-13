vim.bo.textwidth = 80
vim.bo.shiftwidth = 2
vim.bo.softtabstop = 2
vim.bo.tabstop = 2

local null_ls = require("null-ls")
local path = os.getenv("HOME") .. "/.local/share/nvim/mason/bin/"

null_ls.setup({
	sources = {
		null_ls.builtins.formatting.pyink.with({
			extra_args = { "--pyink-indentation", "2" },
			command = path .. "pyink",
		}),
	},
})
