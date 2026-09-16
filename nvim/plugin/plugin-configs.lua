local lazy = require("lazy_loader")

vim.api.nvim_create_autocmd({ "TextYankPost", "TextPutPost" }, {
	group = "vimrc",
	callback = function()
		vim.hl.hl_op({ higroup = "Visual", timeout = 200 })
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

remotefiles.register({ "/placer/*", "/google_src/*" }, function(match)
	return "fileutil cat " .. match
end)

remotefiles.register("/cns/*", function(match)
	return "fileutil cat " .. match
end, function(match)
	return "fileutil tee -f -output " .. match
end)

remotefiles.register_local("//depot/google3/*", function(match)
	local client_path = os.getenv("PWD"):gsub("google3/?.*", "google3/")
	local rel_path = match:sub(1 + #"//depot/google3/")
	return client_path .. rel_path
end)

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
	cmds = { "Mason", "MasonInstall" },
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

-- Undotree (built-in, lazy-loaded)
lazy.lazy_load("nvim.undotree", {
	keys = {
		{ "n", "coz", "<cmd>Undotree<CR>" },
	},
	cmds = { "Undotree" },
})

-- TOhtml (built-in, lazy-loaded)
lazy.lazy_load("nvim.tohtml", {
	cmds = { "TOhtml" },
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

-- Conform (formatting)
local conform = require("conform")
conform.setup({
	formatters_by_ft = {
		lua = { "stylua" },
		python = { "pyink" },
		sh = { "shfmt" },
		zsh = { "shfmt" },
	},
	formatters = {
		pyink = {
			prepend_args = { "--pyink-indentation", "2" },
			range_args = function(self, ctx)
				return {
					"--line-ranges",
					string.format("%d-%d", ctx.range.start[1], ctx.range["end"][1]),
					"--stdin-filename",
					"$FILENAME",
					"--quiet",
					"-",
				}
			end,
		},
		shfmt = {
			prepend_args = { "-i", "2" },
		},
	},
})

vim.o.formatexpr = "v:lua.require'conform'.formatexpr()"

vim.keymap.set({ "n", "v" }, "gQ", function()
	conform.format({ async = true, lsp_format = "fallback" })
end, { desc = "Format buffer" })
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
