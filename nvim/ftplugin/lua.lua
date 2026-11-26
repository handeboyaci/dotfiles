vim.bo.textwidth = 120

if vim.fn.executable("lua-language-server") ~= 1 then
	return
end

local runtime_path = vim.split(package.path, ";")

table.insert(runtime_path, "lua/?.lua")
table.insert(runtime_path, "lua/?/init.lua")

vim.lsp.start({
	name = "lua-lsp",
	cmd = { "lua-language-server" },
	single_file_support = true,
	root_dir = vim.fs.root(0, { ".luarc.json", ".luarc.jsonc", "lua", ".git" }),
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
				library = vim.api.nvim_get_runtime_file("lua", true),
				checkThirdParty = false,
			},
			format = {
				enable = false,
			},
		},
	},
})
