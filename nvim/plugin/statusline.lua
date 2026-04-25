-- Highlight StatusLine
vim.api.nvim_set_hl(0, "StatusLine", { bg = "#8787af", fg = "#262626" })

-- Autocmds for ModeChanged
local statusline_group = vim.api.nvim_create_augroup("StatusLine", { clear = true })

vim.api.nvim_create_autocmd("ModeChanged", {
	group = statusline_group,
	pattern = "*:n*",
	callback = function()
		vim.api.nvim_set_hl(0, "StatusLine", { bg = "#8787af", fg = "#262626" })
	end,
})

vim.api.nvim_create_autocmd("ModeChanged", {
	group = statusline_group,
	pattern = "*:i",
	callback = function()
		vim.api.nvim_set_hl(0, "StatusLine", { bg = "#87af87", fg = "#262626" })
	end,
})

vim.api.nvim_create_autocmd("ModeChanged", {
	group = statusline_group,
	pattern = "*:R",
	callback = function()
		vim.api.nvim_set_hl(0, "StatusLine", { bg = "#af5f5f", fg = "#262626" })
	end,
})

-- StatusLine function
function _G.StatusLine()
	local home = os.getenv("HOME")
	local citc_root = os.getenv("CITC_ROOT")
	local citc_name = os.getenv("CITC_NAME")

	local workdir = vim.fn.getcwd()
	if home then
		workdir = workdir:gsub(home, "~")
	end
	if citc_root and citc_name then
		workdir = workdir:gsub(citc_root, citc_name)
	end

	local stl = "[%n][" .. workdir .. "] "

	local relpath = vim.fn.expand("%:h")
	if home then
		relpath = relpath:gsub(home, "~")
	end

	if relpath ~= "." and relpath ~= "" then
		stl = stl .. vim.fn.pathshorten(relpath) .. "/"
	end

	-- Call LspStatus from lsp-config.lua
	local lsp_status = ""
	if _G.LspStatus then
		lsp_status = _G.LspStatus()
	end

	stl = stl .. "%t%([%M%R%H%W]%)%=%(" .. lsp_status .. " |%) %P col %c %y"

	return stl
end

vim.opt.statusline = "%!v:lua.StatusLine()"
