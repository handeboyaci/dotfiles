M = {}

M.attach = function(citc_root, bufnr)
	local cmd = {
		"/google/bin/releases/cider/ciderlsp/ciderlsp",
		"--noforward_sync_responses",
		"--tooltag=neovim-lsp",
		"--cdpush_name=",
	}
	local proxy = os.getenv("PROD_PROXY")
	if proxy then
		table.insert(cmd, 1, proxy)
		table.insert(cmd, 1, "ssh")
	end
	vim.lsp.start({
		name = "Cider",
		cmd = cmd,
		root_dir = vim.fs.dirname(citc_root),
		-- offset_encoding = "utf-8",
		settings = {},
	})
end

return M
