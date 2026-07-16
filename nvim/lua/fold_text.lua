local M = {}

function M.foldtext()
	local reg = "{{{" .. "\\d\\="
	local commentstring = vim.bo.commentstring
	for _, c in ipairs(vim.split(commentstring, "%s", { plain = true })) do
		if c ~= "" then
			reg = reg .. "\\|" .. vim.fn.escape(c, "*")
		end
	end

	local fillchars = vim.o.fillchars
	local foldchar = vim.fn.matchstr(fillchars, "fold:\\zs.")
	if foldchar == "" then
		foldchar = " "
	end
	local wlength = math.min(vim.api.nvim_win_get_width(0), 80)
	local indent = math.max(vim.fn.indent(vim.v.foldstart), vim.v.foldlevel - 1)
	local nlines = (vim.v.foldend - vim.v.foldstart + 1) .. " lines "

	local ftext = vim.fn.getline(vim.v.foldstart)
	-- Use vim.fn.substitute to match Vim regex behavior accurately
	ftext = vim.fn.substitute(ftext, reg, "", "g")

	local trimmed_ftext = ftext:gsub("^%s*(.-)%s*$", "%1")
	local text_width = vim.fn.strdisplaywidth(trimmed_ftext)
	local nlines_width = vim.fn.strdisplaywidth(nlines)
	local fill_count = math.max(0, wlength - nlines_width - text_width - 2 - indent)

	local result = string.rep(" ", indent)
		.. trimmed_ftext
		.. string.rep(foldchar, fill_count)
		.. " "
		.. nlines
		.. "≡"

	return result
end

return M
