local buffer = require("google3.buffer")

local M = {}

M.setup = function(args)
	-- vim.opt.shada:append("r/google")

	-- We are setting this as global to make sure the quickfix windows also has cs.
	vim.o.grepprg = "cs --local --nostats"

	vim.api.nvim_create_user_command("Comments", require("google3.comments").load, { bang = true })
	vim.api.nvim_create_user_command("Snapshots", function(opts)
		local s = require("google3.snapshots")
		if opts.bang then
			s.load()
		else
			s.diff()
		end
	end, { bang = true })

	vim.cmd("compiler blaze")

	vim.api.nvim_create_augroup("google3", { clear = true })
	-- This is a hack to not save undo/swap files to disk. Unfortunately undodir and directory are global options. We
	-- cannot set them per buffer, so we keep setting them everytime we change windows between google3 and non-google3.
	-- local directory = vim.o.directory
	-- local undodir = vim.o.undodir
	-- vim.api.nvim_create_autocmd("BufWinEnter", {
	-- 	group = "google3",
	-- 	pattern = "*",
	-- 	nested = true, -- This enables running SwapExists autocmd if it exists.
	-- 	callback = function(args_)
	-- 		local bo = vim.bo[args_.buf]
	-- 		bo.undofile = false
	-- 		bo.swapfile = false
	--
	-- 		local filename = args_.match
	-- 		if vim.o.readonly or vim.fn.isdirectory(vim.fs.dirname(filename)) == 0 then
	-- 			return
	-- 		end
	--
	-- 		if vim.b[args_.buf].is_google3_file then
	-- 			vim.o.undodir = "."
	-- 			vim.o.directory = "."
	-- 		else
	-- 			vim.o.undodir = undodir
	-- 			vim.o.directory = directory
	-- 		end
	--
	-- 		bo.undofile = true
	-- 		bo.swapfile = true
	-- 	end,
	-- })

	vim.api.nvim_create_autocmd("FileType", {
		group = "google3",
		pattern = "cpp",
		callback = function()
			vim.bo.includeexpr = "google3#CppProtoHeader(v:fname)"
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

	--vim.api.nvim_create_user_command("Fix",

	-- The previous autocmd won't work for the first google3 file, so we do it separately
	buffer.setup(args)
end

return M
