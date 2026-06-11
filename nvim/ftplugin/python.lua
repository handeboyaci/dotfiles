vim.bo.textwidth = 80
vim.bo.shiftwidth = 2
vim.bo.softtabstop = 2
vim.bo.tabstop = 2

local mason_venv_path = vim.fn.stdpath("data") .. "/mason/packages/basedpyright/venv/bin/basedpyright-langserver"
local root_dir = vim.fs.root(0, {
	"basedpyrightconfig.json",
	"pyrightconfig.json",
	"pyproject.toml",
	"setup.py",
	"setup.cfg",
	"requirements.txt",
	"uv.lock",
	".git",
})

vim.lsp.start({
	name = "BasedPyright",
	cmd = { mason_venv_path, "--stdio" },
	root_dir = root_dir,
	single_file_support = true,
	on_attach = function(client, bufnr)
		client.server_capabilities.documentFormattingProvider = false
		client.server_capabilities.documentRangeFormattingProvider = false
	end,
})
