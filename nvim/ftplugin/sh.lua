if vim.fn.executable("bash-language-server") == 1 then
	vim.lsp.start({
		name = "Bash-LSP",
		cmd = { "bash-language-server", "start" },
		single_file_support = true,
	})
end

