local null_ls = require("null-ls")
local path = os.getenv("HOME") .. "/.local/share/nvim/mason/bin/"
null_ls.setup({
	sources = {
		null_ls.builtins.formatting.shfmt.with({
			command = path .. "shfmt",
			extra_args = {
				"--indent",
				2,
			},
		}),
		null_ls.builtins.formatting.shellharden.with({
			command = path .. "shellharden",
		}),
		null_ls.builtins.code_actions.shellcheck.with({
			command = path .. "shellcheck",
		}),
		null_ls.builtins.formatting.beautysh.with({
			command = path .. "beautysh",
		}),
	},
})

vim.lsp.start({
	name = "Bash-LSP",
	cmd = { path .. "bash-language-server", "start" },
	single_file_support = true,
})
