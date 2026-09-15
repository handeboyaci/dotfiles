local M = {}
local async = require("sencer.async")

local function show_output(output)
	-- Close existing scratch windows
	for _, win in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
		local success, is_scratch = pcall(vim.api.nvim_win_get_var, win, "scratch")
		if success and is_scratch == 1 then
			vim.api.nvim_win_close(win, true)
		end
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

function M.redir(cmd, rng, start, end_line)
	if vim.fn.getcmdwintype() ~= "" then
		vim.notify("Redir cannot be executed from within the command-line window", vim.log.levels.ERROR)
		return
	end

	if cmd:sub(1, 1) == "!" then
		-- Shell command
		local real_cmd = cmd:sub(2)
		if real_cmd:match(" %%") then
			real_cmd = real_cmd:gsub(" %%", " " .. vim.fn.shellescape(vim.fn.expand("%:p")))
		end

		local stdin_text
		if rng ~= 0 then
			stdin_text = table.concat(vim.fn.getline(start, end_line), "\n")
		end

		vim.async.run(function()
			local obj = async.exec(real_cmd, { stdin = stdin_text, text = true })
			local output = vim.split(obj.stdout or "", "\n", { plain = true })
			if output[#output] == "" then
				table.remove(output)
			end
			if obj.stderr and obj.stderr ~= "" then
				for _, line in ipairs(vim.split(obj.stderr, "\n", { plain = true })) do
					if line ~= "" then
						table.insert(output, line)
					end
				end
			end
			show_output(output)
		end)
	else
		-- Vim command
		local ok, res = pcall(vim.api.nvim_exec2, cmd, { output = true })
		if not ok then
			vim.notify("Redir error executing '" .. cmd .. "': " .. tostring(res), vim.log.levels.ERROR)
			return
		end
		local output = vim.split(res.output, "\n")
		show_output(output)
	end
end

return M
