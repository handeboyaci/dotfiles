vim.bo.textwidth = 80
vim.bo.shiftwidth = 2
vim.bo.softtabstop = 2
vim.bo.tabstop = 2

if vim.fn.executable("basedpyright-langserver") ~= 1 then
	return
end

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
	cmd = { "basedpyright-langserver", "--stdio" },
	root_dir = root_dir,
	single_file_support = true,
	on_attach = function(client, bufnr)
		client.server_capabilities.documentFormattingProvider = false
		client.server_capabilities.documentRangeFormattingProvider = false
	end,
})
