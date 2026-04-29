local path = os.getenv("HOME") .. "/.local/share/nvim/mason/bin/"


vim.lsp.start({
	name = "vimls",
	cmd = { path .. "vim-language-server", "--stdio" },
	root_dir = vim.fs.dirname(vim.fs.find({
		".git",
		"autoload",
		"plugin",
		"runtime",
	}, {
		upward = true,
	})[1]),
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
