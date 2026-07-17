local M = {}

function M.redir(cmd, rng, start, end_line)
	if vim.fn.getcmdwintype() ~= "" then
		vim.notify("Redir cannot be executed from within the command-line window", vim.log.levels.ERROR)
		return
	end

	-- Close existing scratch windows
	for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
		local success, is_scratch = pcall(vim.api.nvim_win_get_var, win, "scratch")
		if success and is_scratch == 1 then
			vim.api.nvim_win_close(win, true)
		end
	end

	local output
	if cmd:sub(1, 1) == "!" then
		-- Shell command
		local real_cmd = cmd:sub(2)
		if real_cmd:match(" %%") then
			real_cmd = real_cmd:gsub(" %%", " " .. vim.fn.shellescape(vim.fn.expand("%:p")))
		end

		if rng == 0 then
			output = vim.fn.systemlist(real_cmd)
		else
			local joined_lines = table.concat(vim.fn.getline(start, end_line), "\n")
			output = vim.fn.systemlist(real_cmd, joined_lines)
		end
	else
		-- Vim command
		local ok, res = pcall(vim.api.nvim_exec2, cmd, { output = true })
		if not ok then
			vim.notify("Redir error executing '" .. cmd .. "': " .. tostring(res), vim.log.levels.ERROR)
			return
		end
		output = vim.split(res.output, "\n")
	end

	vim.cmd("vnew")
	local win_id = vim.api.nvim_get_current_win()
	vim.api.nvim_win_set_var(win_id, "scratch", 1)
	vim.bo.buftype = "nofile"
	vim.bo.bufhidden = "wipe"
	vim.bo.buflisted = false
	vim.bo.swapfile = false

	vim.api.nvim_buf_set_lines(0, 0, -1, false, output)
end

return M
