local async = require("sencer.async")
local vcs = require("vcs")

local M = {}

local prefix = "/google/src/cloud"

local function get_citc_root(dir)
	local components = vim.split(dir:sub(prefix:len() + 2), "/", { trimempty = true })
	if #components < 2 then
		return nil
	end
	return prefix .. "/" .. components[1] .. "/" .. components[2] .. "/google3"
end

-- G4 Provider
local g4 = {}
function g4.detect(dir)
	dir = vim.fn.fnamemodify(dir, ":p")
	if not vim.startswith(dir, prefix) then
		return false
	end
	local citc_root = get_citc_root(dir)
	if not citc_root then
		return false
	end
	return vim.fn.filereadable(citc_root .. "/../.citc/p4_client_name") == 1
end

function g4.get_modified_files(dir, callback)
	async.run_cmd({
		command = "g4 whatsout",
		on_exit = function(obj)
			if obj.code ~= 0 then
				callback({})
				return
			end
			local files = {}
			for _, line in ipairs(vim.split(obj.stdout or "", "\n", { plain = true })) do
				if line ~= "" and not line:match("^#") then
					table.insert(files, line)
				end
			end
			callback(files)
		end,
	})
end

local function get_google3_primary_states(file, callback)
	file = vim.fn.fnamemodify(file, ":p")
	local citc_root = get_citc_root(vim.fs.dirname(file))
	if not citc_root then
		callback({}, nil)
		return
	end
	local citc_path = file:sub(#citc_root + 2)
	local baseline_path = citc_root:gsub("/google3$", "") .. "/.citc/baseline/google3/" .. citc_path

	local entries = {}
	if vim.fn.filereadable(baseline_path) == 1 then
		table.insert(entries, {
			uri = "vcs://google3/baseline/baseline/" .. citc_path,
			display = "Baseline",
			rev = "baseline",
		})
	else
		table.insert(entries, {
			uri = "vcs://google3/baseline/empty/" .. citc_path,
			display = "Baseline (Empty)",
			rev = "empty",
		})
	end
	callback(entries, nil)
end

local function get_google3_secondary_states(file, callback)
	file = vim.fn.fnamemodify(file, ":p")
	local citc_root = get_citc_root(vim.fs.dirname(file))
	if not citc_root then
		callback({}, nil)
		return
	end
	local citc_path = file:sub(#citc_root + 2)

	local script = [[
citc_root="%s"
citc_path="%s"
file="$citc_root/google3/$citc_path"
citctools filelog -r "$file" | tail -n +3 | while read rev date time size wx
do
  test "$size" -eq 0 && continue
  snapshot="$citc_root/.snapshot/$rev/google3/$citc_path"
  if cmp -s -- "$file" "$snapshot"
  then
    continue
  fi
  echo "$rev:$snapshot:Snapshot $rev ($date $time, $size bytes.)"
done
]]
	local command = script:format(citc_root:gsub("/google3$", ""), citc_path)

	async.run_shell({
		command = command,
		on_exit = function(obj)
			if obj.code ~= 0 then
				callback({}, nil)
				return
			end

			local entries = {}
			for _, line in ipairs(vim.split(obj.stdout or "", "\n", { plain = true })) do
				if line ~= "" then
					local parts = vim.split(line, ":", { plain = true })
					local rev = parts[1]
					local display = table.concat(vim.list_slice(parts, 3), ":")
					table.insert(entries, {
						uri = "vcs://google3/snapshot/" .. rev .. "/" .. citc_path,
						display = display,
						rev = rev,
					})
				end
			end

			local baseline_path = citc_root:gsub("/google3$", "") .. "/.citc/baseline/google3/" .. citc_path
			if vim.fn.filereadable(baseline_path) == 1 then
				table.insert(entries, {
					uri = "vcs://google3/baseline/baseline/" .. citc_path,
					display = "Baseline",
					rev = "baseline",
				})
			else
				table.insert(entries, {
					uri = "vcs://google3/baseline/empty/" .. citc_path,
					display = "Baseline (Empty)",
					rev = "empty",
				})
			end

			callback(entries, nil)
		end,
	})
end

local function get_google3_file_content(file, rev, callback)
	rev = rev or "baseline"
	if rev == "empty" then
		callback("")
		return
	end

	local citc_root, citc_path
	if vim.startswith(file, "/google/src/cloud/") then
		citc_root = get_citc_root(vim.fs.dirname(file))
		if not citc_root then
			callback(nil)
			return
		end
		citc_path = file:sub(#citc_root + 2)
	else
		local main_buf = vim.g.vcs_main_buf
		if main_buf and vim.api.nvim_buf_is_valid(main_buf) then
			citc_root = vim.b[main_buf].citc_root
		end
		if not citc_root then
			callback(nil)
			return
		end
		citc_path = file:gsub("^/", "")
	end
	local abs_path
	if rev == "baseline" then
		abs_path = citc_root:gsub("/google3$", "") .. "/.citc/baseline/google3/" .. citc_path
	else
		abs_path = citc_root:gsub("/google3$", "") .. "/.snapshot/" .. rev .. "/google3/" .. citc_path
	end

	if vim.fn.filereadable(abs_path) == 1 then
		local lines = vim.fn.readfile(abs_path)
		callback(table.concat(lines, "\n"))
	else
		callback(nil)
	end
end

function g4.get_primary_states(file, callback)
	get_google3_primary_states(file, callback)
end

function g4.get_secondary_states(file, callback)
	get_google3_secondary_states(file, callback)
end

function g4.get_file_content(file, rev, callback)
	get_google3_file_content(file, rev, callback)
end

function M.setup()
	vcs.register("g4", g4)
	vcs.register("google3", {
		detect = g4.detect,
		get_modified_files = g4.get_modified_files,
		get_primary_states = get_google3_primary_states,
		get_secondary_states = get_google3_secondary_states,
		get_file_content = get_google3_file_content,
	})

	local hg = vcs.get_provider("hg")
	if not hg then
		hg = require("vcs.hg")
		vcs.register("hg", hg)
	end

	if hg then
		local original_get_secondary = hg.get_secondary_states
		hg.get_secondary_states = function(file, callback)
			file = vim.fn.fnamemodify(file, ":p")
			if vim.startswith(file, prefix) then
				get_google3_secondary_states(file, callback)
			elseif original_get_secondary then
				original_get_secondary(file, callback)
			else
				callback({}, nil)
			end
		end
	end
end

return M
