if vim.fn.getcmdwintype() ~= "" or vim.fn.executable("vim-language-server") ~= 1 then
	return
end

vim.lsp.start({
	name = "vimls",
	cmd = { "vim-language-server", "--stdio" },
	root_dir = vim.fs.root(0, { ".git", "autoload", "plugin", "runtime" }),
	single_file_support = true,
	init_options = {
		isNeovim = true,
		iskeyword = "@,48-57,_,192-255,-#",
		vimruntime = "",
		runtimepath = vim.o.runtimepath,
		diagnostic = { enable = true },
		indexes = {
			runtimepath = true,
			gap = 200,
			count = 2,
			projectRootPatterns = { "runtime", "nvim", ".git", "autoload", "plugin" },
		},
	},
})
