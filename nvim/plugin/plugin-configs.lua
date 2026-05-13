local lazy = require("lazy_loader")
require("Comment").setup()

vim.api.nvim_create_autocmd("TextYankPost", {
	group = "vimrc",
	callback = function()
		vim.highlight.on_yank({ higroup = "Visual", timeout = 200 })
	end,
})

lazy.lazy_load("aerial.nvim", {
	keys = {
		{ "n", "<F9>", "<cmd>AerialToggle!<CR>" },
	},
	cmds = { "AerialToggle" },
	on_load = function()
		require("aerial").setup({
			on_attach = function(bufnr)
				vim.keymap.set("n", "<C-S-[>", "<cmd>AerialPrev<CR>", { buffer = bufnr })
				vim.keymap.set("n", "<C-S-]>", "<cmd>AerialNext<CR>", { buffer = bufnr })
			end,
		})
	end,
})

local remotefiles = require("remotefiles")

remotefiles.register_local("*:*:*", function(match)
	return vim.split(match, ":")[1]
end, function(match)
	local _, line, col = unpack(vim.split(match, ":"))
	vim.cmd(line)
	if col ~= nil then
		vim.cmd("normal " .. col .. "|")
	end
end)

-- Mason
lazy.lazy_load("mason.nvim", {
	cmds = { "Mason" },
	on_load = function()
		require("mason").setup()
	end,
})

-- Tmux Navigator
vim.g.tmux_navigator_disable_when_zoomed = 1

vim.keymap.set("t", "<C-h>", "<C-\\><C-n>:TmuxNavigateLeft<CR>", { silent = true })
vim.keymap.set("t", "<C-j>", "<C-\\><C-n>:TmuxNavigateDown<CR>", { silent = true })
vim.keymap.set("t", "<C-k>", "<C-\\><C-n>:TmuxNavigateUp<CR>", { silent = true })
vim.keymap.set("t", "<C-l>", "<C-\\><C-n>:TmuxNavigateRight<CR>", { silent = true })
vim.keymap.set("t", "<C-w>", "<C-\\><C-n><C-w>", { silent = true })
vim.keymap.set("t", "<C-x>", "<C-\\><C-n><C-w>c", { silent = true })

-- Surround
vim.g.surround_no_insert_mappings = 1

-- Undotree
lazy.lazy_load("undotree", {
	keys = {
		{ "n", "coz", "<cmd>UndotreeToggle<CR>" },
	},
	cmds = { "UndotreeToggle" },
})

-- Colorizer
lazy.lazy_load("nvim-colorizer.lua", {
	cmds = {
		"ColorizerAttachToBuffer",
		"ColorizerDetachFromBuffer",
		"ColorizerReloadAllBuffers",
		"ColorizerToggle",
	},
})

-- Signify
vim.g.signify_skip_filename_pattern = { [[\.pipertmp.*]] }

-- null-ls setup
local null_ls = require("null-ls")
local mason_path = os.getenv("HOME") .. "/.local/share/nvim/mason/bin/"

null_ls.setup({
	sources = {
		-- Python
		null_ls.builtins.formatting.pyink.with({
			extra_args = { "--pyink-indentation", "2" },
			command = mason_path .. "pyink",
		}),
		-- Lua
		null_ls.builtins.formatting.stylua.with({
			command = mason_path .. "stylua",
		}),
		-- Shell (sh/zsh)
		null_ls.builtins.formatting.shfmt.with({
			command = mason_path .. "shfmt",
			extra_args = { "--indent", 2 },
		}),
		-- Vim
		null_ls.builtins.diagnostics.vint.with({
			command = mason_path .. "vint",
		}),
	},
})
-- Fidget setup
require("fidget").setup({
	notification = {
		-- Using explicit fidget.notify in cider.lua
	},
})

-- VCS Navigation Mappings
vim.keymap.set("n", "[p", "<cmd>Parent<CR>", { desc = "VCS: Go to Parent commit" })
vim.keymap.set("n", "]p", "<cmd>Child<CR>", { desc = "VCS: Go to Child commit" })
vim.keymap.set("n", "[P", "<cmd>Parent!<CR>", { desc = "VCS: Go to Parent commit (exclusive)" })
vim.keymap.set("n", "]P", "<cmd>Child!<CR>", { desc = "VCS: Go to Child commit (exclusive)" })
