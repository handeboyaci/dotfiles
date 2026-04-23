M = {}

M.attach = function(citc_root, bufnr)
  local cmd = { '/google/bin/users/lerm/glint-ale/analysis_lsp/server', '--lint_on_save=false', '--max_qps=10' }
	local proxy = os.getenv("PROD_PROXY")
	if proxy then
		table.insert(cmd, 1, proxy)
		table.insert(cmd, 1, "ssh")
	end
	vim.lsp.start({
		name = "Analysis",
		cmd = cmd,
		root_dir = vim.fs.dirname(citc_root),
		settings = {},
	})
end

return M
