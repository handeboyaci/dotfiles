vim.bo.textwidth = 120

local null_ls = require("null-ls")
local path = os.getenv("HOME") .. "/.local/share/nvim/mason/bin/"
local runtime_path = vim.split(package.path, ";")

table.insert(runtime_path, "lua/?.lua")
table.insert(runtime_path, "lua/?/init.lua")

null_ls.setup({
	sources = {
		null_ls.builtins.formatting.stylua.with({
			command = path .. "stylua",
		}),
	},
})

vim.lsp.start({
	name = "lua-lsp",
	cmd = { path .. "lua-language-server" },
	single_file_support = true,
	root_dir = vim.fn.finddir("lua", ".;"),
	settings = {
		Lua = {
			runtime = {
				version = "LuaJIT",
				path = runtime_path,
			},
			telemetry = {
				enable = false,
			},
			diagnostics = {
				globals = { "vim" },
			},
			workspace = {
				library = vim.api.nvim_get_runtime_file("", true),
				checkThirdParty = false,
			},
			format = {
				enable = false,
			},
		},
	},
})
