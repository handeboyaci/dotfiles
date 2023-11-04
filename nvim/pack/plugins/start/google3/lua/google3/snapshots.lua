local util = require("google3.util")
local async = require("sencer.async")

local M = {}

local script_tmpl = [[
citc_root="%s"
citc_path="%s"
file="$citc_root/google3/$citc_path"
citctools filelog -r "$file" | tail -n +3 | while read -a line
do
  snapshot="$citc_root/.snapshot/${line[0]}/google3/$citc_path"
  if cmp -s -- "$file" "$snapshot"
  then
    continue
  fi
  echo "$snapshot: Snapshot ${line[0]} (${line[1]} ${line[2]}, ${line[3]} bytes.)"
done
]]

M.load = function(citc_path, citc_root)
	citc_path = citc_path or vim.b.citc_path
	if citc_path == nil then
		print("File doesn't seem to be a part of CITC.")
		return
	end
	citc_root = citc_root or vim.fs.dirname(vim.b.citc_root)

	local efm = "%f: %m"

	-- Start with baseline, if it exists.
	local lines = {}
	local baseline = util.joinpath(citc_root, ".citc/baseline/google3", citc_path)
	if vim.fn.filereadable(baseline) == 1 then
		lines = { baseline .. ": Baseline" }
	end

	local opts = {
		nr = "$",
		title = "Snapshots for " .. citc_path,
		efm = efm,
		quickfixtextfunc = require("sencer.format").text,
		lines = lines,
		command = script_tmpl:format(citc_root, citc_path),
		run_shell = true,
	}

	local job = async.qf(opts)

	return job
end

M.diff = function()
	local citc_path = vim.b.citc_path
	if not citc_path then
		print("Not a google3 file.")
		return
	end
	local citc_root = vim.fs.dirname(vim.b.citc_root)

	M.load(citc_path, citc_root)

	local bufnr = vim.fn.bufnr()

	vim.api.nvim_create_augroup("Snapshots", { clear = true })
	vim.api.nvim_create_autocmd("BufReadCmd", {
		group = "Snapshots",
		pattern = {
			util.joinpath(citc_root, ".citc/baseline/google3", citc_path),
			util.joinpath(citc_root, ".snapshot/*", citc_path),
		},
		-- For some reason, not having <afile> before switching b confuses the editor.
		command = "call expand('<afile>')|bd|only|b"
			.. bufnr
			.. "|vert diffsplit <afile>|doautocmd FileType|setl bufhidden=delete",
	})
end

return M
