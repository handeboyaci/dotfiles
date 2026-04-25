local M = {}

function M.redir(cmd, rng, start, end_line)
	-- Close existing scratch windows
	for win = 1, vim.fn.winnr("$") do
		if vim.fn.getwinvar(win, "scratch") == 1 then
			vim.cmd(win .. "windo close")
		end
	end

	local output
	if cmd:sub(1, 1) == "!" then
		-- Shell command
		local real_cmd = cmd:sub(2)
		if real_cmd:match(" %%") then
			real_cmd = real_cmd:gsub(" %%", " " .. vim.fn.expand("%:p"))
		end

		if rng == 0 then
			output = vim.fn.systemlist(real_cmd)
		else
			local joined_lines = table.concat(vim.fn.getline(start, end_line), "\n")
			local cleaned_lines = vim.fn.shellescape(joined_lines):gsub("'\\''", "\\'")
			output = vim.fn.systemlist(real_cmd .. " <<< $" .. cleaned_lines)
		end
	else
		-- Vim command
		output = vim.api.nvim_exec2(cmd, { output = true }).output
		output = vim.split(output, "\n")
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
