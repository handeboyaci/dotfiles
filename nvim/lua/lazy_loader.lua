local M = {}
local loaded = {}

function M.lazy_load(plugin_name, opts)
	opts = opts or {}
	local keys = opts.keys or {}
	local on_load = opts.on_load

	for _, key_cfg in ipairs(keys) do
		local mode, lhs, rhs = unpack(key_cfg)

		-- Set up dummy mapping
		vim.keymap.set(mode, lhs, function()
			-- 1. Load the plugin
			if not loaded[plugin_name] then
				vim.cmd("packadd " .. plugin_name)
				if on_load then
					on_load()
				end
				loaded[plugin_name] = true
			end

			-- 2. Execute the actual action intended by the trigger
			if type(rhs) == "function" then
				rhs()
			elseif type(rhs) == "string" then
				if rhs:match("^<cmd>") or rhs:match("^:") then
					local cmd = rhs:gsub("^<cmd>", ""):gsub("^:", ""):gsub("<CR>$", "")
					vim.cmd(cmd)
				else
					vim.cmd(rhs)
				end
			end

			-- 3. Re-bind the key to the actual action so next time it doesn't load again
			vim.keymap.set(mode, lhs, rhs, { noremap = true, silent = true })
		end, { noremap = true, silent = true })
	end

	local cmds = opts.cmds or {}
	for _, cmd in ipairs(cmds) do
		vim.api.nvim_create_user_command(cmd, function(args)
			vim.api.nvim_del_user_command(cmd)
			if not loaded[plugin_name] then
				vim.cmd("packadd " .. plugin_name)
				if on_load then
					on_load()
				end
				loaded[plugin_name] = true
			end
			vim.cmd(cmd .. " " .. args.args)
		end, { nargs = "*" })
	end
end

return M
