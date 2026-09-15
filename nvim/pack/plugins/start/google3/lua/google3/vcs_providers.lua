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

function g4.get_modified_files(dir)
	local obj = async.exec("g4 whatsout", { text = true })
	if obj.code ~= 0 then
		return {}
	end
	local files = {}
	for _, line in ipairs(vim.split(obj.stdout or "", "\n", { plain = true })) do
		if line ~= "" and not line:match("^#") then
			table.insert(files, line)
		end
	end
	return files
end

local function get_google3_primary_states(file)
	file = vim.fn.fnamemodify(file, ":p")
	local citc_root = get_citc_root(vim.fs.dirname(file))
	if not citc_root then
		return {}, nil
	end
	local citc_path = file:sub(#citc_root + 2)
	local baseline_path = citc_root:gsub("/google3$", "") .. "/.citc/baseline/google3/" .. citc_path

	local entries = {}
	if vim.fn.filereadable(baseline_path) == 1 then
		table.insert(entries, {
			uri = string.format("vcs://google3/diff/baseline%s", file),
			display = "Baseline",
			rev = "baseline",
		})
	else
		table.insert(entries, {
			uri = string.format("vcs://google3/diff/empty%s", file),
			display = "Baseline (Empty)",
			rev = "empty",
		})
	end
	return entries, nil
end

local function get_google3_secondary_states(file)
	file = vim.fn.fnamemodify(file, ":p")
	local citc_root = get_citc_root(vim.fs.dirname(file))
	if not citc_root then
		return {}, nil
	end
	local citc_path = file:sub(#citc_root + 2)

	local script = [[
citc_root=%s
citc_path=%s
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
	local command = script:format(vim.fn.shellescape(citc_root:gsub("/google3$", "")), vim.fn.shellescape(citc_path))
	local obj = async.exec(command, { text = true })
	if obj.code ~= 0 then
		return {}, nil
	end

	local entries = {}
	for _, line in ipairs(vim.split(obj.stdout or "", "\n", { plain = true })) do
		if line ~= "" then
			local parts = vim.split(line, ":", { plain = true })
			local rev = parts[1]
			local display = table.concat(vim.list_slice(parts, 3), ":")
			table.insert(entries, {
				uri = string.format("vcs://google3/diff/%s%s", rev, file),
				display = display,
				rev = rev,
			})
		end
	end

	local baseline_path = citc_root:gsub("/google3$", "") .. "/.citc/baseline/google3/" .. citc_path
	if vim.fn.filereadable(baseline_path) == 1 then
		table.insert(entries, {
			uri = string.format("vcs://google3/diff/baseline%s", file),
			display = "Baseline",
			rev = "baseline",
		})
	else
		table.insert(entries, {
			uri = string.format("vcs://google3/diff/empty%s", file),
			display = "Baseline (Empty)",
			rev = "empty",
		})
	end

	return entries, nil
end

local function get_google3_file_content(file, rev)
	rev = rev or "baseline"
	if rev == "empty" then
		return ""
	end

	local citc_root, citc_path
	if vim.startswith(file, "/google/src/cloud/") then
		citc_root = get_citc_root(vim.fs.dirname(file))
		if not citc_root then
			return nil
		end
		citc_path = file:sub(#citc_root + 2)
	else
		local main_buf = vim.g.vcs_main_buf
		if main_buf and vim.api.nvim_buf_is_valid(main_buf) then
			citc_root = vim.b[main_buf].citc_root
		end
		if not citc_root then
			return nil
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
		return table.concat(lines, "\n")
	else
		local obj = async.exec("g4 ignores -i " .. vim.fn.shellescape(file), { text = true })
		return obj.code == 0 and (obj.stdout or ""):match("not ignored") and "" or nil
	end
end

function g4.get_primary_states(file)
	return get_google3_primary_states(file)
end

function g4.get_secondary_states(file)
	return get_google3_secondary_states(file)
end

function g4.get_file_content(file, rev)
	return get_google3_file_content(file, rev)
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
		hg.get_secondary_states = function(file)
			file = vim.fn.fnamemodify(file, ":p")
			if vim.startswith(file, prefix) then
				return get_google3_secondary_states(file)
			elseif original_get_secondary then
				return original_get_secondary(file)
			else
				return {}, nil
			end
		end
	end

	local jj = vcs.get_provider("jj")
	if not jj then
		jj = require("vcs.jj")
		vcs.register("jj", jj)
	end

	if jj then
		local original_get_secondary = jj.get_secondary_states
		jj.get_secondary_states = function(file)
			file = vim.fn.fnamemodify(file, ":p")
			if vim.startswith(file, prefix) then
				return get_google3_secondary_states(file)
			elseif original_get_secondary then
				return original_get_secondary(file)
			else
				return {}, nil
			end
		end
	end
end

return M
