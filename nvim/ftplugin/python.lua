vim.bo.textwidth = 88
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

vim.lsp.start({
	name = "Pyright",
	cmd = {
		path .. "pyright-langserver",
		"--stdio",
	},
	single_file_support = true,
	on_attach = function(client, bufnr)
		-- if vim.startswith(vim.uri_from_bufnr(bufnr), "file:///google/src") then
		-- 	client.stop()
		-- end
		client.server_capabilities.documentFormattingProvider = false
		client.server_capabilities.documentRangeFormattingProvider = false
	end,
})
