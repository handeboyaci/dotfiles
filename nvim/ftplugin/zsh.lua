local path = os.getenv("HOME") .. "/.local/share/nvim/mason/bin/"

vim.lsp.start({
	name = "Bash-LSP",
	cmd = { path .. "bash-language-server", "start" },
	single_file_support = true,
})
