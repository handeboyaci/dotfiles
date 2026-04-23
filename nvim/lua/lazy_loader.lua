local M = {}

function M.lazy_load(plugin_name, opts)
	opts = opts or {}
	local keys = opts.keys or {}
	local on_load = opts.on_load

	for _, key_cfg in ipairs(keys) do
		local mode, lhs, rhs = unpack(key_cfg)

		-- Set up dummy mapping
		vim.keymap.set(mode, lhs, function()
			-- 1. Load the plugin
			vim.cmd("packadd " .. plugin_name)

			-- 2. Run on_load if provided
			if on_load then
				on_load()
			end

			-- 3. Execute the actual action intended by the trigger
			if type(rhs) == "function" then
				rhs()
			elseif type(rhs) == "string" then
				vim.cmd(rhs)
			end

			-- 4. Re-bind the key to the actual action so next time it doesn't load again
			vim.keymap.set(mode, lhs, rhs, { noremap = true, silent = true })
		end, { noremap = true, silent = true })
	end
end

return M
