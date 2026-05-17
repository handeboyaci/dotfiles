local async = require("sencer.async")

local M = {}

M.load = function(cmd_opts)
	if not vim.b.citc_name then
		print("Not in google3.")
		return
	end

	local cmd = "/google/bin/releases/editor-devtools/get_comments.par -wc 1000 --cc "
	if cmd_opts.bang then
		cmd = cmd .. "--file_comments"
	else
		cmd = cmd .. "--noresolved --file_comments"
	end

	local opts = {
		nr = "$",
		title = "Comments for " .. vim.b.citc_name,
		efm = "@%m,%f:%l:,%-G---%.%#",
		quickfixtextfunc = require("sencer.format").wrap(function(item)
			if item.bufnr > 0 then
				return vim.fn.pathshorten(vim.fn.bufname(item.bufnr)) .. ", line " .. item.lnum
			else
				return item.text
			end
		end),
		command = cmd,
		on_finish = function()
			local qflist = vim.fn.getqflist()
			if #qflist > 0 then
				pcall(vim.cmd, "cfirst")
			else
				print("No comments found.")
			end
		end,
	}

	local job = async.qf(opts)

	return job
end

M.load_all = function()
	if not vim.b.citc_name then
		print("Not in google3.")
		return
	end

	local cmd = "/google/bin/releases/editor-devtools/get_comments.par -wc 1000 --cc --cl_comments --file_comments"

	local opts = {
		nr = "$",
		title = "All Comments for " .. vim.b.citc_name,
		efm = "@%m,%f:%l:,%-G---%.%#",
		quickfixtextfunc = require("sencer.format").wrap(function(item)
			if item.bufnr > 0 then
				return vim.fn.pathshorten(vim.fn.bufname(item.bufnr)) .. ", line " .. item.lnum
			else
				return item.text
			end
		end),
		command = cmd,
		on_finish = function()
			local qflist = vim.fn.getqflist()
			if #qflist > 0 then
				pcall(vim.cmd, "cfirst")
			else
				print("No comments found.")
			end
		end,
	}

	local job = async.qf(opts)

	return job
end

return M
