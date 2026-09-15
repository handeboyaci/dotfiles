local home = os.getenv("HOME")
local citc_root = os.getenv("CITC_ROOT")
local citc_name = os.getenv("CITC_NAME")

local function escape_pattern(str)
	return (str:gsub("[%(%)%.%%%+%-%*%?%[%]%^%$]", "%%%1"))
end

local home_pattern = home and escape_pattern(home)
local citc_root_pattern = citc_root and escape_pattern(citc_root)

-- Highlight StatusLine
vim.api.nvim_set_hl(0, "StatusLine", { bg = "#8787af", fg = "#262626" })

-- Autocmds for ModeChanged & DirChanged
local statusline_group = vim.api.nvim_create_augroup("StatusLine", { clear = true })

local cached_workdir = ""
local function update_workdir()
	local workdir = vim.fn.getcwd()
	if home_pattern then
		workdir = workdir:gsub(home_pattern, "~")
	end
	if citc_root_pattern and citc_name then
		workdir = workdir:gsub(citc_root_pattern, citc_name)
	end
	cached_workdir = workdir
end
update_workdir() -- Initial run

vim.api.nvim_create_autocmd("DirChanged", {
	group = statusline_group,
	callback = update_workdir,
})

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
	local stl = "[%n][" .. cached_workdir .. "] "

	local relpath = vim.fn.expand("%:h")
	if home_pattern then
		relpath = relpath:gsub(home_pattern, "~")
	end

	if relpath ~= "." and relpath ~= "" then
		stl = stl .. vim.fn.pathshorten(relpath) .. "/"
	end

	local diag_status = vim.diagnostic.status()
	local diag_part = (diag_status ~= "" and (" " .. diag_status .. " |") or "")
	stl = stl .. "%t%([%M%R%H%W]%)%=" .. diag_part .. " %P col %c %y"

	return stl
end

vim.opt.statusline = "%!v:lua.StatusLine()"
