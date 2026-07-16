local buffer = require("google3.buffer")
local cider = require("google3.cider")
local analysis = require("google3.analysis")

local M = {}

M.setup = function(args)
	require("google3.vcs_providers").setup()

	-- vim.opt.shada:append("r/google")

	-- We are setting this as global to make sure the quickfix windows also has cs.


	vim.api.nvim_create_user_command("Comments", require("google3.comments").load, { bang = true })
	vim.api.nvim_create_user_command("CommentsAll", require("google3.comments").load_all, {})

	vim.cmd("compiler blaze")



	vim.api.nvim_create_autocmd("FileType", {
		group = "google3",
		pattern = "cpp",
		callback = function()
			vim.bo.includeexpr = "v:lua.require('google3.utils').cpp_proto_header(v:fname)"
		end,
	})

	vim.api.nvim_create_autocmd("FileType", {
		group = "google3",
		pattern = "gcl",
		command = [[ let b:closer = 1 | let b:closer_flags = '([{' ]],
	})

	vim.api.nvim_create_autocmd({ "BufNewFile", "BufReadPre" }, {
		group = "google3",
		nested = true,
		pattern = "/google/src/cloud/*",
		callback = function(args_)
			buffer.setup(args_)
		end,
	})

	vim.api.nvim_create_autocmd("BufReadPost", {
		group = "google3",
		callback = function(args_)
			local bufnr = args_.buf
			if bufnr == nil or not vim.b[bufnr].is_google3_file then
				return
			end
			cider.attach(vim.b[bufnr].citc_root, bufnr)
			analysis.attach(vim.b[bufnr].citc_root, bufnr)
		end,
	})

	--vim.api.nvim_create_user_command("Fix",

	-- The previous autocmd won't work for the first google3 file, so we do it separately
	buffer.setup(args)
end

return M
